#pragma once

#include <cstdint>

#include "esp_err.h"

// MCP9808 surface-temperature sensor on the shared I²C bus.
//
// A task samples on a fixed cadence and publishes the result; consumers read
// the last value with latest(), which never blocks on the bus.
//
// That cadence is deliberately NOT settings::interval_s. Two different periods
// are hiding behind the word "sampling": how often the number on screen goes
// stale (fast, fixed, this component), and how often a reading is committed to
// the log and the Sheet (slow, user-configurable, the logger's business). Tying
// them together would mean the display freezes for a minute at the default
// interval, or the flash wears out at the display's rate.
namespace thermopost::sensor {

// Sentinel for `temp_c100` when there is no valid reading. Same value and same
// reasoning as storage::kInvalidTempC100 — the bottom of the int16 range can
// never collide with a real measurement from a part that spans -40..+125 °C.
constexpr int16_t kInvalidTempC100 = INT16_MIN;

// How often the task samples, and so how fresh the number on screen is.
//
// Public rather than private because `recorder` has to tick faster than this to
// fold every reading into its interval's min/max, and it derives its own period
// from this one. Two constants in two files that must stay in a ratio, with
// nothing enforcing it, is how the envelope quietly starts missing the spikes it
// exists to catch — recorder.cpp static_asserts the relationship.
//
// Floored by the part, not by taste: at the configured 0.0625 °C resolution a
// conversion takes 250 ms, so reading faster than that just re-reads the same
// register. Raising it is free but buys nothing while the screen is on — the
// MCP9808 converts continuously regardless of how often it is asked, and the
// read itself is about a millisecond of CPU on a SoC that is awake anyway.
constexpr uint32_t kSamplePeriodMs = 1000;

struct Reading {
  int16_t  temp_c100;  // °C × 100; kInvalidTempC100 when !valid
  bool     valid;      // false before the first sample and after a fault
  int64_t  uptime_us;  // esp_timer time this sample was taken; 0 if never
  uint32_t samples;    // successful samples since boot — proof it is still live
  uint16_t misses;     // consecutive failed reads; 0 whenever valid
  // The part's OWN verdict on the band, from the alert flags it packs into every
  // ambient read — not this firmware's. False until set_alert_window() has been
  // called, and false whenever !valid. See set_alert_window().
  bool     alert;
};

// Probe the bus, identify the part by its manufacturer ID, configure it and
// start the sampling task. Requires board_i2c::init() to have run.
//
// Returns ESP_ERR_NOT_FOUND if no MCP9808 answered — non-fatal, and the task
// still starts: the sensor is on a flying lead, so it is worth recovering from
// a reseated connector without a reboot. Callers should log and carry on.
// ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// The most recent sample. Cheap and lock-free enough to call from the LVGL task
// on every frame.
Reading latest();

// Program the part's own comparator with the alarm band.
//
// This is what lets the post sleep between records without going blind between
// them. The MCP9808 compares every conversion against these limits in its own
// silicon, at roughly 200 µA, whether or not the SoC is running, and drives its
// ALERT pin (board::kSensorAlert) when the reading leaves the window — within one
// 250 ms conversion, which is sooner than this firmware notices while fully awake.
//
// The hardware window is a trigger to wake and look, never the alarm decision: it
// carries no hysteresis, and alert_logic still makes the call. The limits are
// rounded INWARD so the window can only ever sit inside the software band and trip
// early; a false wake is cheap, and a missed one is the whole thing being prevented.
//
// Queued rather than written here — the sampling task owns the device handle, and
// applies this on its next tick and again after any re-probe, because a reseated
// sensor comes back at its power-on defaults. Cheap to call repeatedly.
void set_alert_window(int16_t min_c100, int16_t max_c100);

// I²C address the sensor was found at, or 0 while it is missing.
uint8_t address();

}  // namespace thermopost::sensor
