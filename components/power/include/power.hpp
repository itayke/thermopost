#pragma once

#include <cstdint>

#include "esp_err.h"

// What the post does when nobody is looking at it, and how much charge it has
// left to do it with.
//
// The policy is a ladder: full brightness while someone is interacting, dimmed
// after a while, panel dark after longer, and eventually deep sleep. Every rung
// below the first is invisible to the parts of the device that matter — the
// sensor keeps sampling, the recorder keeps writing and the alarm keeps watching
// on all of them. A dark post is still a working post; that is the whole point of
// blanking the panel rather than idling the device.
namespace thermopost::power {

enum class Tier : uint8_t {
  Active = 0,  // full brightness
  Dim,         // backlight down, screen still readable at arm's length
  Dark,        // panel blanked; everything else still running
  Asleep,      // deep sleep — see power.cpp, not reachable yet
};

// Configure the battery sense channel and start the ladder. Requires
// display::init() (the backlight is its LEDC channel) and reads alert::status(),
// which is safe before alert::init().
//
// Safe to call once; ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

Tier tier();

// Milliseconds since the last touch, as LVGL counts it.
uint32_t idle_ms();

// A battery reading, refreshed on the ladder's own tick.
//
// `pack_mv` is the number anyone actually wants and the one this board cannot
// yet produce: the wiki documents a divided battery voltage on GPIO9 but not the
// divider, so until the ratio is measured `calibrated` is false and `pack_mv` is
// zero. `pin_mv` and `raw` are honest at all times and are what the measurement
// is taken from — see the `power` serial command, which exists to make that
// capture a single line rather than an afternoon.
struct Battery {
  bool present;     // the ADC answered at all
  bool calibrated;  // the divider ratio is known, so pack_mv means something
  int  raw;         // the ADC's own counts
  int  pin_mv;      // millivolts at GPIO9, i.e. after the divider
  int  pack_mv;     // millivolts at the pack, or 0 while uncalibrated
};
Battery battery();

// Push the ladder back to the top — the effect a touch has, available to anything
// else that decides the screen has earned being lit.
void wake();

}  // namespace thermopost::power
