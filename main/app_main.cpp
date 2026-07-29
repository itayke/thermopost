// ============================================================================
//  Thermo Post — boot sequencer
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
//   display → nvs → storage → settings → i2c → sensor → (alert → cloud → power → ui)
//
// Display comes first so a failure downstream still has somewhere to be shown.
// The bracketed steps land with their milestones.

#include "board_i2c.hpp"
#include "bringup_probe.hpp"

#include "display.hpp"
#include "sensor.hpp"
#include "settings.hpp"
#include "storage.hpp"

#include "esp_log.h"
#include "nvs_flash.h"

#include <cstdio>

namespace {

using namespace thermopost;

constexpr const char* kTag = "app";

// Backlight level the UI comes up at, until the power policy owns it.
constexpr uint8_t kBootBrightnessPct = 100;

// --- Readout screen --------------------------------------------------------
// A deliberately plain stand-in for the real UI (M5). Its job is to let the
// sensor be checked against a reference thermometer, so it shows the raw °C
// alongside the value in the user's chosen unit, plus a live sample counter —
// a frozen number is otherwise indistinguishable from a very stable room.
constexpr int      kReadoutValueDy   = -24;  // big number sits above centre
constexpr int      kReadoutStatusDy  = -8;   // status line above the bottom edge
constexpr int      kReadoutBorderPx  = 2;
constexpr uint32_t kReadoutAccent    = 0x00A0FF;
constexpr uint32_t kReadoutMuted     = 0x808080;
constexpr uint32_t kReadoutFault     = 0xFF5040;
// Poll faster than the sensor samples, so the display is never more than a
// fraction of a period behind the value it is showing.
constexpr uint32_t kReadoutRefreshMs = 250;
constexpr const char* kReadoutNoValue = "--.-";
constexpr size_t kReadoutTextMax = 64;

lv_obj_t* s_value_label = nullptr;
lv_obj_t* s_status_label = nullptr;

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

// Runs in the LVGL task, which already holds the LVGL lock.
//
// Formatting goes through the C library's snprintf, NOT lv_label_set_text_fmt.
// LVGL's built-in printf compiles out `case 'f'` unless LV_USE_FLOAT is on, and
// the unsupported conversion then falls through *without* consuming its double
// from the varargs — so every later argument is read from the wrong offset. A
// trailing %s picks up half a mantissa as a char* and the LVGL task dies in
// lv_strnlen. Silent, and it only fires on the format strings that carry a
// float, which is why it survived the first flash of the display milestone.
void readout_tick(lv_timer_t*) {
  const sensor::Reading r = sensor::latest();
  char text[kReadoutTextMax];

  if (r.valid) {
    std::snprintf(text, sizeof(text), "%.1f%s",
                  static_cast<double>(settings::display_value(r.temp_c100)),
                  settings::unit_suffix());
    lv_label_set_text(s_value_label, text);
    lv_obj_set_style_text_color(s_value_label, lv_color_white(), LV_PART_MAIN);

    // The separator is • (U+2022). The built-in Montserrat fonts carry ASCII
    // plus exactly two extras, ° and • — anything else renders as a blank.
    std::snprintf(text, sizeof(text), "%.2f °C  •  MCP9808 @ 0x%02X  •  #%lu",
                  static_cast<double>(r.temp_c100) / 100.0, sensor::address(),
                  static_cast<unsigned long>(r.samples));
    lv_label_set_text(s_status_label, text);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(kReadoutMuted), LV_PART_MAIN);
    return;
  }

  lv_label_set_text(s_value_label, kReadoutNoValue);
  lv_obj_set_style_text_color(s_value_label, lv_color_hex(kReadoutFault), LV_PART_MAIN);
  lv_label_set_text(s_status_label, sensor::address() == 0
                                        ? "no sensor - check the I2C lead"
                                        : "sensor fault - rescanning");
  lv_obj_set_style_text_color(s_status_label, lv_color_hex(kReadoutFault), LV_PART_MAIN);
}

void build_readout_screen() {
  if (!display::lock()) {
    ESP_LOGE(kTag, "could not take the LVGL lock; skipping the readout screen");
    return;
  }

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

  // A frame around the full extent of the screen. Kept from the hello screen
  // because it is still the cheapest orientation check there is: if a rotation
  // flag or a panel offset is wrong, an edge is clipped or shifted, which is
  // obvious — whereas centred text would still look fine.
  lv_obj_set_style_border_color(scr, lv_color_hex(kReadoutAccent), LV_PART_MAIN);
  lv_obj_set_style_border_width(scr, kReadoutBorderPx, LV_PART_MAIN);

  s_value_label = lv_label_create(scr);
  lv_label_set_text(s_value_label, kReadoutNoValue);
  lv_obj_set_style_text_color(s_value_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, kReadoutValueDy);

  s_status_label = lv_label_create(scr);
  lv_label_set_text(s_status_label, "starting...");
  lv_obj_set_style_text_color(s_status_label, lv_color_hex(kReadoutMuted), LV_PART_MAIN);
  lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, kReadoutStatusDy);

  lv_timer_create(readout_tick, kReadoutRefreshMs, nullptr);

  display::unlock();
}

}  // namespace

extern "C" void app_main(void) {
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

  build_readout_screen();
  display::set_brightness(kBootBrightnessPct);

  ESP_LOGI(kTag, "boot complete");
}
