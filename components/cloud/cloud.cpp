#include "cloud.hpp"

#include "cloud_json.hpp"
#include "cloud_portal.hpp"
#include "power.hpp"
#include "settings.hpp"
#include "shell.hpp"
#include "storage.hpp"

#include "esp_check.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <strings.h>   // strcasecmp, for the case-insensitive Location match
#include <sys/time.h>  // struct timeval, the SNTP sync callback's argument

namespace thermopost::cloud {
namespace {

constexpr const char* kTag = "cloud";

// The JSON layer restates the storage flag bits so it can stay IDF-free and
// host-testable. Assert the two agree, so a bit added on one side cannot
// silently desync the wire format from the on-disk record.
static_assert(json::bits::kFlagAlarmHigh   == storage::kFlagAlarmHigh,   "flag bit drift");
static_assert(json::bits::kFlagAlarmLow    == storage::kFlagAlarmLow,    "flag bit drift");
static_assert(json::bits::kFlagSensorFault == storage::kFlagSensorFault, "flag bit drift");
static_assert(json::kInvalidTempC100 == storage::kInvalidTempC100, "temp sentinel drift");

// --- NVS (namespace "cloud"): endpoint, token, and the upload high-water mark.
// Single-letter keys keep an `nvs` dump readable, matching settings/ and
// storage/. Wi-Fi credentials are deliberately NOT here — the driver keeps
// those itself, so there is exactly one copy of them. ---
constexpr const char* kNvsNamespace = "cloud";
constexpr const char* kNvsKeyUrl    = "u";
constexpr const char* kNvsKeyToken  = "k";
constexpr const char* kNvsKeyHwm    = "hwm";

// Ships in the firmware so a fresh post syncs with only the /exec URL set — the
// token is the sole other input and most deployments keep the default. It is a
// soft gate, not a secret: it is readable in the binary and crosses the wire in
// the clear. Must fit in kTokenMax - 1.
constexpr const char* kDefaultToken = "Thermo Post (c) 2026 Itay Keren";

// --- Upload tunables -------------------------------------------------------
// One hour of history per POST at the default one-minute cadence, which is the
// unit a backlog is naturally thought about in ("it was offline overnight" =
// about eight batches). Roughly 8 KB of JSON, well inside what Apps Script
// accepts, and it drains a full day's backlog in two dozen round trips instead
// of seventy.
constexpr size_t kBatchSize = 60;

// --- Upload cadence ---------------------------------------------------------
// settings::upload_every_min is a floor on how often the radio comes up, not a
// timer of its own: nothing here wakes on a clock, so the effective cadence is the
// first record to land on or after the deadline. That is the right shape for a post
// that will eventually be asleep between records — there is no clock to run.
constexpr int64_t kUsPerMs  = 1000;
constexpr int64_t kMsPerMin = 60'000;

constexpr uint32_t   kSyncTaskStack = 12288;  // the mbedTLS handshake needs the headroom
constexpr UBaseType_t kSyncTaskPrio = 3;      // same as the sampler
constexpr BaseType_t  kSyncTaskCore = 0;      // off the LVGL core (1)

// Apps Script cold-starts a container on the first request after an idle spell,
// which genuinely can take ten seconds. A shorter timeout turns that into a
// failure and a retry, which cold-starts it again.
constexpr int kHttpTimeoutMs = 15000;

// Failure backoff, separate from the Wi-Fi one: this is "the endpoint is
// unhappy" (a bad URL, a revoked deployment, Google having a bad afternoon),
// where the link is fine and the records are safe on disk.
constexpr uint32_t kBackoffMinMs  = 5'000;
constexpr uint32_t kBackoffMaxMs  = 300'000;
constexpr uint32_t kBackoffFactor = 2;

// How much of the response to keep. All we ask of it is whether it contains
// "ok":true, plus enough of the rest to be worth logging when it does not.
constexpr size_t kRespCapBytes = 256;

// Event bits driving the sync task.
constexpr EventBits_t kBitGotIp     = 1u << 0;
constexpr EventBits_t kBitNewRecord = 1u << 1;
constexpr EventBits_t kBitReconnect = 1u << 2;  // the backoff timer came due
// A record the cadence below is not allowed to hold back — see notify_new_record().
constexpr EventBits_t kBitUrgent    = 1u << 3;

// --- mDNS -------------------------------------------------------------------
// What makes a fleet usable: a laptop walks every post by name instead of
// hunting addresses in a DHCP lease table. The service record is what a browser
// finds; the hostname is what someone types.
constexpr const char* kMdnsService = "_http";
constexpr const char* kMdnsProto   = "_tcp";
constexpr uint16_t    kMdnsPort    = 80;

// --- Time -------------------------------------------------------------------
// The public pool rather than a vendor's: this device has no relationship with
// anyone whose NTP server it should prefer, and the pool is what espressopost
// used. One server is enough — the module re-syncs periodically and across
// reconnects on its own.
constexpr const char* kSntpServer = "pool.ntp.org";

// Below this the system clock has never been set (see ui_chrome.cpp, which uses
// the same floor to decide whether to draw a clock at all). Only used to log the
// time we just acquired, so a duplicate of that constant would be worse than the
// coupling — this one is not allowed to disagree with the screen.
constexpr std::time_t kEpochValidFrom = 1'700'000'000;

// --- Reconnect policy -------------------------------------------------------
// espressopost gave up after six attempts and stayed down until someone did
// something about it. That was reasonable for a device on a kitchen counter with
// its owner standing at it. A Thermo Post sits on a tank, unattended, and the AP
// it wants may be rebooting, or out of range while someone moves a pallet, or
// down for an afternoon. So retries here never stop — they only slow down, from
// a couple of seconds up to five minutes.
//
// Nothing is lost by waiting: records keep being written locally the whole time,
// and both the clock and the upload backlog recover on their own the moment the
// link comes back. Giving up would only mean a post that is silently offline
// until it is power-cycled, which is the one failure mode a fleet cannot afford.
constexpr uint32_t kRetryMinMs  = 2'000;
constexpr uint32_t kRetryMaxMs  = 300'000;  // five minutes
constexpr uint32_t kRetryFactor = 2;

// Consecutive failures before the UI stops saying "connecting" and says
// "offline". Two quick retries cover the ordinary AP glitch, which resolves
// faster than anyone reads the screen; past that, the honest word is offline.
constexpr int kRetriesBeforeOffline = 3;

// --- State (guarded by s_mutex unless noted) --------------------------------
SemaphoreHandle_t  s_mutex  = nullptr;
EventGroupHandle_t s_events = nullptr;
TaskHandle_t       s_task   = nullptr;
esp_netif_t*       s_netif  = nullptr;
esp_timer_handle_t s_retry_timer = nullptr;

WifiState s_wifi        = WifiState::Disabled;
SyncState s_sync        = SyncState::Idle;
bool      s_time_synced = false;
int8_t    s_rssi        = 0;
char      s_ssid[kSsidMax]  = {};
char      s_ip[kIpTextMax]  = {};

char      s_url[kUrlMax]     = {};
char      s_token[kTokenMax] = {};
uint32_t  s_hwm        = 0;       // absolute index of the next record to upload
esp_err_t s_last_error = ESP_OK;

// When the queue may next drain. Zero so the first record after boot goes at once,
// whatever the cadence — a post that has just come up should prove it can reach the
// Sheet rather than sitting on that question for an hour. Read and written only by
// the sync task, so it needs no lock.
int64_t   s_next_upload_ms = 0;

// The dedupe key the Sheet groups on: this board's MAC as lowercase hex. Read
// from efuse once at init — stable for the life of the hardware, and unlike the
// post's name it is never edited, which is the whole reason it and not the name
// is what identifies a row's origin.
char s_device_id[13] = {};

// The post's name slugified for mDNS. Kept alongside the address because the
// Network screen shows both — mDNS is unreliable on some networks, and a
// device that can only say its .local name has no fallback to offer.
char s_host[kHostMax] = {};

uint32_t s_retry_delay_ms = kRetryMinMs;
int      s_retry_count    = 0;
bool     s_started        = false;  // esp_wifi_start() has been called
bool     s_sntp_inited    = false;  // esp_netif_sntp_init() has run (once)
bool     s_mdns_up        = false;  // mdns_init() has run (once)

struct Lock {
  bool ok;
  explicit Lock(TickType_t t = portMAX_DELAY) : ok(xSemaphoreTake(s_mutex, t) == pdTRUE) {}
  ~Lock() { if (ok) xSemaphoreGive(s_mutex); }
};

void set_wifi_state(WifiState w) { Lock l; s_wifi = w; }
void set_sync_state(SyncState y) { Lock l; s_sync = y; }

bool is_connected() { Lock l; return s_wifi == WifiState::Connected; }
bool configured()   { Lock l; return s_url[0] != '\0' && s_token[0] != '\0'; }

// "Already provisioned?" is just "are STA credentials stored?". The driver keeps
// them in NVS for us (WIFI_STORAGE_FLASH), and esp_wifi_get_config reads that
// copy — valid any time after esp_wifi_init, including before esp_wifi_start.
bool has_stored_creds() {
  wifi_config_t cfg = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
  return cfg.sta.ssid[0] != 0;
}

// --- Endpoint config (NVS) --------------------------------------------------

void load_config() {
  // Seed the default token first, so it stands whether NVS has no cloud
  // namespace at all (a fresh device) or simply no stored token. A user-set
  // token overwrites it below.
  std::snprintf(s_token, sizeof(s_token), "%s", kDefaultToken);
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  size_t len = sizeof(s_url);
  if (nvs_get_str(h, kNvsKeyUrl, s_url, &len) != ESP_OK) s_url[0] = '\0';
  len = sizeof(s_token);
  if (nvs_get_str(h, kNvsKeyToken, s_token, &len) != ESP_OK) {
    std::snprintf(s_token, sizeof(s_token), "%s", kDefaultToken);
  }
  uint32_t hwm = 0;
  if (nvs_get_u32(h, kNvsKeyHwm, &hwm) == ESP_OK) s_hwm = hwm;
  nvs_close(h);
}

void persist_hwm(uint32_t hwm) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u32(h, kNvsKeyHwm, hwm);
  nvs_commit(h);
  nvs_close(h);
}

bool persist_str(const char* nvs_key, const char* val, char* dst, size_t dst_sz) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  const esp_err_t e = nvs_set_str(h, nvs_key, val);
  if (e == ESP_OK) nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK) return false;
  Lock l;
  std::snprintf(dst, dst_sz, "%s", val);
  return true;
}

