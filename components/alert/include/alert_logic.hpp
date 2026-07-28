#pragma once

#include <cstdint>

// Alarm decision logic — when a reading counts as out-of-band, when it counts as
// recovered, and when the speaker should actually make noise.
//
// **This file and alert_logic.cpp contain NO ESP-IDF dependency, by design.**
// They are compiled by the host test build (tests/host/) so this logic has a
// millisecond feedback loop instead of a flash-and-squint one. If a change here
// ever needs an IDF header, the change belongs in alert.cpp (the wrapper that
// drives LVGL and the codec) instead.
//
// Temperatures are in hundredths of a degree Celsius throughout, matching
// storage::TempRecord::temp_c100, so nothing here rounds or converts.
namespace thermopost::alert {

enum class State : uint8_t {
  Normal = 0,  // inside the band
  High,        // above the maximum
  Low,         // below the minimum
  Fault,       // no valid reading (sensor missing, I²C error, conversion failed)
};

// The user-configured band. Held here rather than reached out of `settings` so
// the logic stays a pure function of its inputs and the host tests need no NVS.
struct Thresholds {
  int16_t min_c100;
  int16_t max_c100;
  // How far back inside the band a reading must come before an alarm clears.
  // Zero means a bare threshold compare, which chatters when the temperature
  // sits right on the limit — the reason this exists at all.
  int16_t hysteresis_c100;
};

// Alarm state plus whether the user has silenced the current episode.
struct Status {
  State state;
  // Set by acknowledge(); cleared automatically whenever `state` changes, so a
  // *new* excursion always re-alerts even if the previous one was silenced.
  bool  acked;
};

inline constexpr Status kInitialStatus = {State::Normal, false};

// Advance the alarm state by one reading.
//
// `valid` is false when the sample failed; the reading is then ignored and the
// state becomes Fault. A Fault does not "latch" — the next valid reading is
// evaluated from scratch, because a sensor that comes back is not evidence
// about which side of the band it came back on.
//
// Hysteresis applies only to LEAVING an alarm, never to entering one: crossing
// the limit alarms immediately, but recovery requires clearing the limit by
// `hysteresis_c100`. A reading can also go straight from High to Low (or back)
// without passing through Normal — a step that large is real and should re-alert
// rather than be smoothed over.
Status step(Status prev, int16_t temp_c100, bool valid, const Thresholds& t);

// Silence the audible alert for the current episode. A no-op while Normal —
// there is nothing to acknowledge, and pre-acking the next excursion would
// defeat the point of the alarm.
Status acknowledge(Status s);

// Should the speaker be sounding right now?
bool should_sound(Status s);

// Is this state one the UI should render as an alarm (tint, banner)? True for
// Fault as well as High/Low: a sensor that stopped answering is a condition the
// user needs to see, not a silent blank on the graph.
bool is_alarm(State s);

// Clamp a threshold set into a configuration the state machine can actually
// leave. Two ways a hand-entered band goes wrong:
//   * min above max — nothing is ever inside the band
//   * hysteresis wider than half the band — the recovery points cross over, so
//     an alarm can never clear and the device buzzes until it is power-cycled
// Both are the settings UI's job to prevent, but clamping here means the logic
// has no unreachable-recovery state regardless of what reaches it, including
// values restored from an older firmware's NVS.
Thresholds sanitize(Thresholds t);

}  // namespace thermopost::alert
