#include "power.hpp"

#include <cinttypes>
#include <cstdio>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "alert.hpp"
#include "board_pins.hpp"
#include "display.hpp"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_console.h"
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

// The divider is NOT documented and has not been measured, so nothing here
// converts a pin voltage into a pack voltage yet. Setting this to the measured
// ratio — pack millivolts per thousand pin millivolts — is the whole of the
// calibration, and `calibrated` below follows from it.
//
// Take the reading with a meter on the pack terminals and the `power` command on
// screen, at two charge levels: two points give the ratio AND the offset, one
// gives a guess. Until then pack_mv is zero rather than wrong, because a battery
// gauge that lies is worse than one that admits it does not know.
constexpr int kDividerPerMille = 0;  // 0 = uncalibrated

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

Tier tier_for(uint32_t idle, bool alarm_active) {
  // An alarm outranks the whole ladder, and lights the screen rather than merely
  // holding it: the panel it raises is useless on a dark display, and a post that
  // beeps in the dark makes someone hunt for a device instead of reading it.
  if (alarm_active) return Tier::Active;
  if (idle >= kDarkAfterMs) return kDeepSleepReady ? Tier::Asleep : Tier::Dark;
  if (idle >= kDimAfterMs) return Tier::Dim;
  return Tier::Active;
}

void sample_battery(Battery* out) {
  out->present = false;
  out->calibrated = kDividerPerMille > 0;
  out->raw = 0;
  out->pin_mv = 0;
  out->pack_mv = 0;

  if (s_adc == nullptr) return;
  if (adc_oneshot_read(s_adc, kAdcChannel, &out->raw) != ESP_OK) return;
  out->present = true;

  if (s_cali != nullptr) {
    adc_cali_raw_to_voltage(s_cali, out->raw, &out->pin_mv);
  }
  if (out->calibrated) {
    out->pack_mv = out->pin_mv * kDividerPerMille / 1000;
  }
}

void task(void*) {
  TickType_t last = xTaskGetTickCount();
  Tier applied = Tier::Active;
  apply(applied);

  for (;;) {
    xTaskDelayUntil(&last, pdMS_TO_TICKS(kTickMs));

    uint32_t idle = 0;
    if (!read_idle_ms(&idle)) continue;

    const bool alarm_active = alert::should_alert(alert::status());

    Battery b;
    sample_battery(&b);

    Tier want;
    {
      Lock l;
      const int64_t now = esp_timer_get_time() / 1000;
      const uint32_t since_wake = static_cast<uint32_t>(now - s_woke_at_ms);
      if (since_wake < idle) idle = since_wake;
      s_idle_ms = idle;
      s_battery = b;
      want = tier_for(idle, alarm_active);
      s_tier = want;
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

esp_err_t init_adc() {
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

void wake() {
  if (s_mutex == nullptr) return;
  const int64_t now = esp_timer_get_time() / 1000;
  Lock l;
  s_woke_at_ms = now;
}

}  // namespace thermopost::power
