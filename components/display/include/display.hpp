#pragma once

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"

namespace thermopost::display {

// Brings up the SPI bus, the ILI9341 panel, the LEDC backlight, LVGL, and the
// LVGL task. Returns ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// LVGL display handle. Valid only after a successful init().
lv_display_t* lvgl_display();

// Register the `shot` console command, which prints the active screen as base64
// for tools/screenshot.py to turn into a PNG.
//
// Separate from init() because it has to run *after* shell::init(): the console's
// command list does not exist until the REPL is created, so registering during
// display bring-up would add the command to nothing.
esp_err_t register_console();

// LVGL is not thread-safe. Any task touching LVGL state (creating widgets,
// reading input, setting values) must hold this lock for the duration of that
// work. The internal LVGL task also holds it across lv_timer_handler().
//
// `timeout_ms == UINT32_MAX` waits forever. Returns true on acquire, false on
// timeout (or if init() hasn't run yet).
bool lock(uint32_t timeout_ms = UINT32_MAX);
void unlock();

// Backlight level in percent (0..100), via LEDC PWM on the backlight GPIO.
//
// Unlike an AMOLED, 0% here only kills the LEDs — the panel keeps scanning and
// the image is still "there", just unlit. That is what the idle-dim tier wants;
// for real power saving use set_on(false).
esp_err_t set_brightness(uint8_t pct);

// Last level passed to set_brightness(), ignoring any set_on(false) blanking.
uint8_t brightness();

// Display on/off. Off stops the panel scanning *and* drops the backlight to 0;
// on restores the scan and the previous brightness. The framebuffer survives,
// so the same UI reappears without a repaint.
esp_err_t set_on(bool on);

}  // namespace thermopost::display
