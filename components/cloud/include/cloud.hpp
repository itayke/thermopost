#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Network side of the post: the Wi-Fi link, the wall clock that rides in on it,
// the durable upload queue that mirrors every record into a Google Sheet, the
// always-on config form, and mDNS.
//
// The captive portal and the config form itself live in cloud_portal.cpp; this
// file owns the radio's lifecycle, so it is what decides when the setup AP is up
// and what the post is called on the network.
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

// The mDNS hostname, slugified from the post's name (24 chars) — dashes never
// add length, so the name's own limit bounds this one.
constexpr size_t kHostMax = 32;

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
  char      host[kHostMax];      // mDNS name, without the ".local" suffix
  bool      provisioning;        // the setup AP is up and serving the form

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
// The config form calls exactly this — the console command is not a lesser path,
// it is the same one.
esp_err_t set_credentials(const char* ssid, const char* password);

// Bring the setup AP back up on a post that is already provisioned, so its
// network can be changed from a phone without a serial cable. Backs the
// Network screen's "Setup Wi-Fi" and the `cloud portal` command. The AP
// comes down by itself on the next GOT_IP, exactly as it does at first boot.
esp_err_t start_provisioning();

// Take the setup AP back down without waiting for a connection — the Configure
// page's Back button, which should not leave an open network behind it. Leaves
// the config form serving on the LAN and touches no stored credentials. A no-op
// when the AP is not up.
void stop_provisioning();

// Erase the stored credentials and drop the link, settling to Disabled rather
// than reconnecting. Backs the Network screen's "Forget".
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

// Copy the stored endpoint URL into `out` (empty when unset). For the config
// form's prefill: the URL is long, awkward to retype and not a secret. The token
// has no equivalent and deliberately never will.
void endpoint(char* out, size_t cap);

// Rewind the high-water mark so every record still on disk is offered again.
// For a Sheet that was cleared or recreated — the device otherwise only ever
// sends above the mark and would never re-offer what it already sent. Safe to
// run against a populated Sheet: the Apps Script dedupes on device + index.
void resync();

// Tell the uploader a record was just appended. Lock-free (it sets an event
// bit), so `recorder` can call it straight from its own task.
//
// `urgent` skips settings::upload_every_min, which otherwise holds ordinary history
// back so the radio comes up on a slow cadence rather than once a minute. Pass it
// for a record the user would not accept hearing about an hour late — one that
// breached the band, or one the sensor failed to produce.
void notify_new_record(bool urgent = false);

}  // namespace thermopost::cloud
