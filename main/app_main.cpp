// ============================================================================
//  Thermopost — boot sequencer
//
//  A surface-temperature monitor for the LCDwiki ES3C28P (a.k.a. the Hosyond
//  2.8" ESP32-S3 touchscreen module): samples an MCP9808 on a fixed cadence,
//  graphs the recent history, alerts on screen and through the speaker when the
//  reading leaves a min/max band, and mirrors every reading to a Google Sheet.
//
//  Author:    Itay Keren <itay@untame.com>
//  Copyright: (c) 2026 Itay Keren. All rights reserved.
// ============================================================================
//
// Boot order, each step logging and continuing where it can:
//
//   display → nvs → storage → settings → i2c → sensor → touch → audio → recorder → ui
//           → shell → cloud → alert → power
//
// Display comes first so a failure downstream still has somewhere to be shown.
// `recorder` precedes `ui` because the graph is drawn from the ring that
// recorder::init() backfills — the other way round would paint an empty chart and
// then fill it a frame later. `shell` precedes `cloud` because esp_console's
// command list is created there and a registration before it fails. `alert` comes
// last of the running parts because it needs the sensor, the settings and the
// codec and is needed by none of them; the UI reads it before it starts, which is
// safe — alert::status() answers Normal until the first tick. `power` is last of
// all: its ladder reads the alarm, and it takes over the backlight from the fixed
// level set at the bottom of this function.
//
// A timer wake takes a shorter road first — see quiet_wake() — and joins this one only
// when it finds something a person has to see.

#include "board_i2c.hpp"
#include "bringup_probe.hpp"

#include "alert.hpp"
#include "audio.hpp"
#include "cloud.hpp"
#include "power.hpp"
#include "display.hpp"
#include "recorder.hpp"
#include "sensor.hpp"
#include "settings.hpp"
#include "shell.hpp"
#include "storage.hpp"
#include "touch.hpp"
#include "ui.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {

using namespace thermopost;

constexpr const char* kTag = "app";

// Backlight level the UI comes up at, until the power policy owns it.
constexpr uint8_t kBootBrightnessPct = 100;

esp_err_t init_nvs() {
  const esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // First boot on a fresh partition, or an NVS format bump. Both are expected
    // once; erase and retry rather than treating it as a failure.
    ESP_ERROR_CHECK(nvs_flash_erase());
    return nvs_flash_init();
  }
  return err;
}

// A subsystem the bring-up probe already brought up reports ESP_ERR_INVALID_STATE
// from its second init(). That is "already running", not a fault.
bool ok_or_already_up(esp_err_t err) {
  return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

void step(const char* name, esp_err_t err) {
  if (ok_or_already_up(err)) {
    ESP_LOGI(kTag, "  %-10s ok", name);
  } else {
    ESP_LOGE(kTag, "  %-10s FAILED: %s", name, esp_err_to_name(err));
  }
}

// How long a quiet wake waits for its one reading: two periods covers a sample that
// just missed, and a sensor that has gone is a fault record rather than a hang.
constexpr int64_t  kUsPerMs = 1000;
constexpr int64_t  kFirstReadingWaitUs = 2 * int64_t{sensor::kSamplePeriodMs} * kUsPerMs;
constexpr uint32_t kFirstReadingPollMs = 10;

// Long enough to associate, take a lease and post a batch through Apps Script's
// redirect; a backlog longer than that finishes on a later wake.
constexpr uint32_t kQuietUploadTimeoutMs = 30'000;

sensor::Reading wait_for_reading() {
  const int64_t until = esp_timer_get_time() + kFirstReadingWaitUs;
  sensor::Reading r = sensor::latest();
  while (r.samples == 0 && esp_timer_get_time() < until) {
    vTaskDelay(pdMS_TO_TICKS(kFirstReadingPollMs));
    r = sensor::latest();
  }
  return r;
}

// A timer wake: record, judge the alarm, upload when the dark cadence is due, and sleep
// again with the panel never lit. Returns only when the post has to come up properly —
// an alarm to raise, or a supply it does not sleep on — with whether the record it wrote
// is one the uploader must not hold back.
bool quiet_wake() {
  step("nvs", init_nvs());
  step("storage", storage::init());
  step("settings", settings::init());
  step("i2c", board_i2c::init());
  // Before the sensor, so its first configure arms the window rather than clearing it.
  const alert::Thresholds t = settings::thresholds();
  sensor::set_alert_window(t.min_c100, t.max_c100);
  step("sensor", sensor::init());
  step("power", power::begin_quiet());

  const sensor::Reading r = wait_for_reading();
  const bool urgent = recorder::record_once() != 0;

  if (alert::judge_wake(r.temp_c100, r.valid)) {
    ESP_LOGW(kTag, "coming up: the alarm needs raising");
    return urgent;
  }
  if (!power::belongs_asleep()) {
    ESP_LOGI(kTag, "coming up: on a supply this mode stays awake on");
    return urgent;
  }
  if (cloud::upload_due_while_asleep()) {
    const esp_err_t e = cloud::upload_while_asleep(kQuietUploadTimeoutMs);
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) {
      ESP_LOGI(kTag, "no upload: %s", esp_err_to_name(e));
    }
  }
  power::sleep_until_next_record();
}

}  // namespace

