#pragma once

#include <cstdint>

// Alarm decision logic — when a reading counts as out-of-band, when it counts as
// recovered, and when the device should actually be shouting about it.
//
// **This file and alert_logic.cpp contain NO ESP-IDF dependency, by design.**
// They are compiled by the host test build (tests/host/) so this logic has a
// millisecond feedback loop instead of a flash-and-squint one. If a change here
// ever needs an IDF header, the change belongs in alert.cpp (the runtime wrapper
// that owns the tick, the speaker and the shared state) instead.
//
// Temperatures are in hundredths of a degree Celsius throughout, matching
// storage::TempRecord::temp_c100, so nothing here rounds or converts. Times are
// milliseconds on a monotonic clock — int64 rather than uint32 because a tank
// monitor outlives the 49 days a millisecond counter fits in 32 bits, and an
// alarm that mis-fires on the seventh week is the worst kind of bug to be handed.
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

// Alarm state plus the two ways the user can silence it, which are the two
// buttons on the main screen's alarm panel:
//
//   Off    — acknowledge(). Done with this excursion; stay quiet until the
//            temperature comes back or goes the other way.
//   Snooze — snooze(). Quiet for a fixed span, then say it again if it is still
//            wrong. The one to press when you are already dealing with it.
//
// Both are scoped to the current episode: any change of state clears them, so a
// *new* excursion always re-alerts however the last one was dismissed.
struct Status {
  State state;
  // Set by acknowledge().
  bool  acked;
  // Monotonic instant the snooze expires; 0 when not snoozed. Cleared by step()
  // once that instant passes rather than compared against the clock on every
  // read, which is what keeps should_alert() below free of a clock argument —
  // and means the expiry lands on the next tick, up to one tick late.
  int64_t snooze_until_ms;
};

inline constexpr Status kInitialStatus = {State::Normal, false, 0};

// Advance the alarm state by one reading. `now_ms` is a monotonic millisecond
// clock, used only to expire a snooze.
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
Status step(Status prev, int16_t temp_c100, bool valid, const Thresholds& t,
            int64_t now_ms);

// Silence the current episode for good — the "Off" button. A no-op while Normal:
// there is nothing to acknowledge, and pre-acking the next excursion would
// defeat the only job the alarm has. Also drops any snooze, since acknowledging
// is the stronger of the two and leaving a deadline behind it would mean the
// alarm had two reasons to be quiet and only one of them visible.
Status acknowledge(Status s);

// Silence the current episode for `for_ms` — the "Snooze" button. A no-op while
// Normal, for the same reason. A snooze that outlives its episode is not carried
// over: step() drops it the moment the state changes.
Status snooze(Status s, int64_t now_ms, int64_t for_ms);

// Is the alarm live and unsilenced right now?
//
// The one predicate behind both outputs: the panel on the main screen is up
// exactly when this is true, and the speaker sounds exactly when this is true
// *and* the user has not muted alarms in settings. The mute is applied by the
// caller rather than folded in here — a muted post still has to show the alarm,
// and a state machine that knew about the volume control would have to be asked
// twice to get two different answers.
bool should_alert(Status s);

// Is this state one the UI should render as an alarm (tint, panel)? True for
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
