#include "catch.hpp"  // main() lives in main.cpp

#include "alert_logic.hpp"

using namespace thermopost::alert;

namespace {

// 20.00 .. 25.00 °C with a 0.50 °C recovery margin. Everything below is written
// in hundredths so it reads the same as the stored record format.
constexpr Thresholds kBand = {2000, 2500, 50};

Status feed(Status s, const Thresholds& t, std::initializer_list<int16_t> temps) {
  for (int16_t v : temps) s = step(s, v, /*valid=*/true, t);
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Entering an alarm
// ---------------------------------------------------------------------------

TEST_CASE("a reading inside the band is Normal", "[state]") {
  const Status s = step(kInitialStatus, 2250, true, kBand);
  REQUIRE(s.state == State::Normal);
  REQUIRE_FALSE(should_sound(s));
}

TEST_CASE("crossing a limit alarms immediately, with no hysteresis delay", "[state]") {
  SECTION("above the maximum") {
    const Status s = step(kInitialStatus, 2501, true, kBand);
    REQUIRE(s.state == State::High);
    REQUIRE(should_sound(s));
  }
  SECTION("below the minimum") {
    const Status s = step(kInitialStatus, 1999, true, kBand);
    REQUIRE(s.state == State::Low);
    REQUIRE(should_sound(s));
  }
}

TEST_CASE("the limits themselves are inside the band", "[state][boundary]") {
  REQUIRE(step(kInitialStatus, 2500, true, kBand).state == State::Normal);
  REQUIRE(step(kInitialStatus, 2000, true, kBand).state == State::Normal);
}

// ---------------------------------------------------------------------------
// Hysteresis — the reason this module exists
// ---------------------------------------------------------------------------

TEST_CASE("an alarm holds until the reading clears the limit by the margin",
          "[hysteresis]") {
  Status s = step(kInitialStatus, 2600, true, kBand);  // High
  REQUIRE(s.state == State::High);

  // Back under the maximum, but not yet by the full 0.50 °C margin.
  s = step(s, 2499, true, kBand);
  REQUIRE(s.state == State::High);
  s = step(s, 2451, true, kBand);
  REQUIRE(s.state == State::High);

  // Exactly at the recovery point (max - hysteresis) clears it.
  s = step(s, 2450, true, kBand);
  REQUIRE(s.state == State::Normal);
}

TEST_CASE("a reading parked on the limit does not chatter", "[hysteresis]") {
  // The failure this prevents: sensor noise around 25.00 °C toggling the
  // speaker on and off every sample period. Once High, dithering across the
  // limit must stay High.
  Status s = step(kInitialStatus, 2502, true, kBand);
  REQUIRE(s.state == State::High);

  s = feed(s, kBand, {2499, 2501, 2498, 2500, 2497, 2503});
  REQUIRE(s.state == State::High);
}

TEST_CASE("the low alarm clears symmetrically", "[hysteresis]") {
  Status s = step(kInitialStatus, 1900, true, kBand);
  REQUIRE(s.state == State::Low);

  s = step(s, 2049, true, kBand);
  REQUIRE(s.state == State::Low);
  s = step(s, 2050, true, kBand);  // min + hysteresis
  REQUIRE(s.state == State::Normal);
}

TEST_CASE("zero hysteresis degrades to a bare threshold compare", "[hysteresis]") {
  constexpr Thresholds bare = {2000, 2500, 0};
  Status s = step(kInitialStatus, 2600, true, bare);
  REQUIRE(s.state == State::High);
  s = step(s, 2500, true, bare);
  REQUIRE(s.state == State::Normal);
}

TEST_CASE("a large swing crosses straight from High to Low", "[hysteresis]") {
  // Recovery must re-classify rather than assume Normal — a reading that
  // overshoots the whole band is a new alarm, not a recovery.
  Status s = step(kInitialStatus, 2600, true, kBand);
  REQUIRE(s.state == State::High);
  s = step(s, 1000, true, kBand);
  REQUIRE(s.state == State::Low);
  REQUIRE(should_sound(s));
}

// ---------------------------------------------------------------------------
// Acknowledgement
// ---------------------------------------------------------------------------

TEST_CASE("acknowledging silences the tone but keeps the alarm state", "[ack]") {
  Status s = acknowledge(step(kInitialStatus, 2600, true, kBand));
  REQUIRE(s.state == State::High);
  REQUIRE(is_alarm(s.state));   // the UI still shows it
  REQUIRE_FALSE(should_sound(s));  // the speaker does not
}

TEST_CASE("an acknowledgement survives further readings in the same episode",
          "[ack]") {
  Status s = acknowledge(step(kInitialStatus, 2600, true, kBand));
  s = feed(s, kBand, {2700, 2650, 2800});
  REQUIRE(s.state == State::High);
  REQUIRE_FALSE(should_sound(s));
}

TEST_CASE("a new episode re-alerts even if the previous one was acknowledged",
          "[ack]") {
  Status s = acknowledge(step(kInitialStatus, 2600, true, kBand));
  REQUIRE_FALSE(should_sound(s));

  s = step(s, 2200, true, kBand);  // recovered — episode over
  REQUIRE(s.state == State::Normal);

  s = step(s, 2600, true, kBand);  // a fresh excursion
  REQUIRE(s.state == State::High);
  REQUIRE(should_sound(s));
}

TEST_CASE("acknowledging while Normal does nothing", "[ack]") {
  // Otherwise a stray tap would pre-silence the next real alarm.
  const Status s = acknowledge(step(kInitialStatus, 2200, true, kBand));
  REQUIRE_FALSE(s.acked);

  const Status next = step(s, 2600, true, kBand);
  REQUIRE(should_sound(next));
}

// ---------------------------------------------------------------------------
// Sensor faults
// ---------------------------------------------------------------------------

TEST_CASE("an invalid reading becomes Fault and is user-visible", "[fault]") {
  const Status s = step(kInitialStatus, 0, /*valid=*/false, kBand);
  REQUIRE(s.state == State::Fault);
  REQUIRE(is_alarm(s.state));
  REQUIRE(should_sound(s));
}

TEST_CASE("a fault does not latch — the next valid reading is judged fresh",
          "[fault]") {
  Status s = step(kInitialStatus, 0, false, kBand);
  REQUIRE(s.state == State::Fault);

  s = step(s, 2200, true, kBand);
  REQUIRE(s.state == State::Normal);
}

TEST_CASE("a fault mid-alarm does not carry the alarm's hysteresis", "[fault]") {
  // Coming back from Fault, we have no evidence about which side of the band
  // the sensor left from, so the returning reading is classified plainly.
  Status s = step(kInitialStatus, 2600, true, kBand);
  REQUIRE(s.state == State::High);
  s = step(s, 0, false, kBand);
  REQUIRE(s.state == State::Fault);
  s = step(s, 2490, true, kBand);  // inside the band, but above max - hysteresis
  REQUIRE(s.state == State::Normal);
}

// ---------------------------------------------------------------------------
// sanitize() — configurations that would otherwise trap the state machine
// ---------------------------------------------------------------------------

TEST_CASE("an inverted band is swapped rather than left unsatisfiable",
          "[sanitize]") {
  const Thresholds t = sanitize({2500, 2000, 50});
  REQUIRE(t.min_c100 == 2000);
  REQUIRE(t.max_c100 == 2500);
}

TEST_CASE("hysteresis is capped at half the band", "[sanitize]") {
  // Uncapped, recovery would need temp <= 2500-400 = 2100 AND the low alarm
  // would need temp >= 2000+400 = 2400 — an alarm that can never clear.
  const Thresholds t = sanitize({2000, 2500, 400});
  REQUIRE(t.hysteresis_c100 == 250);
}

TEST_CASE("negative hysteresis is clamped to zero", "[sanitize]") {
  REQUIRE(sanitize({2000, 2500, -10}).hysteresis_c100 == 0);
}

TEST_CASE("an over-wide hysteresis still permits recovery", "[sanitize]") {
  // The property that actually matters: whatever the user types, there is some
  // reading that clears the alarm.
  const Thresholds raw = {2000, 2500, 9000};
  Status s = step(kInitialStatus, 3000, true, raw);
  REQUIRE(s.state == State::High);

  const Thresholds t = sanitize(raw);
  s = step(s, static_cast<int16_t>(t.max_c100 - t.hysteresis_c100), true, raw);
  REQUIRE(s.state == State::Normal);
}

TEST_CASE("a zero-width band is survivable", "[sanitize][boundary]") {
  // min == max: only one exact value is "inside". Degenerate, but it must not
  // wedge or divide by zero.
  const Thresholds t = sanitize({2200, 2200, 100});
  REQUIRE(t.hysteresis_c100 == 0);

  Status s = step(kInitialStatus, 2300, true, t);
  REQUIRE(s.state == State::High);
  s = step(s, 2200, true, t);
  REQUIRE(s.state == State::Normal);
}
