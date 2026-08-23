#include "cloud_portal.hpp"

#include "settings.hpp"

#include "dhcpserver/dhcpserver.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace thermopost::cloud::portal {
namespace {

constexpr const char* kTag = "cloud.portal";

// --- Tunables ---------------------------------------------------------------
constexpr int      kDnsPort     = 53;
constexpr int      kDnsMaxLen   = 256;   // DNS-over-UDP messages are small
constexpr uint32_t kDnsTtlSec   = 300;
constexpr uint16_t kDnsTypeA    = 1;     // A (host address) query/answer
constexpr uint16_t kDnsClassIn  = 1;
constexpr size_t   kDnsMaxAnswers = 8;   // replies past this are dropped, not truncated
constexpr uint32_t kDnsTaskStack = 3072;
constexpr UBaseType_t kDnsTaskPrio = 4;
constexpr BaseType_t  kDnsTaskCore = 0;  // off the LVGL core (1)

constexpr size_t kMaxScan   = 16;    // SSIDs rendered in the dropdown
constexpr uint32_t kHttpStack = 8192;  // the GET handler runs a blocking scan
constexpr size_t kBodyMax   = 1536;  // POST /save body cap
constexpr size_t kValueMax  = 256;   // one decoded form value
constexpr int    kApMaxConn = 4;
constexpr uint8_t kApChannel = 1;
constexpr size_t kSsidFieldMax = 32;  // 802.11 SSID, unterminated on the wire

// --- State ------------------------------------------------------------------
SubmitCb       s_cb        = nullptr;
httpd_handle_t s_httpd     = nullptr;
int            s_dns_sock  = -1;
volatile bool  s_dns_run   = false;
bool           s_ap_up     = false;
uint32_t       s_ap_ip     = 0;  // network byte order, for DNS answers
esp_netif_t*   s_ap_netif  = nullptr;

wifi_ap_record_t s_scan[kMaxScan];
size_t           s_scan_n = 0;

// ===== DNS responder ========================================================
// Answer every A query with the AP's own address, so the phone OS's
// captive-portal probe — and any name the browser resolves — lands on our HTTP
// server. Adapted from ESP-IDF's captive_portal example.

struct __attribute__((packed)) DnsHeader {
  uint16_t id, flags, qd_count, an_count, ns_count, ar_count;
};
struct __attribute__((packed)) DnsQuestion {
  uint16_t type, qclass;  // 'class' is reserved in C++
};
struct __attribute__((packed)) DnsAnswer {
  uint16_t ptr_offset, type, aclass;
  uint32_t ttl;
  uint16_t addr_len;
  uint32_t ip_addr;
};

// Walk a length-prefixed DNS name (3www6google3com0) past its terminator,
// returning the byte just after it, or nullptr if it runs off the buffer.
const char* skip_dns_name(const char* label, const char* end) {
  while (label < end && *label != 0) {
    label += static_cast<uint8_t>(*label) + 1;
  }
  return (label < end) ? label + 1 : nullptr;
}

// Build a reply into `out`. Returns its length, or -1 to drop the packet.
int build_dns_reply(const char* req, size_t req_len, char* out, size_t out_max) {
  if (req_len < sizeof(DnsHeader) || req_len > out_max) return -1;
  std::memcpy(out, req, req_len);
  auto* h = reinterpret_cast<DnsHeader*>(out);
  if (ntohs(h->flags) & 0x8000) return -1;  // already a response — ignore
  const uint16_t qd = ntohs(h->qd_count);
  if (qd == 0) return -1;

  const size_t reply_len = req_len + qd * sizeof(DnsAnswer);
  if (reply_len > out_max) return -1;
  h->flags |= htons(0x8000);  // QR = response
  h->an_count = h->qd_count;  // one A answer per question

  const char* end = out + req_len;
  const char* qd_ptr = out + sizeof(DnsHeader);
  char* ans_ptr = out + req_len;
  for (uint16_t i = 0; i < qd; ++i) {
    const char* after_name = skip_dns_name(qd_ptr, end);
    if (after_name == nullptr || after_name + sizeof(DnsQuestion) > end) return -1;
    qd_ptr = after_name + sizeof(DnsQuestion);

    auto* a = reinterpret_cast<DnsAnswer*>(ans_ptr);
    a->ptr_offset = htons(0xC000 | sizeof(DnsHeader));  // points at the 1st question
    a->type       = htons(kDnsTypeA);
    a->aclass     = htons(kDnsClassIn);
    a->ttl        = htonl(kDnsTtlSec);
    a->addr_len   = htons(sizeof(uint32_t));
    a->ip_addr    = s_ap_ip;  // already network order
    ans_ptr += sizeof(DnsAnswer);
  }
  return static_cast<int>(reply_len);
}

void dns_task(void* /*arg*/) {
  char rx[kDnsMaxLen];
  char tx[kDnsMaxLen + kDnsMaxAnswers * sizeof(DnsAnswer)];  // request copy + answers
  while (s_dns_run) {
    struct sockaddr_in client = {};
    socklen_t slen = sizeof(client);
    const int len = recvfrom(s_dns_sock, rx, sizeof(rx), 0,
                             reinterpret_cast<struct sockaddr*>(&client), &slen);
    if (len < 0) break;  // socket shut down by stop_ap()
    const int reply = build_dns_reply(rx, static_cast<size_t>(len), tx, sizeof(tx));
    if (reply > 0) {
      sendto(s_dns_sock, tx, reply, 0,
             reinterpret_cast<struct sockaddr*>(&client), slen);
    }
  }
  s_dns_sock = -1;
  vTaskDelete(nullptr);
}

esp_err_t start_dns() {
  s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (s_dns_sock < 0) return ESP_FAIL;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(kDnsPort);
  if (bind(s_dns_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(s_dns_sock);
    s_dns_sock = -1;
    return ESP_FAIL;
  }
  s_dns_run = true;
  xTaskCreatePinnedToCore(dns_task, "captdns", kDnsTaskStack, nullptr,
                          kDnsTaskPrio, nullptr, kDnsTaskCore);
  return ESP_OK;
}

// ===== HTML =================================================================

// Append `in` with the five markup-significant characters escaped, so a network
// name or a URL carrying a quote cannot break out of an attribute.
void append_escaped(std::string& out, const char* in) {
  for (const char* p = in; *p != '\0'; ++p) {
    switch (*p) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += *p;       break;
    }
  }
}

void append_int(std::string& out, long v) { out += std::to_string(v); }

// Hundredths of a degree as a decimal, by integer arithmetic — the stored value
// IS exact hundredths and routing it through a double to print it could only
// introduce error. Same reasoning as cloud_json::fmt_c100.
void append_c100(std::string& out, int16_t c100) {
  const bool neg = c100 < 0;
  const int32_t mag = neg ? -static_cast<int32_t>(c100) : static_cast<int32_t>(c100);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%s%ld.%02ld", neg ? "-" : "",
                static_cast<long>(mag / 100), static_cast<long>(mag % 100));
  out += buf;
}

