#include "ota.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <strings.h>  // strcasecmp, for the hash comparison

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "alert.hpp"
#include "power.hpp"
#include "settings.hpp"
#include "shell.hpp"

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

namespace thermopost::ota {
namespace {

constexpr const char* kTag = "ota";

// What every build not made by tools/release.sh carries, so a bench post is never
// replaced from the Sheet a minute after it was flashed.
constexpr std::string_view kDevSuffix = "-dev";

// An update spends the pack for a minute of radio and flash writes, and a post that
// browns out halfway reboots into the image it had. Below this it waits for a charger.
constexpr int kMinBatteryPct = 50;

// How long a new image has to make its first upload: this margin plus one record
// interval, since an upload needs a record to carry.
constexpr int64_t kProveGraceMs = 10 * 60 * 1000;

// A failed download is retried no sooner than this, so a bad URL or hash in the Sheet
// costs one attempt per period rather than one per upload.
constexpr int64_t kFailedRetryMs = 60 * 60 * 1000;
// A release that was rolled back is offered again only after this much uptime, so one
// that failed on a network blip is not refused forever.
constexpr int64_t kRejectedRetryMs = 24 * 60 * 60 * 1000;

constexpr int64_t kUsPerMs = 1000;
constexpr int     kMsPerSec = 1000;

// GitHub answers a release download with a redirect to a signed URL several hundred
// characters long, which the request line has to fit in.
constexpr int kHttpTxBufBytes = 2048;
constexpr int kHttpRxBufBytes = 4096;
constexpr int kHttpTimeoutMs  = 15000;
constexpr int kMaxRedirects   = 5;
constexpr size_t kChunkBytes  = 4096;
constexpr int kProgressStepPct = 10;
constexpr int kPctFull = 100;
// Time for the last log lines to leave the console before the reboot.
constexpr uint32_t kRebootDelayMs = 500;

constexpr uint32_t    kTaskStack = 12288;  // the TLS handshake, as in cloud.cpp
constexpr UBaseType_t kTaskPrio  = 3;
constexpr BaseType_t  kTaskCore  = 0;      // off the LVGL core

constexpr size_t kSha256Bytes = 32;

bool s_inited = false;
char s_version[kVersionMax] = {};
char s_rejected[kVersionMax] = {};
std::atomic<bool> s_proving{false};
std::atomic<bool> s_busy{false};
esp_timer_handle_t s_deadline = nullptr;

// The job the worker runs; written only while s_busy is false.
Target s_job = {};
// Why the last attempt ended, for the `ota` command. Always a string literal.
std::atomic<const char*> s_last = "none this boot";

// The last version that failed to download, and when.
char s_failed[kVersionMax] = {};
int64_t s_failed_at_ms = 0;

// The last refusal logged, so a post on USB does not repeat it every minute.
const char* s_refused = nullptr;

int64_t now_ms() { return esp_timer_get_time() / kUsPerMs; }

bool is_dev(const char* v) {
  const std::string_view s(v);
  return s.size() >= kDevSuffix.size() && s.substr(s.size() - kDevSuffix.size()) == kDevSuffix;
}

void to_hex(const uint8_t* bytes, size_t n, char* out) {
  constexpr char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 0xf];
  }
  out[2 * n] = '\0';
}

bool is_redirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Why the post should not update right now, or null when it may.
const char* refusal() {
  if (s_proving.load()) return "the running image has not proved itself yet";
  const alert::Status a = alert::status();
  if (a.state != alert::State::Normal) return "an alarm is open";
  if (power::on_external_power()) return nullptr;
  const power::Battery b = power::battery();
  if (!b.settled) return "the battery reading has not settled";
  if (b.pct < kMinBatteryPct) return "the battery is too low";
  return nullptr;
}

// Open `t.url`, following redirects by hand so each hop can be logged. Leaves the
// client positioned at the body; returns its length, or -1 when the server sent none.
esp_err_t open_following(esp_http_client_handle_t c, int64_t* length) {
  for (int hop = 0;; ++hop) {
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) return err;
    *length = esp_http_client_fetch_headers(c);
    if (*length < 0) return ESP_FAIL;
    const int status = esp_http_client_get_status_code(c);
    if (status == 200) return ESP_OK;
    if (!is_redirect(status)) {
      ESP_LOGE(kTag, "download answered %d", status);
      return ESP_FAIL;
    }
    if (hop == kMaxRedirects) {
      ESP_LOGE(kTag, "too many redirects");
      return ESP_FAIL;
    }
    err = esp_http_client_set_redirection(c);
    if (err != ESP_OK) return err;
    esp_http_client_flush_response(c, nullptr);
  }
}