// Read once at init. esp_read_mac reads efuse, so this works before the radio
// has ever been started and gives the same answer on every boot.
void compute_device_id() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  std::snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// --- Identity on the network ------------------------------------------------

// Lowercase the name, keep [a-z0-9], turn every run of anything else into a
// single dash, and trim the ends: "North Fermenter #3" becomes
// "north-fermenter-3". A hostname carries neither spaces nor punctuation, which
// is the third reason the name is ASCII-only (settings.hpp has the other two).
void slugify(const char* in, char* out, size_t cap) {
  size_t o = 0;
  bool prev_dash = true;  // starts true so a leading run produces no dash
  for (const char* p = in; *p != '\0' && o + 1 < cap; ++p) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[o++] = c;
      prev_dash = false;
    } else if (!prev_dash) {
      out[o++] = '-';
      prev_dash = true;
    }
  }
  while (o > 0 && out[o - 1] == '-') --o;
  out[o] = '\0';
  // A name of nothing but punctuation is legal (set_name only strips non-ASCII)
  // and slugifies to nothing. The device id always yields a usable label.
  if (o == 0) std::snprintf(out, cap, "post-%s", s_device_id);
}

// Recompute the hostname from the current name and push it to mDNS if it is up.
// Called at every point the name can change, which is exactly the config form.
void apply_identity() {
  const settings::Settings cfg = settings::get();
  char slug[kHostMax] = {};
  slugify(cfg.name, slug, sizeof(slug));
  { Lock l; std::snprintf(s_host, sizeof(s_host), "%s", slug); }
  if (!s_mdns_up) return;
  mdns_hostname_set(slug);
  mdns_instance_name_set(cfg.name);
}