// Blocking scan of nearby networks, cached after the first success. Runs in the
// httpd task, never the LVGL one, so the ~1.5 s block is invisible on screen.
//
// Only ever called while the SoftAP is up. A scan makes the radio leave its
// channel, and doing that to an associated station — which is what serving this
// form over the LAN means — would stall the very connection carrying the page.
void ensure_scan() {
  if (s_scan_n > 0) return;
  wifi_scan_config_t sc = {};  // active scan, all channels
  if (esp_wifi_scan_start(&sc, /*block=*/true) != ESP_OK) return;
  uint16_t n = kMaxScan;
  if (esp_wifi_scan_get_ap_records(&n, s_scan) != ESP_OK) n = 0;
  s_scan_n = (n > kMaxScan) ? kMaxScan : n;
}

void append_network_picker(std::string& h) {
  ensure_scan();
  // A real <select>, not a <datalist>: mobile browsers render the latter
  // inconsistently. The free-text field below covers hidden networks and wins
  // over the dropdown when non-empty.
  h += "<label>Wi-Fi network</label>"
       "<select name=ssid><option value=\"\" selected>&mdash; pick a network &mdash;</option>";
  for (size_t i = 0; i < s_scan_n; ++i) {
    const char* ssid = reinterpret_cast<const char*>(s_scan[i].ssid);
    if (ssid[0] == '\0') continue;
    bool dup = false;  // one name, several APs or bands
    for (size_t j = 0; j < i; ++j) {
      if (std::strcmp(ssid, reinterpret_cast<const char*>(s_scan[j].ssid)) == 0) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    h += "<option value=\"";
    append_escaped(h, ssid);
    h += "\">";
    append_escaped(h, ssid);
    h += "</option>";
  }
  h += "</select>"
       "<input name=ssidm autocomplete=off placeholder=\"&hellip;or type a hidden network\">";
}

void append_network_current(std::string& h) {
  wifi_ap_record_t ap = {};
  char joined[kSsidFieldMax + 1] = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    std::memcpy(joined, ap.ssid,
                strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(joined) - 1));
  }
  h += "<label>Wi-Fi network <small>(leave alone to stay on this one)</small></label>"
       "<input name=ssidm autocomplete=off value=\"";
  append_escaped(h, joined);
  h += "\">";
}

