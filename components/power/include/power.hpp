#pragma once

#include <cstdint>

#include "esp_err.h"

// What the post does when nobody is looking at it, and how much charge it has
// left to do it with.
//
// The policy is a ladder: full brightness while someone is interacting, dimmed
// after a while, panel dark after longer, and — once state survives a wake — deep
// sleep, which for now is only reachable by hand, through Turn Off. Every rung
// below the first is invisible to the parts of the device that matter — the
// sensor keeps sampling, the recorder keeps writing and the alarm keeps watching
// on all of them. A dark post is still a working post; that is the whole point of
// blanking the panel rather than idling the device.
namespace thermopost::power {

enum class Tier : uint8_t {
  Active = 0,  // full brightness
  Dim,         // backlight down, screen still readable at arm's length
  Dark,        // panel blanked; everything else still running
  Asleep,      // deep sleep — reached only by turn_off(); see power.cpp
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
  // 0..100, and an estimate rather than a measurement — lithium's voltage is
  // nearly flat through the middle of its range, so the curve behind this is a
  // published approximation and not a property of your pack. Good to about ten
  // points; useful as a bar, misleading as a number, which is why the main screen
  // draws it and only the Power page prints it beside the volts it came from.
  int  pct;
};
Battery battery();

// Does the ladder actually reach the fourth rung on its own? Deep sleep itself is
// wired — turn_off() below is one — but the post does not yet put *itself* to
// sleep, and the Power settings page says so on its footer rather than offering
// two modes that quietly do half of what they claim.
bool auto_sleep_armed();

// Is the post running on external power?
//
// A heuristic rather than a fact — see the threshold in power.cpp for what it
// keys off and the one window where it is wrong. Answers true before the first
// battery sample, which is the safe way round: it means a post never sleeps
// because it has not measured itself yet.
bool on_external_power();

// Shut down until someone touches the screen — the Power page's Turn Off button.
//
// Deep sleep with exactly one wake source, the touch controller's interrupt line.
// No timer, so a switched-off post stays off on USB and on battery alike and costs
// nothing to keep that way. Never returns.
//
// This needs none of the RTC-memory mirroring the *automatic* fourth rung waits
// on, and that is the whole reason it can exist first. Sleeping to save power has
// to come back as the same device it was; being switched off does not — the alarm
// state and the interval in progress are meant to be gone, and coming back is
// honestly a fresh boot.
//
// As off as software gets on this board, which is not all the way: without cutting
// the rail there is always residual draw, and how much depends on what else on the
// module stays awake.
[[noreturn]] void turn_off();

// Called first thing in app_main. Returns true when this boot is a wake from Turn
// Off rather than an ordinary one, and hands the touch interrupt pad back from the
// RTC domain so the touch driver can claim it normally.
bool resume_from_off();

// Push the ladder back to the top — the effect a touch has, available to anything
// else that decides the screen has earned being lit.
void wake();

// Report a touch that has just landed, and ask whether it should go no further.
//
// Returns true when the screen was not fully lit: the tap's only job was to bring
// it back, and delivering it as well would mean the first touch on a dimmed or
// dark post also presses whatever happened to be under the finger — on a screen
// nobody has read yet. Returns false when the post was already awake, where the
// touch is an ordinary one and belongs to the UI.
//
// Wakes the ladder either way, so ask once per press, on the press edge.
bool consume_touch();

}  // namespace thermopost::power