// Brought up on the first GOT_IP and left up: the responder needs an interface
// with an address, and re-registering on every reconnect would only churn the
// same records.
void start_mdns() {
  if (s_mdns_up) return;
  const esp_err_t e = mdns_init();
  if (e != ESP_OK) {
    ESP_LOGW(kTag, "mDNS init failed: %s; the post is reachable by IP only",
             esp_err_to_name(e));
    return;
  }
  s_mdns_up = true;
  apply_identity();
  mdns_service_add(nullptr, kMdnsService, kMdnsProto, kMdnsPort, nullptr, 0);
  ESP_LOGI(kTag, "config form reachable at http://%s.local/", s_host);
}

// --- Reconnect --------------------------------------------------------------

void schedule_retry(uint32_t delay_ms) {
  if (s_retry_timer == nullptr) return;
  esp_timer_stop(s_retry_timer);  // harmless if not armed; makes this a reschedule
  esp_timer_start_once(s_retry_timer, static_cast<uint64_t>(delay_ms) * 1000);
}

// A one-shot timer rather than a task, because the only thing a retry loop does
// is sleep and a task to hold a sleeping stack is 4 KB spent on nothing.
//
// **This callback sets a bit and returns, and that is not a style choice.**
// esp_timer dispatches every callback from one shared task, and on this device
// the other timer on it is the LVGL tick (display.cpp). Calling esp_wifi_connect()
// here — which blocks until the Wi-Fi task takes the request, and can sit there
// for seconds while the radio scans — stops lv_tick_inc() for exactly that long.
// LVGL then has no clock: the screen stops repainting and, because the input
// device is polled by an LVGL timer, touch stops being read at all. The symptom
// is a frozen screen with dead buttons for the whole duration of a connection
// attempt, which is a UI bug caused entirely by where the network call was made.
// Nothing that can block belongs in an esp_timer callback.
void on_retry_timer(void* /*arg*/) {
  if (s_events != nullptr) xEventGroupSetBits(s_events, kBitReconnect);
}

// --- Time sync (SNTP) -------------------------------------------------------

// Called from the SNTP task once a server reply lands; the module has already
// pushed the time into the system clock via settimeofday, so there is nothing to
// do but record that it happened. Everything that reads the clock — the screens'
// clock line, every record's epoch_s — goes straight to time(), so no value has
// to be handed anywhere from here.
void on_sntp_sync(struct timeval* /*tv*/) {
  { Lock l; s_time_synced = true; }
  const std::time_t now = std::time(nullptr);
  if (now < kEpochValidFrom) return;  // a sync callback with an unset clock; say nothing
  std::tm tm = {};
  gmtime_r(&now, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  ESP_LOGI(kTag, "clock set: %s UTC", buf);
}

// Brought up once, on the first GOT_IP. It re-syncs itself periodically and
// across reconnects afterwards, so this runs a single time per boot.
void start_sntp() {
  if (s_sntp_inited) return;
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(kSntpServer);
  cfg.sync_cb = on_sntp_sync;
  const esp_err_t e = esp_netif_sntp_init(&cfg);
  if (e != ESP_OK) {
    ESP_LOGW(kTag, "SNTP init failed: %s", esp_err_to_name(e));
    return;
  }
  s_sntp_inited = true;
  ESP_LOGI(kTag, "SNTP started (%s)", kSntpServer);
}

// --- HTTP -------------------------------------------------------------------

struct RespCtx {
  char   buf[kRespCapBytes];
  size_t len;
  char   location[512];  // a redirect's Location header, captured to follow by hand
};

esp_err_t http_event(esp_http_client_event_t* evt) {
  auto* ctx = static_cast<RespCtx*>(evt->user_data);
  if (ctx == nullptr) return ESP_OK;
  if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (evt->header_key != nullptr && strcasecmp(evt->header_key, "Location") == 0) {
      std::snprintf(ctx->location, sizeof(ctx->location), "%s", evt->header_value);
    }
  } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
    // Keep only the prefix; the rest is discarded rather than buffered, because
    // an endpoint answering with a megabyte of HTML must not be able to exhaust
    // the heap of a device whose actual job is elsewhere.
    if (ctx->len + 1 < sizeof(ctx->buf)) {
      const size_t space = sizeof(ctx->buf) - 1 - ctx->len;
      const size_t n = std::min(static_cast<size_t>(evt->data_len), space);
      std::memcpy(ctx->buf + ctx->len, evt->data, n);
      ctx->len += n;
      ctx->buf[ctx->len] = '\0';
    }
  }
  return ESP_OK;
}