std::string build_form_html() {
  const settings::Settings s = settings::get();
  const bool provisioning = s_ap_up;

  std::string h;
  h.reserve(4096);
  h += "<!DOCTYPE html><html><head><meta charset=utf-8>"
       "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
       "<title>";
  append_escaped(h, s.name);
  h += "</title><style>"
       "body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:#111;color:#eee}"
       ".c{max-width:430px;margin:0 auto;padding:24px 20px}"
       "h1{font-size:20px;font-weight:600;margin:0 0 4px}"
       "h2{font-size:13px;font-weight:600;color:#C88036;text-transform:uppercase;"
       "letter-spacing:.08em;margin:28px 0 0;border-bottom:1px solid #333;padding-bottom:6px}"
       "label{display:block;margin:16px 0 5px;font-size:14px;color:#bbb}"
       "input,select{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;"
       "border:1px solid #444;background:#1c1c1c;color:#eee;font-size:16px}"
       "select{margin-bottom:8px}"
       ".row{display:flex;gap:10px}.row>div{flex:1}"
       ".chk{display:flex;align-items:center;gap:10px;margin-top:16px}"
       ".chk input{width:auto}"
       "button{margin-top:26px;width:100%;padding:13px;border:0;border-radius:24px;"
       "background:#C88036;color:#111;font-size:16px;font-weight:600}"
       "small{color:#888;font-weight:400}"
       "p.sub{color:#888;font-size:13px;margin:0 0 4px}"
       "</style></head><body><div class=c><h1>";
  append_escaped(h, s.name);
  h += "</h1><p class=sub>Thermo Post configuration</p>"
       "<form method=POST action=/save>"

       "<h2>Identity</h2>"
       "<label>Post name <small>(names the tank in the Sheet and on screen)</small></label>"
       "<input name=name maxlength=";
  append_int(h, static_cast<long>(settings::kNameMaxLen));
  h += " value=\"";
  append_escaped(h, s.name);
  h += "\">"

       "<h2>Network</h2>";
  if (provisioning) {
    append_network_picker(h);
  } else {
    append_network_current(h);
  }
  h += "<label>Password <small>(blank keeps the stored one)</small></label>"
       "<input name=pass type=password autocomplete=off>"

       "<h2>Sheet</h2>"
       "<label>Apps Script /exec URL</label>"
       "<input name=url type=url placeholder=\"https://script.google.com/&hellip;/exec\" value=\"";
  // The URL is prefilled because it is long, awkward to retype and not secret.
  // The token never is: echoing a stored secret back to whoever can reach the
  // form would hand it to them.
  char url[kUrlMax] = {};
  cloud::endpoint(url, sizeof(url));
  append_escaped(h, url);
  h += "\">"
       "<label>Token <small>(blank keeps the current one)</small></label>"
       "<input name=token autocomplete=off>"

       "<h2>Monitoring</h2>"
       "<p class=sub>All temperatures in &deg;C &mdash; that is what the post stores, "
       "whatever the screen is set to show.</p>"
       "<div class=row>"
       "<div><label>Alarm below</label><input name=min type=number step=0.01 value=\"";
  append_c100(h, s.min_c100);
  h += "\"></div>"
       "<div><label>Alarm above</label><input name=max type=number step=0.01 value=\"";
  append_c100(h, s.max_c100);
  h += "\"></div></div>"
       "<label>Hysteresis <small>(margin the reading must clear to recover)</small></label>"
       "<input name=hyst type=number step=0.01 value=\"";
  append_c100(h, s.hysteresis_c100);
  h += "\">"
       "<div class=row>"
       "<div><label>Interval <small>(s)</small></label>"
       "<input name=interval type=number min=";
  append_int(h, settings::kIntervalMinS);
  h += " max=";
  append_int(h, settings::kIntervalMaxS);
  h += " value=";
  append_int(h, s.interval_s);
  h += "></div>"
       "<div><label>Graph points</label>"
       "<input name=graph type=number min=";
  append_int(h, settings::kGraphPointsMin);
  h += " max=";
  append_int(h, settings::kGraphPointsMax);
  h += " value=";
  append_int(h, s.graph_points);
  h += "></div></div>"

       "<h2>Display &amp; sound</h2>"
       "<label>Show temperatures in</label>"
       "<select name=unit>"
       "<option value=c";
  if (s.unit == settings::TempUnit::Celsius) h += " selected";
  h += ">Celsius</option><option value=f";
  if (s.unit == settings::TempUnit::Fahrenheit) h += " selected";
  h += ">Fahrenheit</option></select>"
       // The hidden field is what makes the checkbox honest: an unchecked box
       // submits nothing at all, so without it "off" would be indistinguishable
       // from "field absent" and the alarm could never be muted from here. Both
       // keys arrive when it is checked and the later one wins.
       "<input type=hidden name=sound value=0>"
       "<div class=chk><input type=checkbox name=sound value=1 id=snd";
  if (s.sound_enabled) h += " checked";
  h += "><label for=snd style=\"margin:0\">Alarm sound</label></div>"
       "<label>Volume <small>(%)</small></label>"
       "<input name=volume type=number min=0 max=";
  append_int(h, settings::kVolumeMaxPct);
  h += " value=";
  append_int(h, s.volume_pct);
  h += ">"
       "<button type=submit>";
  h += provisioning ? "Connect" : "Save";
  h += "</button></form></div></body></html>";
  return h;
}

