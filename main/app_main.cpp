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
//   display → storage → settings → (sensor → alert → cloud → power → ui)
//
// Display comes first so a failure downstream still has somewhere to be shown.
// The bracketed steps land with their milestones.

#include "board_pins.hpp"
#include "bringup_probe.hpp"

#include "display.hpp"
#include "settings.hpp"
#include "storage.hpp"

#include "esp_log.h"
#include "nvs_flash.h"

namespace {

using namespace thermopost;

constexpr const char* kTag = "app";

// Backlight level the UI comes up at, until the power policy owns it.
constexpr uint8_t kBootBrightnessPct = 100;

// Placeholder screen until the real UI lands in a later milestone. Proves the
// whole path end to end: panel, orientation, LVGL, backlight.
constexpr int      kHelloGapPx    = 16;  // half the gap between the two labels
constexpr int      kHelloBorderPx = 2;
constexpr uint32_t kHelloAccent   = 0x00A0FF;
constexpr uint32_t kHelloMuted    = 0x808080;

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

void build_hello_screen() {
  if (!display::lock()) {
    ESP_LOGE(kTag, "could not take the LVGL lock; skipping the hello screen");
    return;
  }

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

  // A frame around the full extent of the screen. This is the actual test: if
  // the orientation or an unexpected panel offset is wrong, an edge is clipped
  // or shifted, which is obvious — whereas centered text would still look fine.
  lv_obj_set_style_border_color(scr, lv_color_hex(kHelloAccent), LV_PART_MAIN);
  lv_obj_set_style_border_width(scr, kHelloBorderPx, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "Thermo Post");
  lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -kHelloGapPx);

  lv_obj_t* sub = lv_label_create(scr);
  lv_label_set_text_fmt(sub, "%d x %d", board::kLcdHRes, board::kLcdVRes);
  lv_obj_set_style_text_color(sub, lv_color_hex(kHelloMuted), LV_PART_MAIN);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, kHelloGapPx);

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

  build_hello_screen();
  display::set_brightness(kBootBrightnessPct);

  ESP_LOGI(kTag, "boot complete");
}
