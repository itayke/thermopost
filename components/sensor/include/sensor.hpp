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

struct Reading {
  int16_t  temp_c100;  // °C × 100; kInvalidTempC100 when !valid
  bool     valid;      // false before the first sample and after a fault
  int64_t  uptime_us;  // esp_timer time this sample was taken; 0 if never
  uint32_t samples;    // successful samples since boot — proof it is still live
  uint16_t misses;     // consecutive failed reads; 0 whenever valid
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

// I²C address the sensor was found at, or 0 while it is missing.
uint8_t address();

}  // namespace thermopost::sensor