esp_err_t send_message(httpd_req_t* req, const char* title, const char* body) {
  std::string h =
      "<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0}"
      ".c{max-width:430px;margin:0 auto;padding:40px 24px;text-align:center}"
      "h1{font-size:20px}p{color:#bbb;line-height:1.5}"
      "a{color:#C88036}</style></head><body><div class=c><h1>";
  append_escaped(h, title);
  h += "</h1><p>";
  append_escaped(h, body);
  h += "</p><p><a href=\"/\">Back to settings</a></p></div></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, h.data(), static_cast<ssize_t>(h.size()));
}

esp_err_t root_get(httpd_req_t* req) {
  const std::string html = build_form_html();
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html.data(), static_cast<ssize_t>(html.size()));
}

// ===== Form parsing =========================================================

// Percent-decode `in` (and '+' to space) into NUL-terminated `out`.
size_t url_decode(const char* in, size_t in_len, char* out, size_t out_max) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  size_t o = 0;
  for (size_t i = 0; i < in_len && o + 1 < out_max; ++i) {
    char c = in[i];
    if (c == '+') {
      c = ' ';
    } else if (c == '%' && i + 2 < in_len) {
      const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        c = static_cast<char>(hi * 16 + lo);
        i += 2;
      }
    }
    out[o++] = c;
  }
  out[o] = '\0';
  return o;
}