// Stream the image into the idle slot, hashing it on the way. ESP_OK leaves a checked
// image there; the boot partition is not touched.
esp_err_t download(const Target& t, const esp_partition_t* part) {
  esp_http_client_config_t cfg = {};
  cfg.url               = t.url;
  cfg.timeout_ms        = kHttpTimeoutMs;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size       = kHttpRxBufBytes;
  cfg.buffer_size_tx    = kHttpTxBufBytes;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return ESP_ERR_NO_MEM;

  auto* buf = static_cast<uint8_t*>(std::malloc(kChunkBytes));
  esp_ota_handle_t h = 0;
  bool begun = false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);

  int64_t length = 0;
  esp_err_t err = buf == nullptr ? ESP_ERR_NO_MEM : open_following(c, &length);
  if (err == ESP_OK && length > static_cast<int64_t>(part->size)) {
    ESP_LOGE(kTag, "image is %" PRId64 " bytes, the slot holds %" PRIu32, length, part->size);
    s_last = "the image does not fit";
    err = ESP_ERR_INVALID_SIZE;
  }
  if (err == ESP_OK) {
    // Sequential writes erase as they go, rather than the whole slot up front.
    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    begun = err == ESP_OK;
  }

  size_t total = 0;
  int next_pct = kProgressStepPct;
  if (err == ESP_OK) {
    mbedtls_sha256_starts(&sha, /*is224=*/0);
    for (;;) {
      const int n = esp_http_client_read(c, reinterpret_cast<char*>(buf), kChunkBytes);
      if (n < 0) { err = ESP_FAIL; break; }
      if (n == 0) {
        if (!esp_http_client_is_complete_data_received(c)) err = ESP_FAIL;
        break;
      }
      mbedtls_sha256_update(&sha, buf, n);
      err = esp_ota_write(h, buf, n);
      if (err != ESP_OK) break;
      total += n;
      if (length > 0 && static_cast<int64_t>(total) * kPctFull >= length * next_pct) {
        ESP_LOGI(kTag, "%d%%", next_pct);
        next_pct += kProgressStepPct;
      }
    }
    if (err != ESP_OK) s_last = "the download broke off";
  }

  if (err == ESP_OK) {
    uint8_t digest[kSha256Bytes];
    char hex[kSha256HexLen + 1];
    mbedtls_sha256_finish(&sha, digest);
    to_hex(digest, sizeof(digest), hex);
    if (strcasecmp(hex, t.sha256) != 0) {
      ESP_LOGE(kTag, "sha256 is %s, the Sheet says %s", hex, t.sha256);
      s_last = "the sha256 does not match";
      err = ESP_ERR_INVALID_CRC;
    }
  }

  if (err == ESP_OK) {
    // Validates the image structure; the hash above is what says it is the right one.
    err = esp_ota_end(h);
    begun = false;
    if (err != ESP_OK) s_last = "the image is not a valid app";
  }
  if (begun) esp_ota_abort(h);

  if (err == ESP_OK && t.version[0] != '\0') {
    // A release whose binary says otherwise would be offered again after every boot.
    esp_app_desc_t desc = {};
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK ||
        std::strncmp(desc.version, t.version, sizeof(desc.version)) != 0) {
      ESP_LOGE(kTag, "the image calls itself %s, the Sheet %s", desc.version, t.version);
      s_last = "the image's version is not the one named";
      err = ESP_ERR_INVALID_VERSION;
    }
  }

  mbedtls_sha256_free(&sha);
  std::free(buf);
  esp_http_client_cleanup(c);
  if (err == ESP_OK) ESP_LOGI(kTag, "%u bytes, sha256 matches", static_cast<unsigned>(total));
  return err;
}

void worker(void* /*arg*/) {
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  esp_err_t err = ESP_ERR_NOT_FOUND;
  if (part == nullptr) {
    s_last = "no slot to update into";
  } else {
    ESP_LOGW(kTag, "updating %s -> %s into %s", s_version,
             s_job.version[0] ? s_job.version : "(unnamed)", part->label);
    err = download(s_job, part);
  }
  if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
  if (err == ESP_OK) {
    ESP_LOGW(kTag, "rebooting into %s; it has to upload once to stay", s_job.version);
    vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
    esp_restart();
  }
  ESP_LOGE(kTag, "update failed (%s): %s", esp_err_to_name(err), s_last.load());
  std::snprintf(s_failed, sizeof(s_failed), "%s", s_job.version);
  s_failed_at_ms = now_ms();
  s_busy = false;
  vTaskDelete(nullptr);
}

bool start(const Target& t) {
  bool idle = false;
  if (!s_busy.compare_exchange_strong(idle, true)) return false;
  s_job = t;
  s_last = "in progress";
  if (xTaskCreatePinnedToCore(worker, "ota", kTaskStack, nullptr, kTaskPrio, nullptr,
                              kTaskCore) != pdPASS) {
    s_last = "no memory for the update task";
    s_busy = false;
    return false;
  }
  return true;
}

// The proving deadline. Whichever of this and confirm() comes first decides.
void on_deadline(void* /*arg*/) {
  if (!s_proving.exchange(false)) return;
  ESP_LOGE(kTag, "%s never uploaded; rolling back", s_version);
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

const char* state_name(const esp_partition_t* run) {
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(run, &st) != ESP_OK) return "flashed over USB";
  switch (st) {
    case ESP_OTA_IMG_PENDING_VERIFY: return "proving itself";
    case ESP_OTA_IMG_VALID:          return "confirmed";
    case ESP_OTA_IMG_NEW:            return "new";
    default:                         return "flashed over USB";
  }
}

