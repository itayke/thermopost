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

// Deliberately a plain room-temperature band rather than something tuned to a
// specific use. It exists so a fresh device does something sensible on first
// boot and so the alarm path is exercisable out of the box — it is expected to
// be the first thing the user changes.
constexpr int16_t kMinDefaultC100 = 1500;  // 15.00 °C
constexpr int16_t kMaxDefaultC100 = 3000;  // 30.00 °C
constexpr int16_t kHysteresisDefaultC100 = 50;  // 0.50 °C

constexpr uint8_t kVolumeDefaultPct = 70;

// --- Post name -------------------------------------------------------------
// Thermo Posts are deployed in groups, one per tank, and every upload is tagged
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
  TempUnit unit;             // display only — never affects stored values
  bool     sound_enabled;    // master mute for the alarm tone
  uint8_t  volume_pct;       // 0..100
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
esp_err_t set_unit(TempUnit u);
esp_err_t set_sound_enabled(bool on);
esp_err_t set_volume_pct(uint8_t pct);

// Set the post name. Sanitizes rather than rejects — non-ASCII bytes dropped,
// surrounding spaces trimmed, truncated to kNameMaxLen — because a value arriving
// from a web form cannot be trusted to have honoured its own maxlength. An empty
// result restores the MAC-derived default, which is the useful reading of a blank
// submission.
//
// Deliberately NOT reachable from the device menu. It is the only free-text
// setting, and an on-screen keyboard on a 2.8" panel is the worst possible way to
// enter one; the web config form owns it. Every other setting is a number or a
// toggle and is editable from both.
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

}  // namespace thermopost::settings
