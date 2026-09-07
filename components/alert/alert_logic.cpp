#include "alert_logic.hpp"

// IDF-free by design — see the header. Nothing in this file may include an
// ESP-IDF, FreeRTOS or LVGL header; the host test build compiles it directly
// with the system compiler and will break loudly if that rule is broken.

namespace thermopost::alert {
namespace {

// Where a reading sits relative to the band, ignoring the current state. This is
// the "entering" test — bare comparisons, no hysteresis.
State classify(int16_t temp_c100, const Thresholds& t) {
  if (temp_c100 > t.max_c100) return State::High;
  if (temp_c100 < t.min_c100) return State::Low;
  return State::Normal;
}

}  // namespace

Thresholds sanitize(Thresholds t) {
  // A band with min above max can never be satisfied. Swapping is the least
  // surprising repair — it preserves both numbers the user typed and yields the
  // band they almost certainly meant.
  if (t.min_c100 > t.max_c100) {
    const int16_t tmp = t.min_c100;
    t.min_c100 = t.max_c100;
    t.max_c100 = tmp;
  }

  if (t.hysteresis_c100 < 0) t.hysteresis_c100 = 0;

  // Recovery happens at (max - hyst) and (min + hyst). If those cross, a reading
  // can satisfy neither and the alarm never clears. Capping hysteresis at half
  // the band makes the two recovery points meet at worst, never invert.
  const int32_t span = static_cast<int32_t>(t.max_c100) - t.min_c100;
  const int32_t cap  = span / 2;
  if (t.hysteresis_c100 > cap) t.hysteresis_c100 = static_cast<int16_t>(cap);

  return t;
}

Status step(Status prev, int16_t temp_c100, bool valid, const Thresholds& raw,
            int64_t now_ms) {
  const Thresholds t = sanitize(raw);

  State next;
  if (!valid) {
    next = State::Fault;
  } else if (prev.state == State::High) {
    // Hold High until the reading clears the maximum by the hysteresis margin.
    // Re-classify rather than dropping straight to Normal, so a plunge all the
    // way past the minimum lands in Low instead of being reported as recovered.
    next = (temp_c100 <= t.max_c100 - t.hysteresis_c100)
               ? classify(temp_c100, t)
               : State::High;
  } else if (prev.state == State::Low) {
    next = (temp_c100 >= t.min_c100 + t.hysteresis_c100)
               ? classify(temp_c100, t)
               : State::Low;
  } else {
    // Normal or Fault — no episode in progress to hold, so classify fresh.
    next = classify(temp_c100, t);
  }

  // Both silences belong to one episode, not to the alarm generally. Any change
  // of state ends that episode, so they are dropped and the next excursion says
  // so again.
  const bool same_episode = next == prev.state;

  Status out;
  out.state = next;
  out.acked = prev.acked && same_episode;
  // One comparison covers both ways a snooze stops applying: an expired deadline
  // and a never-set one (zero, which no monotonic `now` is ever below).
  out.snooze_until_ms =
      (same_episode && now_ms < prev.snooze_until_ms) ? prev.snooze_until_ms : 0;
  return out;
}

Status acknowledge(Status s) {
  if (s.state == State::Normal) return s;
  s.acked = true;
  // See the header: the stronger silence absorbs the weaker one rather than
  // sitting on top of a deadline nobody can see.
  s.snooze_until_ms = 0;
  return s;
}

Status snooze(Status s, int64_t now_ms, int64_t for_ms) {
  if (s.state == State::Normal) return s;
  s.snooze_until_ms = now_ms + for_ms;
  return s;
}

bool is_alarm(State s) { return s != State::Normal; }

bool should_alert(Status s) {
  return is_alarm(s.state) && !s.acked && s.snooze_until_ms == 0;
}

}  // namespace thermopost::alert