void print_status() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  std::printf("version   %s%s\n", s_version, is_dev(s_version) ? " (dev build: the Sheet leaves it alone)" : "");
  std::printf("running   %s, %s\n", run->label, state_name(run));
  std::printf("rejected  %s\n", s_rejected[0] ? s_rejected : "none");
  std::printf("update    %s\n", s_last.load());
}

int cmd_ota(int argc, char** argv) {
  if (argc == 1) { print_status(); return 0; }
  const std::string_view sub(argv[1]);
  if (sub == "confirm" && argc == 2) {
    if (!s_proving.load()) { std::printf("nothing to confirm\n"); return 1; }
    confirm();
    return 0;
  }
  if (sub == "rollback" && argc == 2) {
    // Returns only when there is no other image to go back to.
    const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    std::printf("cannot roll back: %s\n", esp_err_to_name(err));
    return 1;
  }
  if (argc == 3 && std::strlen(argv[2]) == kSha256HexLen && std::strlen(argv[1]) < kUrlMax) {
    // The bench path: no version to check, and dev builds allowed, but the power and
    // alarm checks still apply.
    if (const char* why = refusal()) { std::printf("not now: %s\n", why); return 1; }
    Target t = {};
    std::snprintf(t.url, sizeof(t.url), "%s", argv[1]);
    std::snprintf(t.sha256, sizeof(t.sha256), "%s", argv[2]);
    if (!start(t)) { std::printf("busy\n"); return 1; }
    std::printf("downloading\n");
    return 0;
  }
  std::printf("usage: ota | ota <url> <sha256> | ota confirm | ota rollback\n");
  return 1;
}

void register_command() {
  const esp_console_cmd_t cmd = {
      .command = "ota",
      .help = "firmware version and update state; `ota <url> <sha256>` updates now; "
              "`ota confirm` keeps a proving image; `ota rollback` returns to the last one",
      .hint = nullptr,
      .func = &cmd_ota,
  };
  esp_console_cmd_register(&cmd);
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;
  (void)version();

  if (const esp_partition_t* bad = esp_ota_get_last_invalid_partition()) {
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(bad, &desc) == ESP_OK) {
      std::snprintf(s_rejected, sizeof(s_rejected), "%s", desc.version);
    }
  }

  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  s_proving = esp_ota_get_state_partition(run, &st) == ESP_OK &&
              st == ESP_OTA_IMG_PENDING_VERIFY;
  if (s_proving.load()) {
    const int64_t deadline_ms =
        kProveGraceMs + static_cast<int64_t>(settings::get().interval_s) * kMsPerSec;
    const esp_timer_create_args_t args = {
        .callback = &on_deadline,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_prove",
        .skip_unhandled_events = false,
    };
    esp_err_t err = esp_timer_create(&args, &s_deadline);
    if (err == ESP_OK) err = esp_timer_start_once(s_deadline, deadline_ms * kUsPerMs);
    if (err != ESP_OK) {
      // Without the deadline a broken image would be kept, so keep this one honestly.
      ESP_LOGE(kTag, "no proving deadline (%s); keeping the image", esp_err_to_name(err));
      confirm();
    } else {
      ESP_LOGW(kTag, "%s is new; it stays if it uploads within %" PRId64 " s", s_version,
               deadline_ms / kMsPerSec);
    }
  }
  if (s_rejected[0]) ESP_LOGW(kTag, "%s was rolled back", s_rejected);

  if (shell::ready()) register_command();
  s_inited = true;
  ESP_LOGI(kTag, "firmware %s", s_version);
  return ESP_OK;
}

const char* version() {
  if (s_version[0] == '\0') {
    std::snprintf(s_version, sizeof(s_version), "%s", esp_app_get_description()->version);
  }
  return s_version;
}

const char* rejected() { return s_rejected; }

bool proving() { return s_proving.load(); }

void confirm() {
  if (!s_proving.exchange(false)) return;
  if (s_deadline != nullptr) esp_timer_stop(s_deadline);
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    ESP_LOGI(kTag, "%s uploaded and is kept", s_version);
  } else {
    ESP_LOGE(kTag, "could not keep %s: %s", s_version, esp_err_to_name(err));
  }
}

bool busy() { return s_busy.load(); }

void offer(const Target& t) {
  if (!s_inited || s_busy.load()) return;
  if (std::strncmp(t.version, s_version, sizeof(s_version)) == 0) return;

  const char* why = refusal();
  if (why == nullptr && is_dev(s_version)) why = "this is a dev build";
  if (why == nullptr && std::strcmp(t.version, s_rejected) == 0 && now_ms() < kRejectedRetryMs) {
    why = "that release was rolled back here";
  }
  if (why == nullptr && std::strcmp(t.version, s_failed) == 0 &&
      now_ms() - s_failed_at_ms < kFailedRetryMs) {
    why = "that release failed to download recently";
  }
  if (why != nullptr) {
    if (why != s_refused) ESP_LOGI(kTag, "not updating to %s: %s", t.version, why);
    s_refused = why;
    return;
  }
  s_refused = nullptr;
  start(t);
}

}  // namespace thermopost::ota