// POST `body` to `url`, writing the HTTP status and a truncated response body
// back through the out-parameters.
//
// Apps Script's /exec answers a POST with a 302 to a script.googleusercontent.com
// echo URL that serves doPost's return value and accepts GET only. The redirect
// has to be followed manually as a GET: esp_http_client's automatic redirect
// preserves the method, so the echo host answers 405 and the upload looks like a
// failure when it in fact already succeeded. doPost ran with the body on the
// first hop, so re-sending it would double-write; the echo URL just reports.
esp_err_t http_post(const char* url, const std::string& body, int* out_status, RespCtx* resp) {
  esp_http_client_config_t cfg = {};
  cfg.url                   = url;
  cfg.method                = HTTP_METHOD_POST;
  cfg.timeout_ms            = kHttpTimeoutMs;
  cfg.crt_bundle_attach     = esp_crt_bundle_attach;
  cfg.event_handler         = http_event;
  cfg.user_data             = resp;
  cfg.disable_auto_redirect = true;  // see above

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return ESP_FAIL;

  esp_http_client_set_header(c, "Content-Type", "application/json");
  esp_http_client_set_post_field(c, body.data(), static_cast<int>(body.size()));

  esp_err_t err = esp_http_client_perform(c);
  int status = (err == ESP_OK) ? esp_http_client_get_status_code(c) : 0;

  if (err == ESP_OK && (status == 301 || status == 302 || status == 303) &&
      resp->location[0] != '\0') {
    // The 302's body is the throwaway redirect page; clear it so what we test
    // for "ok":true is the echo URL's answer and not this.
    resp->len = 0;
    resp->buf[0] = '\0';
    esp_http_client_set_url(c, resp->location);
    esp_http_client_set_method(c, HTTP_METHOD_GET);
    esp_http_client_set_post_field(c, nullptr, 0);
    err = esp_http_client_perform(c);
    if (err == ESP_OK) status = esp_http_client_get_status_code(c);
  }

  if (err == ESP_OK) *out_status = status;
  esp_http_client_cleanup(c);
  return err;
}

// --- The upload queue -------------------------------------------------------

enum class BatchResult { Ok, UpToDate, Failed };

json::RecordJson to_json(const storage::TempRecord& r, uint32_t index) {
  json::RecordJson j{};
  j.index     = index;
  j.version   = r.version;
  j.flags     = r.flags;
  j.temp_c100 = r.temp_c100;
  j.min_c100  = r.min_c100;
  j.max_c100  = r.max_c100;
  j.epoch_s   = r.epoch_s;
  j.uptime_s  = r.uptime_s;
  return j;
}

// Send the next batch of records above the high-water mark.
//
// Records are read into a heap buffer, turned into the payload string, and the
// buffers freed before the POST — so storage's lock is long released and ~2 KB
// is back on the heap by the time the TLS handshake wants its stack.
BatchResult upload_batch() {
  uint32_t hwm;
  char url[kUrlMax];
  json::Envelope env;
  {
    Lock l;
    hwm = s_hwm;
    std::snprintf(url, sizeof(url), "%s", s_url);
    env.token  = s_token;
    env.device = s_device_id;
  }
  // The name is read fresh every batch rather than cached, which is what makes a
  // rename retag the queued backlog. That is the intended behaviour: renaming a
  // post should relabel that post, not leave a seam in the middle of its history.
  env.name = settings::get().name;

  if (hwm >= storage::next_index()) return BatchResult::UpToDate;

  auto* recs = static_cast<storage::TempRecord*>(
      std::malloc(sizeof(storage::TempRecord) * kBatchSize));
  if (recs == nullptr) return BatchResult::Failed;

  // The read reports where it actually started. It can be later than we asked
  // for: rotation drops the oldest block once the log passes its cap, and a post
  // that was offline for months can have records rotated away before they were
  // ever sent. Trusting `hwm` for the indices would then label every row with a
  // number seventy days off. Taking the reported start instead makes the gap
  // simply a gap — permanent, logged once, and not something that stalls every
  // later record behind a record that no longer exists.
  uint32_t first = hwm;
  const size_t n = storage::read_from(hwm, recs, kBatchSize, &first);
  if (n == 0) { std::free(recs); return BatchResult::UpToDate; }
  if (first > hwm) {
    ESP_LOGW(kTag, "rotation outran the uploader: %lu records dropped unsent (%lu..%lu)",
             static_cast<unsigned long>(first - hwm),
             static_cast<unsigned long>(hwm), static_cast<unsigned long>(first - 1));
  }

  std::string payload;
  {
    auto* jbuf = static_cast<json::RecordJson*>(std::malloc(sizeof(json::RecordJson) * n));
    if (jbuf == nullptr) { std::free(recs); return BatchResult::Failed; }
    for (size_t i = 0; i < n; ++i) jbuf[i] = to_json(recs[i], first + static_cast<uint32_t>(i));
    payload = json::build_payload(env, jbuf, n);
    std::free(jbuf);
  }
  std::free(recs);

  RespCtx resp{};
  int http_status = 0;
  const esp_err_t err = http_post(url, payload, &http_status, &resp);

  // ContentService answers 200 whatever happened inside the script, so "did it
  // work" is the body's ok flag and not the status code — a rejected token comes
  // back as a perfectly healthy 200 with ok:false.
  const bool ok = (err == ESP_OK && http_status == 200 &&
                   std::strstr(resp.buf, "\"ok\":true") != nullptr);
  if (!ok) {
    { Lock l; s_last_error = (err != ESP_OK) ? err : ESP_FAIL; }
    ESP_LOGW(kTag, "upload failed: err=%s http=%d resp=%.64s",
             esp_err_to_name(err), http_status, resp.buf);
    return BatchResult::Failed;
  }

  uint32_t new_hwm;
  {
    Lock l;
    // From `first`, not from the old mark: if rotation skipped us forward, the
    // records in between are gone and the mark has to move past them too.
    s_hwm = first + static_cast<uint32_t>(n);
    new_hwm = s_hwm;
    s_last_error = ESP_OK;
  }
  persist_hwm(new_hwm);
  ESP_LOGI(kTag, "uploaded %u records, hwm=%lu/%lu", static_cast<unsigned>(n),
           static_cast<unsigned long>(new_hwm),
           static_cast<unsigned long>(storage::next_index()));
  return BatchResult::Ok;
}

int64_t now_ms() { return esp_timer_get_time() / kUsPerMs; }