// Copy into a fixed field, truncating to fit and always terminating. memcpy
// rather than snprintf so the compiler's format-truncation warning is moot —
// truncation is intended here, and validation happens after the parse.
void set_field(char* dst, size_t cap, const char* src) {
  const size_t n = strnlen(src, cap - 1);
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

// A decimal degree count to hundredths, by hand rather than through strtod: the
// destination is exact hundredths, and a float round trip can only lose to it.
// Accepts "21", "21.5", "-0.05"; more than two decimal places are truncated.
bool parse_c100(const char* s, int16_t& out) {
  if (s == nullptr || s[0] == '\0') return false;
  const char* p = s;
  bool neg = false;
  if (*p == '+' || *p == '-') { neg = (*p == '-'); ++p; }
  if (*p == '\0') return false;
  long whole = 0;
  bool any = false;
  for (; *p >= '0' && *p <= '9'; ++p) {
    whole = whole * 10 + (*p - '0');
    any = true;
    if (whole > INT16_MAX) return false;  // far past the sensor's range already
  }
  long frac = 0;
  if (*p == '.' || *p == ',') {
    ++p;
    for (int i = 0; i < 2; ++i) {
      const bool digit = (*p >= '0' && *p <= '9');
      frac = frac * 10 + (digit ? (*p - '0') : 0);
      if (digit) { ++p; any = true; }
    }
    while (*p >= '0' && *p <= '9') ++p;  // beyond hundredths: dropped
  }
  if (!any || *p != '\0') return false;
  const long v = whole * 100 + frac;
  if (v > INT16_MAX) return false;
  out = static_cast<int16_t>(neg ? -v : v);
  return true;
}

// A plain unsigned field. Out-of-range values are left to the settings setters
// to clamp — they are the single place that owns every range.
bool parse_u32(const char* s, uint32_t& out) {
  if (s == nullptr || s[0] == '\0') return false;
  char* end = nullptr;
  const long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < 0) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

// Everything the form can change, split by who owns it. `net` goes back to
// cloud.cpp; the rest is applied here through the settings setters.
struct Parsed {
  Submission net;
  settings::Settings cfg;  // seeded from the current values, overwritten per key
};

void parse_form(const char* body, size_t len, Parsed& p) {
  p.net = {};
  p.cfg = settings::get();
  size_t i = 0;
  while (i < len) {
    const size_t ks = i;
    while (i < len && body[i] != '=' && body[i] != '&') ++i;
    const size_t kl = i - ks;
    char val[kValueMax] = {};
    if (i < len && body[i] == '=') {
      ++i;
      const size_t vs = i;
      while (i < len && body[i] != '&') ++i;
      url_decode(body + vs, i - vs, val, sizeof(val));
    }
    if (i < len && body[i] == '&') ++i;
    auto key_is = [&](const char* k) {
      return std::strlen(k) == kl && std::memcmp(body + ks, k, kl) == 0;
    };

    uint32_t n = 0;
    int16_t c = 0;
    if      (key_is("name"))  set_field(p.net.name,     sizeof(p.net.name),     val);
    else if (key_is("ssid"))  set_field(p.net.ssid,     sizeof(p.net.ssid),     val);
    // Free text beats the dropdown when filled — it is the hidden-network path,
    // and over the LAN it is the only network field on the form.
    else if (key_is("ssidm")) { if (val[0] != '\0') set_field(p.net.ssid, sizeof(p.net.ssid), val); }
    else if (key_is("pass"))  set_field(p.net.password, sizeof(p.net.password), val);
    else if (key_is("url"))   set_field(p.net.url,      sizeof(p.net.url),      val);
    else if (key_is("token")) set_field(p.net.token,    sizeof(p.net.token),    val);
    else if (key_is("min"))   { if (parse_c100(val, c)) p.cfg.min_c100 = c; }
    else if (key_is("max"))   { if (parse_c100(val, c)) p.cfg.max_c100 = c; }
    else if (key_is("hyst"))  { if (parse_c100(val, c)) p.cfg.hysteresis_c100 = c; }
    else if (key_is("interval")) { if (parse_u32(val, n)) p.cfg.interval_s = static_cast<uint16_t>(std::min<uint32_t>(n, UINT16_MAX)); }
    else if (key_is("graph"))    { if (parse_u32(val, n)) p.cfg.graph_points = static_cast<uint16_t>(std::min<uint32_t>(n, UINT16_MAX)); }
    else if (key_is("volume"))   { if (parse_u32(val, n)) p.cfg.volume_pct = static_cast<uint8_t>(std::min<uint32_t>(n, UINT8_MAX)); }
    else if (key_is("unit"))     { p.cfg.unit = (val[0] == 'f') ? settings::TempUnit::Fahrenheit
                                                                : settings::TempUnit::Celsius; }
    else if (key_is("sound"))    { p.cfg.sound_enabled = (val[0] == '1'); }
  }
}

// Each setter clamps and is a no-op when unchanged, so passing the whole set
// through unconditionally costs nothing and needs no dirty tracking.
void apply_settings(const settings::Settings& c) {
  settings::set_band(c.min_c100, c.max_c100);
  settings::set_hysteresis_c100(c.hysteresis_c100);
  settings::set_interval_s(c.interval_s);
  settings::set_graph_points(c.graph_points);
  settings::set_unit(c.unit);
  settings::set_sound_enabled(c.sound_enabled);
  settings::set_volume_pct(c.volume_pct);
}

esp_err_t save_post(httpd_req_t* req) {
  const int total = req->content_len;
  if (total <= 0 || static_cast<size_t>(total) > kBodyMax) {
    return send_message(req, "Setup error", "The form was too large. Go back and retry.");
  }
  char body[kBodyMax + 1];
  int got = 0;
  while (got < total) {
    const int r = httpd_req_recv(req, body + got, total - got);
    if (r <= 0) return send_message(req, "Setup error", "Upload interrupted. Go back and retry.");
    got += r;
  }
  body[got] = '\0';

  Parsed p;
  parse_form(body, static_cast<size_t>(got), p);

  // A network has to be chosen only while the setup AP is what the browser is
  // talking through. Over the LAN the post is already on a network and a blank
  // field means "stay on it".
  if (s_ap_up && p.net.ssid[0] == '\0') {
    return send_message(req, "Setup error", "Please choose a Wi-Fi network.");
  }
  if (p.net.url[0] != '\0' && std::strncmp(p.net.url, "https://", 8) != 0) {
    return send_message(req, "Setup error", "The endpoint URL must start with https://");
  }

  apply_settings(p.cfg);
  // The network half goes back before the response is rendered, so a connect is
  // already under way as the browser paints the confirmation.
  if (s_cb != nullptr) s_cb(p.net);

  if (p.net.ssid[0] != '\0' && s_ap_up) {
    char msg[160];  // ~90 characters of fixed text plus a 32-character SSID
    std::snprintf(msg, sizeof(msg),
                  "Connecting to \"%s\". This setup network will disappear once the "
                  "post is online.", p.net.ssid);
    return send_message(req, "Connecting\xE2\x80\xA6", msg);
  }
  return send_message(req, "Saved", "The post has taken the new settings.");
}

}  // namespace

