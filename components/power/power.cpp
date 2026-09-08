#include "power.hpp"

#include <cinttypes>
#include <cstdio>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "alert.hpp"
#include "settings.hpp"
#include "board_pins.hpp"
#include "display.hpp"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_console.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "sdkconfig.h"

namespace thermopost::power {
namespace {

constexpr const char* kTag = "power";

constexpr uint32_t kMsPerSec = 1000;

// Fast enough that a tap lights the screen before the finger leaves it. The tick
// itself is an integer read and, at most, one LEDC write — the cost of polling
// this often is not the work, it is the LVGL lock, and that is held for the one
// call that reads the inactivity counter.
constexpr uint32_t kTickMs = 200;

constexpr uint32_t kDimAfterMs  = CONFIG_THERMOPOST_POWER_DIM_SECONDS * kMsPerSec;
constexpr uint32_t kDarkAfterMs = CONFIG_THERMOPOST_POWER_DARK_SECONDS * kMsPerSec;
static_assert(kDimAfterMs < kDarkAfterMs,
              "the dark tier must come after the dim one, or dim is unreachable");

constexpr uint8_t kActivePct = 100;
constexpr uint8_t kDimPct    = CONFIG_THERMOPOST_POWER_DIM_PERCENT;

// --- Battery sense ----------------------------------------------------------
// GPIO9 is ADC1_CH8. 12 dB of attenuation because a single Li-ion cell tops out
// at 4.2 V and any plausible divider still leaves more than the 1.1 V the lowest
// attenuation would cover — being able to read the number matters more here than
// the last few counts of resolution.
constexpr adc_unit_t    kAdcUnit    = ADC_UNIT_1;
constexpr adc_channel_t kAdcChannel = ADC_CHANNEL_8;
constexpr adc_atten_t   kAdcAtten   = ADC_ATTEN_DB_12;
static_assert(board::kBatteryAdc == 9,
              "battery sense moved off GPIO9, so ADC1_CH8 above is no longer it");

// Pack millivolts per thousand millivolts at the pin. Two equal resistors, so
// the ratio is 2.
//
// INFERRED, not measured with a meter — and the inference is sound rather than
// convenient. A working 1S pack read 2012 mV at the pin, and only a ratio of 2
// puts that anywhere a lithium cell can be: 4.02 V. The next divider anyone fits
// would say 3.02 V (a nearly flat cell, which would not have been running the
// board) or 6.04 V (a voltage this chemistry does not reach). One meter reading
// across the pack terminals against `power`'s pin mV confirms it, and if it
// disagrees this is the only line that changes.
constexpr int kDividerPerMille = 2000;

// When to treat the post as running on external power. TWO thresholds, not one,
// and the gap between them is the whole point.
//
// This is the FALLBACK signal, and it is only consulted when the USB monitor below
// says nothing. The TP4054 holds a charging pack at its 4.2 V constant-voltage
// point and leaves it near there after termination, while a pack under load falls
// away — so a threshold separates the two, badly.
//
// It has been wrong twice, in opposite ways, and both are worth keeping written
// down. A single threshold chattered: a pack resting near the line crossed it back
// and forth on ADC noise and on the load steps the backlight and the radio make, so
// the charge bolt blinked. Then a hysteresis band from 4.05 to 4.15 latched: a pack
// an hour off the charger sits around 4.10, which is above the falling edge, so the
// state could never leave "external" at all. The band has to sit *between* the two
// states, and the second attempt put it on top of one of them.
//
// 4.16 rising is under the TP4054's worst-case low CV point, so a charger at the
// bottom of its tolerance still registers; 4.12 falling is high enough that a
// discharging pack clears it in minutes rather than hours.
//
// It still cannot be quick, and no threshold can: unplugging a full pack leaves it
// above the falling edge until the load drags it down. That lag belongs to the
// signal. The only real fix is a wire from VBUS to a spare expansion pin.
constexpr int kExternalRiseMv = 4160;
constexpr int kExternalFallMv = 4120;
static_assert(kExternalFallMv < kExternalRiseMv,
              "the external-power thresholds are inverted, which is worse than having "
              "only one: the state would flip on every sample");

// Reads averaged per sample. The ESP32-S3's SAR is noisy enough at 12 dB that a
// single conversion wanders by more than a millivolt or two, and the divider
// doubles whatever it wanders by. Sixteen reads cost microseconds on a tick that
// happens five times a second.
constexpr int kAdcSamples = 16;

// The other half of the answer, and the better half when it applies.
//
// IDF's USB monitor watches for start-of-frame packets on a FreeRTOS tick hook, so
// it knows within milliseconds whether a USB *host* is on the other end of the
// cable — exact and instant, where the pack voltage is neither. Its limit is stated
// in its own documentation: a power bank or a wall charger sends no SOF packets and
// "will never be considered as connected". So it is authoritative when true and
// silent when false, which is why the pack voltage is still consulted underneath
// it rather than replaced by it.
//
// Merely calling it is what links the monitor in — the object file carries an
// ESP_SYSTEM_INIT_FN that registers the tick hook — and it reports connected until
// proven otherwise, which is the safe way round for something that gates sleep.
bool usb_host_present() { return usb_serial_jtag_is_connected(); }

// The FT6336 pulls its interrupt line low on a touch and leaves it high at rest —
// the same polarity the touch driver's own config states.
constexpr int kTouchIntActiveLevel = 0;

// --- State of charge --------------------------------------------------------
// A published approximation of a lithium cell's resting curve, not a measurement
// of this pack. It exists because voltage alone is unreadable at a glance and a
// linear map would be a lie: 4.2 to 3.9 is the first quarter of the charge, then
// the cell sits near 3.7 for most of its life and falls off a cliff. Interpolating
// between these points reproduces that shape well enough for a bar, which is why
// the bar is what the main screen draws.
//
// The ends are the parts that are not approximate: 4.2 V is the TP4054's own
// constant-voltage setpoint, fixed in silicon, and 3.3 V is where a 1S pack under
// load is done. Neither had to be measured.
struct CurvePoint { int mv; int pct; };
constexpr CurvePoint kSocCurve[] = {
    {4200, 100}, {3900, 75}, {3700, 50}, {3600, 25}, {3300, 0},
};
constexpr int kSocPoints = static_cast<int>(sizeof(kSocCurve) / sizeof(kSocCurve[0]));

// --- Turning off ------------------------------------------------------------
// Off wakes on a touch, and on nothing else.
//
// The first version polled the pack every thirty seconds and came back when the
// voltage said a charger was there. That was wrong in both directions on USB,
// because it tested a level where the question is an edge: a pack that is being
// charged is a moving target, so switching off with a part-charged pack plugged in
// meant the post turned itself back on partway through the charge. The pack simply
// cannot answer "has a charger just appeared" — only a wire from VBUS to a spare
// expansion pin could, and this board has none.
//
// Touch is better than the behaviour it replaces on every axis that matters: off
// means off on USB and on battery alike, there is no polling so no wake current at
// all, and "touch the screen to turn it back on" is a shorter sentence than the one
// it replaces.
//
// IO17 is the FT6336's interrupt line — active low, idle high, and RTC-capable, so
// ext0 can watch it while everything else is stopped. The touch driver runs in
// polling mode and never claims it (int_gpio_num = GPIO_NUM_NC), so there is
// nothing to hand back but the pad itself.
// RTC memory survives deep sleep but is uninitialised after a power-on reset, so
// it is checked against a magic AND against the reset reason. Either alone would
// eventually read garbage as "the user switched this off", which is a post that
// refuses to boot.
constexpr uint32_t kOffMagic = 0x0FF0FF01;
RTC_DATA_ATTR uint32_t s_off_magic;

// Where the dim and dark rungs land when a sleep deadline is in play. The ladder
// is then scaled to fit underneath it instead of using the Kconfig delays, so one
// number — settings::sleep_after_s — is the whole policy rather than the third of
// three that all have to agree. A 60 s sleep dims at 20 and darkens at 40; an
// hour-long one lands close to the Kconfig defaults anyway.
constexpr uint32_t kDimFractionNum  = 1;
constexpr uint32_t kDarkFractionNum = 2;
constexpr uint32_t kFractionDen     = 3;

// --- Deep sleep -------------------------------------------------------------
// The fourth rung is deliberately unreachable, and this is the gate rather than
// missing code so the reason travels with it. Two things have to land first:
//
//   * the divider above, because sleeping is only correct on battery and USB
//     presence is not detectable on this board until the ADC can tell a charging
//     pack from a full one;
//   * RTC-memory mirroring for the alarm's Status and the recorder's in-flight
//     interval, because deep sleep loses RAM — and an acknowledged alarm that
//     forgets it was acknowledged would re-alert on every wake, which turns the
//     Off button into a button that does nothing.
//
// The graph's ring needs no mirroring: recorder::init() already backfills it from
// the log, so a wake rebuilds it exactly as a cold boot does.
constexpr bool kDeepSleepReady = false;

constexpr int kTaskStack = 3072;
// Below the alarm and the sampler, above nothing in particular: a late tier
// change is a screen that dims a fraction of a second past its cue, which is not
// a thing anyone can see.
constexpr int kTaskPrio = 3;
constexpr int kTaskCore = 0;

SemaphoreHandle_t s_mutex = nullptr;
bool s_inited = false;

adc_oneshot_unit_handle_t s_adc  = nullptr;
adc_cali_handle_t         s_cali = nullptr;

// Guarded by s_mutex.
Tier     s_tier = Tier::Active;
Battery  s_battery = {};
uint32_t s_idle_ms = 0;
bool     s_external = true;  // assumed until the first sample says otherwise

// Has a USB *host* ever answered since boot?
//
// Sticky, and it is what makes unplugging register at all. The pack cannot answer
// this question: a just-unplugged pack sits around 4.18 V and a charging one at
// 4.20, twenty millivolts apart and under the noise floor, so no threshold
// separates "on the charger" from "off it ten seconds ago". Three different
// thresholds were tried before that was obvious.
//
// But a board that has seen a host is a board where the host signal works, and
// there it is exact in both directions and instant. So the voltage path stops being
// consulted the moment something better exists, and remains for posts that only
// ever meet dumb chargers.
//
// Known limit: a post moved from a computer to a wall charger within one boot reads
// as unplugged, because the sticky flag outlives the cable. A reboot clears it, and
// the wire from VBUS would retire the whole question.
bool     s_ever_saw_host = false;
// When something that is not a touch last insisted the device was in use — a
// serial `power wake`, or anything else that decides the screen has earned being
// lit. LVGL's own counter cannot be reset from outside without faking an input
// event, so the two are combined instead: whichever says the device was used more
// recently wins.
int64_t s_woke_at_ms = 0;

struct Lock {
  bool ok;
  explicit Lock(TickType_t t = portMAX_DELAY) : ok(xSemaphoreTake(s_mutex, t) == pdTRUE) {}
  ~Lock() { if (ok) xSemaphoreGive(s_mutex); }
};

const char* tier_name(Tier t) {
  switch (t) {
    case Tier::Dim:    return "dim";
    case Tier::Dark:   return "dark";
    case Tier::Asleep: return "asleep";
    default:           return "active";
  }
}

// LVGL owns the only inactivity counter worth having: it is reset by the indev
// layer on a real press, so it counts taps rather than interrupt edges and it
// does not need this component to know anything about the touch controller.
//
// Returns false when the lock could not be taken quickly. The caller then leaves
// the tier alone for one tick, which is the right failure: a busy UI is not an
// idle device, and blanking the panel because LVGL was mid-redraw would be the
// exact opposite of what the counter is measuring.
bool read_idle_ms(uint32_t* out) {
  if (!display::lock(kTickMs)) return false;
  *out = lv_display_get_inactive_time(nullptr);
  display::unlock();
  return true;
}

void apply(Tier t) {
  switch (t) {
    case Tier::Active:
      display::set_on(true);
      display::set_brightness(kActivePct);
      break;
    case Tier::Dim:
      display::set_on(true);
      display::set_brightness(kDimPct);
      break;
    case Tier::Dark:
    case Tier::Asleep:
      // Blanked rather than dimmed to zero: set_on(false) stops the panel
      // scanning as well as killing the backlight, and the framebuffer survives,
      // so coming back is a repaint of what was already there.
      display::set_on(false);
      break;
  }
}

// Does this mode, on this supply, ever put the post to sleep?
bool sleeps(settings::PowerMode mode, bool external) {
  switch (mode) {
    case settings::PowerMode::AlwaysOn:    return false;
    case settings::PowerMode::AlwaysSleep: return true;
    default:                               return !external;
  }
}

Tier tier_for(uint32_t idle, bool alarm_active, settings::PowerMode mode, bool external,
              uint32_t sleep_after_ms) {
  // An alarm outranks the whole ladder, and lights the screen rather than merely
  // holding it: the panel it raises is useless on a dark display, and a post that
  // beeps in the dark makes someone hunt for a device instead of reading it.
  if (alarm_active) return Tier::Active;

  if (!sleeps(mode, external)) {
    // Nothing to fit underneath, so the rungs are the Kconfig ones and the ladder
    // simply stops at Dark.
    if (idle >= kDarkAfterMs) return Tier::Dark;
    if (idle >= kDimAfterMs) return Tier::Dim;
    return Tier::Active;
  }

  if (idle >= sleep_after_ms) return kDeepSleepReady ? Tier::Asleep : Tier::Dark;
  if (idle >= sleep_after_ms * kDarkFractionNum / kFractionDen) return Tier::Dark;
  if (idle >= sleep_after_ms * kDimFractionNum / kFractionDen) return Tier::Dim;
  return Tier::Active;
}

int percent_for(int pack_mv) {
  if (pack_mv >= kSocCurve[0].mv) return 100;
  for (int i = 1; i < kSocPoints; ++i) {
    if (pack_mv >= kSocCurve[i].mv) {
      const CurvePoint& hi = kSocCurve[i - 1];
      const CurvePoint& lo = kSocCurve[i];
      const int span_mv = hi.mv - lo.mv;
      return lo.pct + (pack_mv - lo.mv) * (hi.pct - lo.pct) / span_mv;
    }
  }
  return 0;
}

void sample_battery(Battery* out) {
  out->present = false;
  out->calibrated = kDividerPerMille > 0;
  out->raw = 0;
  out->pin_mv = 0;
  out->pack_mv = 0;
  out->pct = 0;

  if (s_adc == nullptr) return;

  int sum = 0;
  int taken = 0;
  for (int i = 0; i < kAdcSamples; ++i) {
    int raw = 0;
    if (adc_oneshot_read(s_adc, kAdcChannel, &raw) != ESP_OK) continue;
    sum += raw;
    ++taken;
  }
  if (taken == 0) return;
  out->raw = sum / taken;
  out->present = true;

  if (s_cali != nullptr) {
    adc_cali_raw_to_voltage(s_cali, out->raw, &out->pin_mv);
  }
  if (out->calibrated) {
    out->pack_mv = out->pin_mv * kDividerPerMille / 1000;
    out->pct = percent_for(out->pack_mv);
  }
}

void task(void*) {
  TickType_t last = xTaskGetTickCount();
  Tier applied = Tier::Active;
  bool s_last_logged_external = !s_external;  // force the first state to be logged
  apply(applied);

  for (;;) {
    xTaskDelayUntil(&last, pdMS_TO_TICKS(kTickMs));

    uint32_t idle = 0;
    if (!read_idle_ms(&idle)) continue;

    const bool alarm_active = alert::should_alert(alert::status());

    Battery b;
    sample_battery(&b);

    const settings::Settings cfg = settings::get();
    const uint32_t sleep_after_ms = static_cast<uint32_t>(cfg.sleep_after_s) * kMsPerSec;
    // Once a USB host has ever answered on this cable, the host signal is the only
    // one worth listening to — in both directions. See kEverSawHost's note.
    // Otherwise fall back to the pack, with hysteresis, holding the previous answer
    // inside the band and holding it too when there is no usable reading at all: a
    // post that cannot measure itself must not conclude it is unplugged and put
    // itself to sleep.
    const bool host = usb_host_present();
    if (host) s_ever_saw_host = true;

    bool external = s_external;
    if (s_ever_saw_host) {
      external = host;
    } else if (b.present && b.calibrated) {
      external = s_external ? b.pack_mv >= kExternalFallMv : b.pack_mv >= kExternalRiseMv;
    }

    Tier want;
    {
      Lock l;
      const int64_t now = esp_timer_get_time() / 1000;
      const uint32_t since_wake = static_cast<uint32_t>(now - s_woke_at_ms);
      if (since_wake < idle) idle = since_wake;
      s_idle_ms = idle;
      s_battery = b;
      s_external = external;
      want = tier_for(idle, alarm_active, cfg.power_mode, external, sleep_after_ms);
      s_tier = want;
    }

    // Logged on the transition, so the console shows the moment a cable changed
    // rather than requiring the screen to be watched.
    if (external != s_last_logged_external) {
      s_last_logged_external = external;
      ESP_LOGI(kTag, "external power %s (usb host %s, pack %d mV)",
               external ? "yes" : "no", host ? "yes" : "no", b.pack_mv);
    }

    if (want != applied) {
      ESP_LOGI(kTag, "%s -> %s (idle %" PRIu32 " s)", tier_name(applied),
               tier_name(want), idle / kMsPerSec);
      apply(want);
      applied = want;
    }
  }
}

// --- The serial command -----------------------------------------------------
// Exists mostly to be the instrument the divider is measured with: a line that
// can be read off the console while a meter sits on the pack terminals, rather
// than a debug log someone has to go and add.
int cmd_power(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "wake") {
    wake();
    std::printf("woken\n");
    return 0;
  }