// Which of the two cadences applies right now.
//
// Keyed on the idle tier, not on the supply. power::on_external_power() is a
// heuristic that cannot tell a just-unplugged pack from a charging one for minutes;
// the tier is a fact. It is also the better question — what makes a radio session
// expensive is the post being otherwise idle, not where its current comes from.
//
// Dark rather than Asleep, so the behaviour exists and can be watched on a post that
// only blanks its panel, long before one that sleeps. Safe before power::init(),
// which answers Active.
uint16_t cadence_min() {
  const settings::Settings cfg = settings::get();
  return power::tier() >= power::Tier::Dark ? cfg.upload_dark_min : cfg.upload_every_min;
}

bool upload_due() {
  const uint16_t cadence = cadence_min();
  if (cadence == 0) return true;
  const int64_t now = now_ms();
  const int64_t cadence_ms = static_cast<int64_t>(cadence) * kMsPerMin;
  // Pulls a pending deadline in whenever the applicable cadence is shorter than what
  // is outstanding — an edit from the menu, and also a post waking up, where the
  // dark hour gives way to the lit figure. So walking up to a post that has been
  // asleep drains its backlog promptly rather than on the schedule it went dark
  // under, which is what someone standing in front of it expects. Lengthening is left
  // alone: the next drain is what pushes the deadline out.
  if (s_next_upload_ms > now + cadence_ms) s_next_upload_ms = now + cadence_ms;
  return now >= s_next_upload_ms;
}

// Advanced from the previous deadline, not from the moment the drain finished.
// Stamping it from the end would push every cycle later by however long the
// association and the handshake took, until a whole cadence had been swallowed. A
// deadline already in the past — a post that spent the night offline — is resynced
// to now instead of being caught up one cadence at a time.
void advance_upload_deadline() {
  const uint16_t cadence = cadence_min();
  if (cadence == 0) { s_next_upload_ms = 0; return; }
  const int64_t cadence_ms = static_cast<int64_t>(cadence) * kMsPerMin;
  const int64_t now = now_ms();
  s_next_upload_ms += cadence_ms;
  if (s_next_upload_ms <= now) s_next_upload_ms = now + cadence_ms;
}

void sync_task(void* /*arg*/) {
  ESP_LOGI(kTag, "sync task started (stack %u, core %d)",
           static_cast<unsigned>(kSyncTaskStack), static_cast<int>(kSyncTaskCore));
  uint32_t backoff = kBackoffMinMs;
  for (;;) {
    const EventBits_t bits =
        xEventGroupWaitBits(s_events,
                            kBitGotIp | kBitNewRecord | kBitReconnect | kBitUrgent,
                            /*clear=*/pdTRUE, /*waitAll=*/pdFALSE, portMAX_DELAY);

    // The backoff timer came due. Done here rather than in the timer callback
    // because esp_wifi_connect() blocks — see on_retry_timer() for what that
    // costs when it runs on the task that also drives the LVGL tick.
    if (bits & kBitReconnect) {
      if (has_stored_creds()) {
        esp_wifi_connect();  // errors surface as the next DISCONNECTED event
      } else {
        set_wifi_state(WifiState::Disabled);
      }
    }

    // Hold the queue back until the cadence says the radio may come up. After the
    // reconnect above, deliberately: a post whose link is down still gets its retry
    // on schedule, because a cadence that also gated reconnecting would leave a post
    // that dropped its Wi-Fi offline until the next drain was due.
    //
    // Nothing is lost by returning to the wait — the bits are cleared, but the next
    // record sets them again, and records are what this cadence is measured in.
    if ((bits & kBitUrgent) == 0 && !upload_due()) continue;

    // Drain everything above the mark, then go back to waiting. Both exits are
    // deliberate: up-to-date means there is nothing to do until something
    // happens, and a lost link or a cleared endpoint means there is nowhere to
    // send it — in both cases the next event bit wakes us.
    for (;;) {
      if (!is_connected() || !configured()) { set_sync_state(SyncState::Idle); break; }
      set_sync_state(SyncState::Syncing);
      const BatchResult r = upload_batch();
      if (r == BatchResult::Ok) { backoff = kBackoffMinMs; continue; }
      if (r == BatchResult::UpToDate) {
        set_sync_state(SyncState::Idle);
        backoff = kBackoffMinMs;
        // Only here, where the queue is genuinely empty. A drain that ended because
        // the link dropped leaves the deadline in the past on purpose, so the next
        // wake retries instead of waiting out another cadence.
        advance_upload_deadline();
        break;
      }
      set_sync_state(SyncState::Backoff);
      vTaskDelay(pdMS_TO_TICKS(backoff));
      backoff = std::min(backoff * kBackoffFactor, kBackoffMaxMs);
    }
  }
}

// --- Wi-Fi events -----------------------------------------------------------