extern "C" void app_main(void) {
  // Before everything, including the probe: a post switched off from the Power page
  // wakes on the touch interrupt, and that pad is still held by the RTC domain
  // until this hands it back — which has to happen before touch::init() tries to
  // claim it as an ordinary GPIO.
  power::resume_from_off();
  if (power::wake_cause() != power::Wake::PowerOn) {
    ESP_LOGI(kTag, "woke: %s", power::wake_name(power::wake_cause()));
  }
  const bool quiet_urgent = power::wake_cause() == power::Wake::Timer && quiet_wake();

#if CONFIG_THERMOPOST_RUN_BRINGUP_PROBE
  bringup::run();
#endif

  ESP_LOGI(kTag, "boot");
  step("display", display::init());
  step("nvs", init_nvs());
  step("storage", storage::init());
  step("settings", settings::init());
  step("i2c", board_i2c::init());
  // Non-fatal by design: a missing sensor still boots to a screen that says so,
  // and the sampler keeps rescanning in case the flying lead is reseated.
  step("sensor", sensor::init());
  step("touch", touch::init());
  // Non-fatal for the same reason the sensor is: an alarm that can only be seen
  // is still an alarm, and a board with no codec should boot to a screen that
  // says so rather than not boot.
  step("audio", audio::init());
  step("recorder", recorder::init());
  step("ui", ui::init());
  // After the screens, so a slow radio can never delay the first frame, and
  // before the backlight, so the boot log still reads top to bottom.
  step("shell", shell::init());
  // Not part of display::init(): esp_console has no command list until the REPL
  // above creates one, so `shot` has to be registered on this side of it.
  display::register_console();
  // Non-fatal for the same reason the sensor is: a post with no network still
  // samples, logs and alarms. It just cannot stamp a wall-clock time or upload —
  // and this board has no RTC, so the clock is genuinely the radio's to bring.
  step("cloud", cloud::init());
  // The record a quiet wake escalated over was written before the uploader existed.
  if (quiet_urgent) cloud::notify_new_record(/*urgent=*/true);
  // Last, and needing everything above it: the band from settings, the readings
  // from sensor, the speaker from audio. Non-fatal like the rest — a post that
  // cannot start its watchdog still shows the temperature, which is the one thing
  // it must never stop doing.
  step("alert", alert::init());
  // After the alarm, because the idle ladder asks it whether the screen is
  // allowed to go dark, and after the shell, because it registers a command.
  step("power", power::init());

  // Last, so the panel lights only once there is a finished screen behind it —
  // otherwise the boot shows a frame of whatever the framebuffer happened to hold.
  // power::init() has already started the ladder, whose first tick sets the same
  // level; this is what lights the screen in the gap before it runs.
  display::set_brightness(kBootBrightnessPct);

  ESP_LOGI(kTag, "boot complete");
}
