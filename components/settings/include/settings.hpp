#pragma once

#include <cstddef>
#include <cstdint>

#include "alert_logic.hpp"
#include "esp_err.h"

// User-configurable device settings, persisted in NVS and edited from the MENU
// screens.
//
// Stored as individual NVS keys rather than one packed blob. A blob is fewer
// writes, but every future field addition then needs a version byte and a
// migration; per-key means an unknown key simply reads as absent and falls back
// to its default, which is the correct behavior for a firmware upgrade that
// adds a setting.
//
// Temperatures are hundredths of a degree Celsius everywhere, matching
// storage::TempRecord and alert::Thresholds — °F exists only at the point of
// display, so no stored value ever depends on the unit setting.
namespace thermopost::settings {

enum class TempUnit : uint8_t { Fahrenheit = 0, Celsius = 1 };

// What the post is allowed to do when nobody is looking at it. The ladder in the
// `power` component reads this; see power.hpp for what each rung costs.
//
// The middle one is the default because it is the only one that is right in both
// situations without being told which it is in: a post on a bench stays lit and
// answers instantly, and the same post on a battery in a tank room puts itself
// away. The other two exist because "which situation am I in" is inferred rather
// than known, and an operator who can see the post is a better judge than a
// voltage threshold.
enum class PowerMode : uint8_t {
  AlwaysOn = 0,         // never sleeps; on battery it simply runs until flat
  OnWhenConnected = 1,  // full behaviour on external power, ladder on battery
  AlwaysSleep = 2,      // sleeps regardless — wakes on touch, stays up, goes back
};

// --- Accepted ranges. Anything loaded from NVS or passed to a setter is clamped
// --- into these, so no downstream code has to defend against a wild value.
constexpr uint16_t kIntervalMinS = 10;    // below this the log grows absurdly fast
constexpr uint16_t kIntervalMaxS = 3600;  // an hour between samples
constexpr uint16_t kIntervalDefaultS = 60;

// The MCP9808's own operating range — a threshold outside it could never fire.
constexpr int16_t kTempFloorC100 = -4000;   // -40.00 °C
constexpr int16_t kTempCeilC100  = 12500;   // 125.00 °C

// Points plotted on the timeline. The 320 px-wide landscape layout leaves
// roughly 280 px of plot, so the default of 120 gives a little over two pixels
// per sample — dense enough to read a trend, sparse enough to stay legible.
// At the default one-minute cadence that is a two-hour window.
constexpr uint16_t kGraphPointsMin = 30;
constexpr uint16_t kGraphPointsMax = 480;
constexpr uint16_t kGraphPointsDefault = 120;

// How long records may queue before the radio comes up to send them — one figure for
// a post someone is looking at, one for a post whose screen has gone dark.
//
// Minutes rather than a record count, deliberately. A count would mean a different
// span on every post in a fleet, because the span is count x interval_s: "every 60
// records" is an hour at the default cadence and two and a half days at the slowest
// one. Minutes are what the setting is actually about.
//
// The split is on the idle tier, NOT on the supply, and that is the important part.
// power::on_external_power() is a heuristic — it cannot tell a just-unplugged pack
// from a charging one for minutes — while the tier is a fact. It is also the better
// question: what makes a radio session expensive is the post being otherwise idle,
// not where its current comes from. A lit post is burning tens of milliamps anyway
// and one more TLS handshake is marginal; a dark one is doing almost nothing else,
// so the handshake is the entire budget. Associating and completing that handshake
// costs far more than taking a reading, and the cost is per *session* rather than per
// POST — the uploader drains the whole queue once the link is up — so an hourly
// cadence is roughly a twelfth of the radio time of a five-minute one, and cloud.cpp's
// kBatchSize is already one hour of history per request.
//
// The lit figure defaults to zero — "send every record" — because a post someone is
// standing in front of should have a Sheet that matches the screen.
//
// The dark one defaults to a quarter of an hour. Zero would have been the
// change-nothing default, but it is the wrong one to ship: the dark cadence exists
// precisely for the deployment nobody is going to tune, and a fleet left at its
// defaults is a fleet spending its whole radio budget on a Sheet nobody is watching.
// Fifteen minutes is short enough that a post looks alive to someone checking on it
// and long enough to cut the sessions by more than an order of magnitude at the
// default record interval. Anything that actually needs to be seen sooner is flagged,
// and flagged records ignore this entirely.
//
// A record that breached the band or failed to read goes at once, whichever figure
// is in force. A saving that also delayed the alarm would be the one worth refusing.
constexpr uint16_t kUploadEveryDefaultMin = 0;
constexpr uint16_t kUploadDarkDefaultMin  = 15;
// A day. Longer is a Sheet nobody would trust to be current, and the backlog is
// safe on flash either way.
constexpr uint16_t kUploadEveryMaxMin = 1440;

// A deliberately tight band straddling room temperature, so both directions are
// reachable on a desk: cup the sensor in a hand to breach the maximum, and a cold
// drink or a puff of canned air breaches the minimum. That makes the alarm path,
// the record flags and the graph's envelope all testable in seconds rather than
// needing a tank.
//
// It follows that a device left on a normal desk will alarm — that is the point of
// these numbers, not an oversight. Expected to be the first thing changed for real
// use, and a wider 15..30 is the sane generic band to go back to.
constexpr int16_t kMinDefaultC100 = 2000;  // 20.00 °C
constexpr int16_t kMaxDefaultC100 = 2500;  // 25.00 °C
constexpr int16_t kHysteresisDefaultC100 = 50;  // 0.50 °C

// Full scale, named so the setter's clamp and the menu's stepper bound are the same
// number rather than two independent hundreds.
constexpr uint8_t kVolumeMaxPct = 100;
constexpr uint8_t kVolumeDefaultPct = 70;

// Idle seconds before the post sleeps, in the two modes that sleep at all. The
// dim and dark rungs are scaled to fit underneath it, so this one number is the
// whole of the policy rather than the third of three that have to agree.
//
// The floor is ten seconds because anything shorter sleeps while a hand is still
// moving towards the screen; the ceiling is an hour, which is "effectively awake"
// without needing a fourth mode to say so.
constexpr uint16_t kSleepAfterMinS = 10;
constexpr uint16_t kSleepAfterMaxS = 3600;
constexpr uint16_t kSleepAfterDefaultS = 60;

constexpr PowerMode kPowerModeDefault = PowerMode::OnWhenConnected;

// --- Post name -------------------------------------------------------------
// Thermoposts are deployed in groups, one per tank, and every upload is tagged
// with the post's name — a row in a shared Sheet is meaningless without knowing
// which tank produced it.
//
// 24 characters of printable ASCII. The length is the status strip's share of a
// 320 px line; the ASCII restriction is not stylistic. LVGL's built-in Montserrat
// carries 0x20-0x7F, ° and • and a fixed FontAwesome subset — an accented letter
// or an emoji renders as a blank box on the very screen the name exists to label.
// The name also has to survive slugification into an mDNS hostname later, which
// can't carry one either.
constexpr size_t kNameMaxLen = 24;

// An unnamed post falls back to Post-XXXX from the low bytes of its MAC: unique
// across a group with no user action, and it answers "which of these five
// identical boxes am I configuring right now" during provisioning.
constexpr const char* kNamePrefix = "Post-";

struct Settings {
  uint16_t interval_s;       // seconds between samples
  int16_t  min_c100;         // alarm below this
  int16_t  max_c100;         // alarm above this
  int16_t  hysteresis_c100;  // recovery margin; see alert_logic.hpp
  uint16_t graph_points;     // samples plotted on the timeline
  uint16_t upload_every_min; // while the screen is lit; 0 = every record
  uint16_t upload_dark_min;  // while the screen is dark or asleep; 0 = every record
  TempUnit unit;             // display only — never affects stored values
  bool     sound_enabled;    // master mute for the alarm tone
  uint8_t  volume_pct;       // 0..100
  PowerMode power_mode;      // what idling is allowed to do
  uint16_t sleep_after_s;    // idle seconds before sleeping, where sleeping happens
  char     name[kNameMaxLen + 1];  // never empty; see kNameMaxLen
};

// Load from NVS, applying defaults for absent keys and clamping anything out of
// range. Requires nvs_flash_init() to have run. Non-fatal: on failure the
// in-RAM defaults are used and the device still works, it just won't remember
// changes. Safe to call once; ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// Cheap snapshot of the current settings — served from RAM, no NVS access.
// Safe to call from the LVGL task on every frame.
Settings get();

// Setters write through to NVS immediately. The edit cadence is human-paced
// (a few taps in a settings screen), comfortably inside the part's wear budget.
// Each clamps its argument into the ranges above and is a no-op if the value is
// unchanged, so holding down a stepper doesn't burn a flash write per repeat.
esp_err_t set_interval_s(uint16_t v);
esp_err_t set_band(int16_t min_c100, int16_t max_c100);
esp_err_t set_hysteresis_c100(int16_t v);
esp_err_t set_graph_points(uint16_t v);
esp_err_t set_upload_every_min(uint16_t v);
esp_err_t set_upload_dark_min(uint16_t v);
esp_err_t set_unit(TempUnit u);
esp_err_t set_sound_enabled(bool on);
esp_err_t set_volume_pct(uint8_t pct);
esp_err_t set_power_mode(PowerMode m);
esp_err_t set_sleep_after_s(uint16_t v);

// Set the post name. Sanitizes rather than rejects — non-ASCII bytes dropped,
// surrounding spaces trimmed, truncated to kNameMaxLen — because a value arriving
// from a web form cannot be trusted to have honoured its own maxlength. An empty
// result restores the MAC-derived default, which is the useful reading of a blank
// submission.
//
// Deliberately NOT reachable from the device menu. It is the only free-text
// setting, and an on-screen keyboard on a 2.8" panel is the worst possible way to
// enter one; the web config form owns it — along with the network and the Sheet
// endpoint, and nothing else. Every setting above is a number or a toggle, which a
// touchscreen does well, and the menu is the only place they are edited.
esp_err_t set_name(const char* s);

// The alarm band in the form alert::step() wants. Already passed through
// alert::sanitize(), so callers get a set the state machine can always leave.
alert::Thresholds thresholds();

// Restore every setting to its default and persist. Backs a "reset settings"
// menu action and the `settings reset` serial command. The post name is the one
// exception and survives — it is identity, not a tunable.
esp_err_t reset_to_defaults();

// --- Display helpers. The only place the unit setting is allowed to matter. ---
inline float c100_to_f(int16_t c100) { return (static_cast<float>(c100) / 100.0f) * 1.8f + 32.0f; }
inline float c100_to_c(int16_t c100) { return static_cast<float>(c100) / 100.0f; }

// Render `c100` in the user's chosen unit. Returns the numeric value; the
// caller supplies the suffix (there is no single right format across the big
// readout, the threshold chips and the graph axis).
float display_value(int16_t c100);
const char* unit_suffix();

// --- Tenth-of-a-degree editing, for the menu's steppers ---------------------
// A stepper shows and steps tenths of a degree in the active unit while the
// stored value stays hundredths of a °C; these are that round trip. They live
// here for the same reason display_value() does — the unit setting is not allowed
// to leak out of this component.
//
// Tenths rather than whole degrees because the device menu is now the *only* way
// to set a threshold: the web config form carries the network and the post's
// identity and nothing else, so a resolution the menu cannot reach is a
// resolution the product does not have.
//
// A point on the scale and a span along it convert differently: 20 °C is 68 °F,
// but a span of 20 °C is a span of 36 °F. Feeding a hysteresis through the
// absolute pair would put it 32 degrees out, i.e. an alarm that can never clear,
// so the two are separate functions rather than one with a flag.
//
// Both directions round to nearest, and no two display steps land on the same
// stored value in either unit — 0.1 °C is exactly ten hundredths, 0.1 °F is about
// 5.6 — so a value edited in one unit and read back in the other does not drift.
int16_t display_tenths(int16_t c100);
int16_t c100_from_tenths(int16_t tenths);
int16_t display_span_tenths(int16_t c100);
int16_t c100_from_span_tenths(int16_t tenths);

}  // namespace thermopost::settings