void on_event(void* /*arg*/, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    // Fires once per esp_wifi_start(). Auto-connect only if credentials are
    // stored; an unprovisioned post sits idle rather than scanning forever.
    if (has_stored_creds()) {
      set_wifi_state(WifiState::Connecting);
      esp_wifi_connect();
    } else {
      set_wifi_state(WifiState::Disabled);
    }
    return;
  }

  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
    { Lock l; s_rssi = 0; s_ssid[0] = '\0'; s_ip[0] = '\0'; }

    // Credentials were erased (forget) — settle to idle instead of retrying.
    if (!has_stored_creds()) { set_wifi_state(WifiState::Disabled); return; }

    uint32_t delay_ms;
    int count;
    {
      Lock l;
      count = ++s_retry_count;
      delay_ms = s_retry_delay_ms;
      s_retry_delay_ms = std::min(s_retry_delay_ms * kRetryFactor, kRetryMaxMs);
      s_wifi = count <= kRetriesBeforeOffline ? WifiState::Connecting : WifiState::Offline;
    }
    // Log the first few and then only on the way up to the ceiling, so a post
    // that spends a night offline does not fill the log with the same line.
    if (count <= kRetriesBeforeOffline || delay_ms >= kRetryMaxMs) {
      ESP_LOGW(kTag, "disconnected (reason %d), attempt %d, retry in %lums",
               ev != nullptr ? ev->reason : 0, count, static_cast<unsigned long>(delay_ms));
    }
    schedule_retry(delay_ms);
    return;
  }

  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* ev = static_cast<ip_event_got_ip_t*>(data);
    char ip[kIpTextMax] = {};
    if (ev != nullptr) esp_ip4addr_ntoa(&ev->ip_info.ip, ip, sizeof(ip));

    int8_t rssi = 0;
    char ssid[kSsidMax] = {};
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      rssi = ap.rssi;
      std::memcpy(ssid, ap.ssid,
                  strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(ssid) - 1));
    }
    {
      Lock l;
      s_wifi = WifiState::Connected;
      s_retry_count = 0;
      s_retry_delay_ms = kRetryMinMs;
      s_rssi = rssi;
      std::memcpy(s_ssid, ssid, sizeof(s_ssid));
      std::memcpy(s_ip, ip, sizeof(s_ip));
    }
    if (s_retry_timer != nullptr) esp_timer_stop(s_retry_timer);
    ESP_LOGI(kTag, "connected to '%s' as %s, rssi %d", ssid, ip, rssi);

    // Provisioning is over the moment the post has an address of its own. The
    // setup AP and the DNS hijack exist only to get it here; the config form
    // they were serving carries on over the LAN.
    if (portal::ap_active()) {
      portal::stop_ap();
      esp_wifi_set_mode(WIFI_MODE_STA);
    }
    start_mdns();
    start_sntp();  // correct the clock now that we are online (idempotent)
    xEventGroupSetBits(s_events, kBitGotIp);
    return;
  }
}

// --- Serial console ---------------------------------------------------------

const char* wifi_word(WifiState w) {
  switch (w) {
    case WifiState::Disabled:   return "disabled";
    case WifiState::Connecting: return "connecting";
    case WifiState::Connected:  return "connected";
    case WifiState::Offline:    return "offline";
  }
  return "?";
}

const char* sync_word(SyncState y) {
  switch (y) {
    case SyncState::Idle:    return "idle";
    case SyncState::Syncing: return "syncing";
    case SyncState::Backoff: return "backoff";
  }
  return "?";
}

void print_status() {
  const Status st = status();
  std::printf("wifi:   %s\n", wifi_word(st.wifi));
  if (st.wifi == WifiState::Connected) {
    std::printf("ssid:   %s\nip:     %s\nrssi:   %d dBm\n", st.ssid, st.ip, st.rssi_dbm);
  }
  if (st.provisioning) std::printf("setup:  AP up, form at http://192.168.4.1/\n");
  std::printf("host:   %s.local\n", st.host);
  std::printf("clock:  %s\n", st.time_synced ? "synced" : "not set");
  { Lock l; std::printf("device: %s\n", s_device_id); }
  std::printf("name:   %s\n", settings::get().name);
  std::printf("url:    %s\n", st.endpoint_set ? "set" : "unset");
  std::printf("sync:   %s\n", sync_word(st.sync));
  std::printf("queue:  %lu uploaded, %lu pending\n",
              static_cast<unsigned long>(st.uploaded),
              static_cast<unsigned long>(st.pending));
  if (st.last_error != ESP_OK) std::printf("error:  %s\n", esp_err_to_name(st.last_error));
}

