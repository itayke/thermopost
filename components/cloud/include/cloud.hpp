#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Network side of the post: the Wi-Fi link, the wall clock that rides in on it,
// and — as M8 lands — the durable upload queue that mirrors every record into a
// Google Sheet, the always-on config form, and mDNS.
//
// **The link, the clock and the uploader exist today.** Still to come in this
// milestone: the captive portal that replaces the console as the way credentials
// arrive, the always-on config form, and mDNS. They build on top of this file
// rather than beside it.
//
// Why the clock lives here at all: this board has no battery-backed RTC (see
// storage.hpp), so SNTP over Wi-Fi is the *only* source of wall-clock time.
// Until it lands, every record carries epoch_s = 0 and the screens read "clock
// not set". Bringing up the radio is therefore the same act as starting the
// clock, and splitting them into two components would give the second nothing
// to own.
//
// Shaped like the other services here: non-fatal init(), background work off the
// LVGL core, and a mutex-guarded status snapshot the UI polls.
namespace thermopost::cloud {

// Sized by the 802.11 fields themselves, plus a terminator the driver's own
// (unterminated, fixed-width) buffers do not carry.
constexpr size_t kSsidMax     = 33;  // 32 bytes + NUL
constexpr size_t kPasswordMax = 65;  // 64 bytes + NUL
constexpr size_t kIpTextMax   = 16;  // "255.255.255.255" + NUL

// An Apps Script /exec URL runs to about 120 characters; the rest is headroom.
constexpr size_t kUrlMax   = 160;
constexpr size_t kTokenMax = 64;

enum class WifiState : uint8_t {
  Disabled,    // no credentials stored; the radio is idle and will stay idle
  Connecting,  // associating, or awaiting an IP
  Connected,   // associated with an address
  Offline,     // credentials stored, link down, still retrying in the background
};

// `Offline` is not a terminal state and does not mean the device gave up — see
// the reconnect policy in cloud.cpp. It exists so a screen can say "offline"
// after a couple of failed attempts instead of saying "connecting" for an hour.

enum class SyncState : uint8_t {
  Idle,     // nothing queued, or no link / no endpoint to send it to
  Syncing,  // a POST is in flight
  Backoff,  // the last POST failed; waiting before the retry
};

struct Status {
  WifiState wifi;
  bool      time_synced;         // SNTP has set the system clock at least once
  int8_t    rssi_dbm;            // last AP RSSI; 0 when not associated
  char      ssid[kSsidMax];      // network joined while Connected; "" otherwise
  char      ip[kIpTextMax];      // address held while Connected; "" otherwise

  SyncState sync;
  bool      endpoint_set;        // a /exec URL is stored (the token has a default)
  uint32_t  uploaded;            // the high-water mark: absolute index of the
                                 // next record to send, i.e. how many are done
  uint32_t  pending;             // records written but not yet accepted
  esp_err_t last_error;          // last upload failure; ESP_OK if none
};

// Bring up netif, the default event loop and Wi-Fi STA, then auto-connect if
// credentials are already stored. Registers the `cloud` serial command if the
// shell is up. Non-fatal — a post with no network still samples, logs and
// alarms; it just cannot timestamp or upload.
//
// Requires nvs_flash_init() to have run: the driver keeps credentials in NVS
// itself (WIFI_STORAGE_FLASH), which is why there is no credential key of our
// own anywhere in this component.
//
// Safe to call once; ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// Mutex-guarded snapshot for the UI. Cheap; safe to call from the LVGL task.
Status status();

// Store credentials and (re)connect with them. Both are truncated to the fields
// above rather than rejected, matching settings::set_name(): a value arriving
// from a form or a console line is not to be trusted to have honoured its own
// limit. `password` may be null or empty for an open network.
//
// The captive portal in a later step of this milestone calls exactly this — the
// console command is not a lesser path, it is the same one.
esp_err_t set_credentials(const char* ssid, const char* password);

// Erase the stored credentials and drop the link, settling to Disabled rather
// than reconnecting. Backs the Connections screen's "Forget Network".
void forget();

// --- The Sheet endpoint -----------------------------------------------------
// Both persist immediately and take effect on the next batch. The URL must be
// https — Apps Script only answers there, and an http:// endpoint would be a
// typo that fails once a minute forever rather than at the moment it is set.
//
// A device ships with a default token, so a fresh post syncs once the URL alone
// is set. That token is a soft gate and not a secret: it is compiled into the
// firmware and crosses the wire in the clear. Set your own here and match it in
// the Apps Script if the Sheet needs real access control.
esp_err_t set_endpoint(const char* url);
esp_err_t set_token(const char* token);

// Rewind the high-water mark so every record still on disk is offered again.
// For a Sheet that was cleared or recreated — the device otherwise only ever
// sends above the mark and would never re-offer what it already sent. Safe to
// run against a populated Sheet: the Apps Script dedupes on device + index.
void resync();

// Tell the uploader a record was just appended. Lock-free (it sets an event
// bit), so `recorder` can call it straight from its own task.
void notify_new_record();

}  // namespace thermopost::cloud