  const Battery b = battery();
  std::printf("tier      %s\n", tier_name(tier()));
  std::printf("idle      %" PRIu32 " s (dim at %" PRIu32 ", dark at %" PRIu32 ")\n",
              idle_ms() / kMsPerSec, kDimAfterMs / kMsPerSec, kDarkAfterMs / kMsPerSec);
  std::printf("deep sleep %s\n", kDeepSleepReady ? "armed" : "not wired yet");

  if (!b.present) {
    std::printf("battery   no answer from ADC1_CH8 (GPIO%d)\n", board::kBatteryAdc);
    return 0;
  }
  std::printf("usb host  %s\n", usb_host_present() ? "yes" : "no");
  std::printf("external  %s\n", on_external_power() ? "yes" : "no");
  std::printf("battery   raw %d counts, %d mV at the pin\n", b.raw, b.pin_mv);
  if (b.calibrated) {
    std::printf("pack      %d mV\n", b.pack_mv);
  } else {
    std::printf("pack      uncalibrated — set kDividerPerMille in power.cpp from a\n"
                "          meter on the pack terminals against the pin mV above,\n"
                "          at two charge levels\n");
  }
  return 0;
}

void register_command() {
  // Trailing members left defaulted for the same reason cloud.cpp leaves them:
  // esp_console_cmd_t has grown fields across IDF releases.
  const esp_console_cmd_t cmd = {
      .command = "power",
      .help = "idle tier + battery sense; `power wake` lights the screen",
      .hint = nullptr,
      .func = &cmd_power,
  };
  esp_console_cmd_register(&cmd);
}

// Idempotent. Nothing calls it twice today, but a second adc_oneshot_new_unit() on
// the same unit fails rather than being a no-op, and the symptom of that would be a
// post with no battery sense at all — expensive to diagnose for a guard this cheap.
esp_err_t init_adc() {
  if (s_adc != nullptr) return ESP_OK;

  const adc_oneshot_unit_init_cfg_t unit_cfg = {
      .unit_id = kAdcUnit,
  };
  esp_err_t e = adc_oneshot_new_unit(&unit_cfg, &s_adc);
  if (e != ESP_OK) return e;

  const adc_oneshot_chan_cfg_t chan_cfg = {
      .atten = kAdcAtten,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  e = adc_oneshot_config_channel(s_adc, kAdcChannel, &chan_cfg);
  if (e != ESP_OK) return e;

  // Calibration is optional: without an eFuse curve the raw counts are still the
  // number the divider is measured from, so a board with no calibration data
  // reports raw and nothing else rather than refusing to sense at all.
  const adc_cali_curve_fitting_config_t cali_cfg = {
      .unit_id = kAdcUnit,
      .chan = kAdcChannel,
      .atten = kAdcAtten,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
    s_cali = nullptr;
    ESP_LOGW(kTag, "no ADC calibration curve — battery reports raw counts only");
  }
  return ESP_OK;
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;

  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) return ESP_ERR_NO_MEM;

  const esp_err_t adc_err = init_adc();
  if (adc_err != ESP_OK) {
    // Non-fatal, and deliberately so: the ladder is worth having on a board whose
    // battery sense is broken or absent, and the screen dimming is the half that
    // saves power on any supply.
    ESP_LOGW(kTag, "battery sense unavailable: %s", esp_err_to_name(adc_err));
    s_adc = nullptr;
  }

  if (xTaskCreatePinnedToCore(task, "power", kTaskStack, nullptr, kTaskPrio, nullptr,
                              kTaskCore) != pdPASS) {
    vSemaphoreDelete(s_mutex);
    s_mutex = nullptr;
    return ESP_ERR_NO_MEM;
  }

  register_command();
  s_inited = true;
  ESP_LOGI(kTag, "dim at %" PRIu32 " s, dark at %" PRIu32 " s, deep sleep %s",
           kDimAfterMs / kMsPerSec, kDarkAfterMs / kMsPerSec,
           kDeepSleepReady ? "armed" : "not wired yet");
  return ESP_OK;
}

Tier tier() {
  if (s_mutex == nullptr) return Tier::Active;
  Lock l;
  return s_tier;
}

uint32_t idle_ms() {
  if (s_mutex == nullptr) return 0;
  Lock l;
  return s_idle_ms;
}

Battery battery() {
  if (s_mutex == nullptr) return Battery{};
  Lock l;
  return s_battery;
}

bool deep_sleep_armed() { return kDeepSleepReady; }

void turn_off() {
  ESP_LOGW(kTag, "switching off; a touch on the screen is the way back");
  s_off_magic = kOffMagic;
  // Blanked before sleeping rather than left to the pin falling on its own, so the
  // screen goes out at the moment of the tap instead of a beat later.
  display::set_on(false);

  const gpio_num_t pin = static_cast<gpio_num_t>(board::kTouchInt);
  // The pull-up has to be held through the sleep by the RTC domain, not the normal
  // GPIO matrix, which is powered down. Without it the line floats and the post
  // wakes on nothing at all — which looks exactly like Turn Off not working.
  rtc_gpio_pullup_en(pin);
  rtc_gpio_pulldown_dis(pin);
  esp_sleep_enable_ext0_wakeup(pin, kTouchIntActiveLevel);
  esp_deep_sleep_start();
}

bool resume_from_off() {
  // The reset reason is the authoritative half of the check; the magic guards the
  // case where a deep-sleep wake happens for some other reason later.
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP || s_off_magic != kOffMagic) {
    s_off_magic = 0;
    return false;
  }
  s_off_magic = 0;

  // Hand the pad back before the touch driver comes up: it was held by the RTC
  // domain across the sleep, and a pin still owned by RTC IO is not one the normal
  // GPIO matrix can configure.
  rtc_gpio_deinit(static_cast<gpio_num_t>(board::kTouchInt));
  return true;
}

bool on_external_power() {
  if (s_mutex == nullptr) return true;
  Lock l;
  return s_external;
}

void wake() {
  if (s_mutex == nullptr) return;
  const int64_t now = esp_timer_get_time() / 1000;
  Lock l;
  s_woke_at_ms = now;
}

bool consume_touch() {
  // Before init(), every touch is an ordinary one — which is the safe answer: a
  // post whose ladder is not running yet is at full brightness by definition.
  if (s_mutex == nullptr) return false;
  const int64_t now = esp_timer_get_time() / 1000;
  Lock l;
  // Read the tier before stamping the wake, or the touch would be judged against
  // the state it is about to create.
  const bool was_lit = s_tier == Tier::Active;
  s_woke_at_ms = now;
  return !was_lit;
}

}  // namespace thermopost::power