int cmd_cloud(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: cloud <status | wifi SSID [PASSWORD] | portal | forget\n"
                "             | url URL | token TOKEN | sync | resync>\n");
    return 0;
  }
  const char* sub = argv[1];

  if (std::strcmp(sub, "status") == 0) { print_status(); return 0; }

  if (std::strcmp(sub, "wifi") == 0) {
    if (argc < 3) { std::printf("usage: cloud wifi <ssid> [password]\n"); return 1; }
    // Never echo the password, only whether one was given — the console output
    // ends up pasted into notes and issues far more often than anyone expects.
    const char* pass = argc >= 4 ? argv[3] : "";
    const esp_err_t e = set_credentials(argv[2], pass);
    std::printf("wifi '%s' (%s): %s\n", argv[2],
                pass[0] != '\0' ? "with password" : "open", esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
  }

  if (std::strcmp(sub, "portal") == 0) {
    const esp_err_t e = start_provisioning();
    std::printf("setup AP '%s': %s\n", settings::get().name, esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
  }

  if (std::strcmp(sub, "forget") == 0) { forget(); std::printf("credentials erased\n"); return 0; }

  if (std::strcmp(sub, "url") == 0) {
    if (argc < 3) { std::printf("usage: cloud url <https-exec-url>\n"); return 1; }
    const esp_err_t e = set_endpoint(argv[2]);
    std::printf("url: %s\n", esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
  }

  if (std::strcmp(sub, "token") == 0) {
    if (argc < 3) { std::printf("usage: cloud token <token>\n"); return 1; }
    const esp_err_t e = set_token(argv[2]);
    // Length only, never the value — console output gets pasted into notes.
    std::printf("token (%u chars): %s\n", static_cast<unsigned>(std::strlen(argv[2])),
                esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
  }

  if (std::strcmp(sub, "sync") == 0) { notify_new_record(); std::printf("sync kicked\n"); return 0; }

  if (std::strcmp(sub, "resync") == 0) {
    resync();
    std::printf("high-water mark rewound; re-offering every record on disk\n");
    return 0;
  }

  std::printf("unknown subcommand '%s'\n", sub);
  return 1;
}

void register_commands() {
  if (!shell::ready()) {
    ESP_LOGW(kTag, "shell not up; `cloud` command unavailable");
    return;
  }
  // Trailing members left defaulted rather than spelled out: esp_console_cmd_t
  // has grown fields across IDF releases, and naming ones this command does not
  // use is how a header bump turns into a build break.
  const esp_console_cmd_t cmd = {
      .command = "cloud",
      .help = "network + sheet sync: status | wifi | portal | forget | url | token | sync | resync",
      .hint = nullptr,
      .func = &cmd_cloud,
  };
  esp_console_cmd_register(&cmd);
}

// --- The config form --------------------------------------------------------

// Runs on the httpd task. The portal applies everything `settings` owns itself;
// what arrives here is the half this component owns — the endpoint, the radio's
// credentials, and the name, which has to reach mDNS as well as NVS.
void on_portal_submit(const portal::Submission& sub) {
  // A blank name is not "unchanged": set_name reads it as "restore the
  // MAC-derived default", which is the useful meaning of clearing the field.
  settings::set_name(sub.name);
  apply_identity();

  if (sub.url[0] != '\0') set_endpoint(sub.url);
  if (sub.token[0] != '\0') set_token(sub.token);

  if (sub.ssid[0] != '\0') {
    wifi_config_t cur = {};
    const bool same_ssid =
        esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK &&
        std::strncmp(reinterpret_cast<const char*>(cur.sta.ssid), sub.ssid,
                     sizeof(cur.sta.ssid)) == 0;
    // Re-applying credentials drops the link and reconnects. Over the LAN the
    // network field comes back prefilled with the network the page was served
    // on, so doing that unconditionally would cost a reconnect every time
    // someone nudged a threshold. Only a different network, or a password
    // deliberately typed in, is a real change.
    if (!same_ssid || sub.password[0] != '\0') {
      set_credentials(sub.ssid, sub.password);
    }
  }
}

}  // namespace

esp_err_t init() {
  if (s_mutex != nullptr) return ESP_ERR_INVALID_STATE;

  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) return ESP_ERR_NO_MEM;
  s_events = xEventGroupCreate();
  if (s_events == nullptr) return ESP_ERR_NO_MEM;

  load_config();
  compute_device_id();

  // We are the first owner of the default event loop and of netif today; guard
  // both against ESP_ERR_INVALID_STATE so a future second user cannot break this
  // by initialising first.
  esp_err_t e = esp_netif_init();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
  s_netif = esp_netif_create_default_wifi_sta();
  if (s_netif == nullptr) return ESP_ERR_NO_MEM;

  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), kTag, "wifi_init");
  // Credentials persist in NVS through the driver, so this component stores none
  // of its own — there is exactly one copy and it is the one the radio uses.
  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), kTag, "wifi_storage");

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr, nullptr);

  const esp_timer_create_args_t targs = {
      .callback = &on_retry_timer,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "cloud_retry",
      .skip_unhandled_events = true,
  };
  ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), kTag, "retry_timer");

  apply_identity();

  const bool provisioned = has_stored_creds();
  if (provisioned) {
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "wifi_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi_start");
    s_started = true;
    set_wifi_state(WifiState::Connecting);
  } else {
    // No credentials: come up as an open access point serving the setup form, so
    // a phone is the whole client. APSTA rather than AP, because the form's
    // network picker needs a station to scan with.
    //
    // The SSID is the post's name, which on an unprovisioned device is still the
    // MAC-derived Post-XXXX — so a rack of identical boxes is already told apart
    // in the phone's Wi-Fi list, before the portal has even opened.
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "wifi_mode");
    // A named copy, not `settings::get().name`: get() returns by value, so the
    // array member would decay to a pointer into a temporary that dies at the end
    // of the statement.
    const settings::Settings cfg = settings::get();
    const esp_err_t ap_err = portal::start_ap(cfg.name);
    if (ap_err != ESP_OK) ESP_LOGE(kTag, "setup AP failed: %s", esp_err_to_name(ap_err));
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi_start");
    s_started = true;
    set_wifi_state(WifiState::Disabled);
  }

  // Started whichever way the radio came up, and never stopped again: the form
  // is where thresholds and the name are edited, not just where credentials
  // arrive once. See cloud_portal.hpp for why the two halves are separate.
  const esp_err_t srv = portal::start_server(&on_portal_submit);
  if (srv != ESP_OK) ESP_LOGE(kTag, "config form unavailable: %s", esp_err_to_name(srv));

  register_commands();

  xTaskCreatePinnedToCore(sync_task, "cloud", kSyncTaskStack, nullptr,
                          kSyncTaskPrio, &s_task, kSyncTaskCore);
  if (s_task == nullptr) return ESP_ERR_NO_MEM;

  // Kick once at boot: records written while the last session was offline are
  // still queued, and nothing else would wake the uploader until either the
  // link comes up or the next record lands a minute from now.
  xEventGroupSetBits(s_events, kBitNewRecord);

  ESP_LOGI(kTag, "init: %s, endpoint %s, device %s, hwm %lu",
           provisioned ? "credentials stored, connecting" : "no credentials, setup AP up",
           s_url[0] != '\0' ? "set" : "unset", s_device_id,
           static_cast<unsigned long>(s_hwm));
  return ESP_OK;
}

Status status() {
  Status st{};
  st.wifi = WifiState::Disabled;
  if (s_mutex == nullptr) return st;

  // A short timeout, not portMAX_DELAY: this is called from the LVGL task on a
  // repaint, and a dropped frame is a far better outcome than a stalled one.
  uint32_t hwm = 0;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    st.wifi         = s_wifi;
    st.time_synced  = s_time_synced;
    st.rssi_dbm     = s_rssi;
    std::memcpy(st.ssid, s_ssid, sizeof(st.ssid));
    std::memcpy(st.ip, s_ip, sizeof(st.ip));
    std::memcpy(st.host, s_host, sizeof(st.host));
    st.sync         = s_sync;
    st.endpoint_set = s_url[0] != '\0';
    st.uploaded     = s_hwm;
    st.last_error   = s_last_error;
    hwm             = s_hwm;
    xSemaphoreGive(s_mutex);
  }
  // Outside our lock: next_index() takes storage's own, and holding two at once
  // is how a deadlock gets built by accident.
  const uint32_t total = storage::next_index();
  st.pending = (total > hwm) ? (total - hwm) : 0;
  st.provisioning = portal::ap_active();
  return st;
}