esp_err_t start_server(SubmitCb cb) {
  s_cb = cb;
  if (s_httpd != nullptr) return ESP_OK;

  // The wildcard GET is what makes a phone's captive check fail and pop the
  // sheet: every probe URL it tries answers with the form instead of the
  // 204/"Success" it is looking for.
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size       = kHttpStack;
  cfg.lru_purge_enable = true;
  cfg.uri_match_fn     = httpd_uri_match_wildcard;
  ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), kTag, "httpd_start");

  const httpd_uri_t save_uri = {
      .uri = "/save", .method = HTTP_POST, .handler = save_post, .user_ctx = nullptr};
  const httpd_uri_t root_uri = {
      .uri = "/*", .method = HTTP_GET, .handler = root_get, .user_ctx = nullptr};
  httpd_register_uri_handler(s_httpd, &save_uri);
  httpd_register_uri_handler(s_httpd, &root_uri);
  ESP_LOGI(kTag, "config form up on port %d", cfg.server_port);
  return ESP_OK;
}

esp_err_t start_ap(const char* ap_ssid) {
  if (s_ap_up) return ESP_OK;
  s_scan_n = 0;  // a fresh scan for this session

  if (s_ap_netif == nullptr) s_ap_netif = esp_netif_create_default_wifi_ap();
  if (s_ap_netif == nullptr) return ESP_ERR_NO_MEM;

  // Open, because the alternative is printing a passphrase somewhere the person
  // holding the box can read it. The window is short, user-initiated and local,
  // and the only secret crossing it is a Wi-Fi password.
  wifi_config_t ap = {};
  const size_t n = std::min(std::strlen(ap_ssid), kSsidFieldMax);
  std::memcpy(ap.ap.ssid, ap_ssid, n);
  ap.ap.ssid_len       = static_cast<uint8_t>(n);
  ap.ap.authmode       = WIFI_AUTH_OPEN;
  ap.ap.max_connection = kApMaxConn;
  ap.ap.channel        = kApChannel;
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), kTag, "ap_config");

  // Hand out ourselves as the DHCP-offered DNS server, so every lookup reaches
  // the responder above. The option only takes with dhcps stopped.
  esp_netif_ip_info_t ip_info = {};
  esp_netif_get_ip_info(s_ap_netif, &ip_info);
  s_ap_ip = ip_info.ip.addr;  // network byte order

  esp_netif_dhcps_stop(s_ap_netif);
  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
  esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
  dhcps_offer_t offer = OFFER_DNS;
  esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                         &offer, sizeof(offer));
  esp_netif_dhcps_start(s_ap_netif);

  if (start_dns() != ESP_OK) {
    ESP_LOGW(kTag, "DNS responder failed to start; the captive sheet may not auto-open");
  }

  s_ap_up = true;
  char ip[16] = {};
  esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
  ESP_LOGI(kTag, "setup AP '%s' up, form at http://%s/", ap_ssid, ip);
  return ESP_OK;
}

void stop_ap() {
  if (!s_ap_up) return;
  if (s_dns_run) {
    s_dns_run = false;
    if (s_dns_sock >= 0) {
      shutdown(s_dns_sock, SHUT_RDWR);  // unblock recvfrom so the task can exit
      close(s_dns_sock);
    }
  }
  s_ap_up = false;
  s_scan_n = 0;
  ESP_LOGI(kTag, "setup AP down; the config form stays up on the LAN");
}

bool ap_active() { return s_ap_up; }

void stop() {
  stop_ap();
  if (s_httpd != nullptr) {
    httpd_stop(s_httpd);
    s_httpd = nullptr;
  }
  s_cb = nullptr;
}

}  // namespace thermopost::cloud::portal