esp_err_t set_credentials(const char* ssid, const char* password) {
  if (s_mutex == nullptr) return ESP_ERR_INVALID_STATE;
  if (ssid == nullptr || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

  // memcpy of a bounded length, not snprintf: the driver's fields are fixed-width
  // byte arrays that need not be NUL-terminated, and a zero-initialised config
  // leaves any unused tail as NUL anyway.
  wifi_config_t sta = {};
  std::memcpy(sta.sta.ssid, ssid, strnlen(ssid, sizeof(sta.sta.ssid)));
  if (password != nullptr) {
    std::memcpy(sta.sta.password, password, strnlen(password, sizeof(sta.sta.password)));
  }
  // Capable but not required: WPA3-only APs refuse a station that cannot do
  // protected management frames, and requiring it would refuse every WPA2 AP in
  // the other direction. Advertising the capability joins both.
  sta.sta.pmf_cfg.capable = true;
  sta.sta.pmf_cfg.required = false;

  if (!s_started) {
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "wifi_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), kTag, "wifi_config");
    { Lock l; s_retry_count = 0; s_retry_delay_ms = kRetryMinMs; }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi_start");
    s_started = true;
    set_wifi_state(WifiState::Connecting);  // STA_START then issues the connect
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), kTag, "wifi_config");
  { Lock l; s_retry_count = 0; s_retry_delay_ms = kRetryMinMs; s_wifi = WifiState::Connecting; }
  // Drop whatever link exists, then arm the retry ourselves rather than relying
  // on the disconnect event: if the station was already idle — offline, waiting
  // out a long backoff — no event arrives, and the pending timer could be five
  // minutes away. Arming here makes "new credentials" always mean "try now",
  // whichever state the radio was in.
  esp_wifi_disconnect();
  schedule_retry(kRetryMinMs);
  return ESP_OK;
}

esp_err_t start_provisioning() {
  if (s_mutex == nullptr) return ESP_ERR_INVALID_STATE;
  if (portal::ap_active()) return ESP_OK;
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "wifi_mode");
  ESP_RETURN_ON_ERROR(portal::start_ap(settings::get().name), kTag, "start_ap");
  if (!s_started) {
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi_start");
    s_started = true;
  }
  return ESP_OK;
}

void stop_provisioning() {
  if (s_mutex == nullptr) return;
  if (!portal::ap_active()) return;
  portal::stop_ap();
  // Same teardown the GOT_IP path does: the portal drops the DNS responder, the
  // mode change is what actually takes the AP off the air.
  esp_wifi_set_mode(WIFI_MODE_STA);
  ESP_LOGI(kTag, "setup AP closed");
}

void forget() {
  if (s_mutex == nullptr) return;
  // Erase first, so the disconnect below settles to Disabled — the DISCONNECTED
  // handler only reschedules while credentials exist.
  wifi_config_t empty = {};
  esp_wifi_set_config(WIFI_IF_STA, &empty);
  if (s_retry_timer != nullptr) esp_timer_stop(s_retry_timer);
  esp_wifi_disconnect();
  {
    Lock l;
    s_retry_count = 0;
    s_retry_delay_ms = kRetryMinMs;
    s_rssi = 0;
    s_ssid[0] = '\0';
    s_ip[0] = '\0';
    s_wifi = WifiState::Disabled;
  }
  ESP_LOGI(kTag, "forgot stored Wi-Fi credentials");
}


esp_err_t set_endpoint(const char* url) {
  if (s_mutex == nullptr) return ESP_ERR_INVALID_STATE;
  if (url == nullptr) return ESP_ERR_INVALID_ARG;
  // Rejected rather than coerced. Apps Script only answers on https, so an
  // http:// endpoint is a typo — and one that would otherwise fail silently once
  // a minute for as long as the post is deployed.
  if (std::strncmp(url, "https://", 8) != 0) return ESP_ERR_INVALID_ARG;
  if (std::strlen(url) >= kUrlMax) return ESP_ERR_INVALID_SIZE;
  if (!persist_str(kNvsKeyUrl, url, s_url, sizeof(s_url))) return ESP_FAIL;
  notify_new_record();  // an endpoint that just appeared has a backlog waiting
  return ESP_OK;
}

esp_err_t set_token(const char* token) {
  if (s_mutex == nullptr) return ESP_ERR_INVALID_STATE;
  if (token == nullptr) return ESP_ERR_INVALID_ARG;
  if (std::strlen(token) >= kTokenMax) return ESP_ERR_INVALID_SIZE;
  if (!persist_str(kNvsKeyToken, token, s_token, sizeof(s_token))) return ESP_FAIL;
  notify_new_record();
  return ESP_OK;
}

void endpoint(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return;
  out[0] = '\0';
  if (s_mutex == nullptr) return;
  Lock l;
  std::snprintf(out, cap, "%s", s_url);
}

void resync() {
  if (s_mutex == nullptr) return;
  // Rewound to first_index(), not to zero: everything below it was rotated away
  // and cannot be re-offered, so zero would only mean the uploader spends its
  // first batch discovering that. The Apps Script dedupes on device + index, so
  // re-sending rows the Sheet already holds is safe.
  const uint32_t first = storage::first_index();
  { Lock l; s_hwm = first; s_last_error = ESP_OK; }
  persist_hwm(first);
  notify_new_record(/*urgent=*/true);
  ESP_LOGI(kTag, "hwm rewound to %lu", static_cast<unsigned long>(first));
}

void notify_new_record(bool urgent) {
  if (s_events == nullptr) return;
  xEventGroupSetBits(s_events, urgent ? (kBitNewRecord | kBitUrgent) : kBitNewRecord);
}

}  // namespace thermopost::cloud
