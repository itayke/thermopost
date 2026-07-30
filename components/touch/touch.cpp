#include "touch.hpp"

#include <cinttypes>

#include "sdkconfig.h"

#include "board_i2c.hpp"
#include "board_pins.hpp"
#include "display.hpp"

#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"

namespace thermopost::touch {
namespace {

constexpr const char* kTag = "touch";

// --- Orientation ------------------------------------------------------------
// The display is landscape because its MADCTL swaps X and Y (display.cpp,
// kSwapXy). The touch controller has its own MADCTL equivalent — none — so it
// keeps reporting in the glass's native 240×320 portrait frame and the same
// transform has to be applied here in software.
//
// esp_lcd_touch applies these in a fixed order: mirror first, THEN swap. That
// makes x_max/y_max the PRE-swap extents, i.e. the panel's native ones, not the
// 320×240 the UI works in. Passing the landscape numbers here is the easy
// mistake and it doesn't fail loudly — it mirrors about the wrong axis length,
// so the finger tracks correctly near one edge and drifts across the screen.
//
// Which mirrors are needed cannot be read off the display config: the panel and
// the touch glass have their own origin corners, and this module is a rebrand.
// Settled by dragging: swap_xy alone tracked the horizontal axis correctly but
// put a top-left touch at the bottom left, so the vertical axis needed flipping.
//
// Mind which flag does that. Like the display's, these mirrors are in the
// controller's own frame, which swap_xy then transposes relative to the viewer —
// so it is kMirrorX, acting on the native X the swap turns into the viewer's
// vertical axis, that corrects an up/down inversion. kMirrorY would have moved
// the axis that was already right.
constexpr bool kSwapXy   = true;
constexpr bool kMirrorX  = true;
constexpr bool kMirrorY  = false;

// --- Offset -----------------------------------------------------------------
// A constant nudge applied to every reported point, in screen coordinates, after
// the transform above. Taps land a little below where they are aimed, and two
// causes push the same way: the touch glass is bonded over the LCD with a small
// vertical offset, and a fingertip's contact patch — which is what the controller
// reports — sits below the spot the user believes they are pointing at.
//
// A constant, not a scale factor, because that is the shape of both causes. The
// mapping itself is a clean inclusive 0..239 to 0..239, so there is no gain error
// to correct; if the miss ever grows towards one edge the fault is in the driver's
// x_max/y_max instead, and an offset would only hide it near the middle.
// THERMOPOST_TOUCH_LOG is how to tell those apart — tap near the top and near the
// bottom and compare.
constexpr int32_t kOffsetX = CONFIG_THERMOPOST_TOUCH_OFFSET_X;
constexpr int32_t kOffsetY = CONFIG_THERMOPOST_TOUCH_OFFSET_Y;

// Reset is active low on the FT6x36 family.
constexpr unsigned kResetActiveLevel = 0;

lv_indev_t* s_indev = nullptr;
esp_lcd_touch_handle_t s_touch = nullptr;

// Last reported position, held across the release. LVGL needs the release event
// to carry the same point as the press, or a tap registers as a click at
// wherever the pointer happened to sit before — usually (0,0).
lv_point_t s_last = {0, 0};

int32_t clamp(int32_t v, int32_t hi) {
  if (v < 0) return 0;
  if (v > hi) return hi;
  return v;
}

// Raw controller reading beside the mapped screen point, for dialling the offset in
// by measurement instead of by feel: tap a known target and read the difference.
// Only on the press edge, so a held finger logs once rather than at LVGL's read
// rate. Compiles away entirely — including its edge detector — when the option is
// off, which is why the state lives in here rather than beside s_last.
#if CONFIG_THERMOPOST_TOUCH_LOG
void log_touch(const esp_lcd_touch_point_data_t& raw, lv_point_t mapped, bool pressed) {
  static bool was_pressed = false;
  if (pressed && !was_pressed) {
    ESP_LOGI(kTag, "touch raw (%d,%d) -> screen (%" PRId32 ",%" PRId32 ")",
             static_cast<int>(raw.x), static_cast<int>(raw.y), mapped.x, mapped.y);
  }
  was_pressed = pressed;
}
#else
inline void log_touch(const esp_lcd_touch_point_data_t&, lv_point_t, bool) {}
#endif

// Called by LVGL from its own task, which already holds the LVGL lock.
void read_cb(lv_indev_t*, lv_indev_data_t* data) {
  esp_lcd_touch_point_data_t point = {};
  uint8_t points = 0;

  esp_lcd_touch_read_data(s_touch);
  esp_lcd_touch_get_data(s_touch, &point, &points, 1);

  if (points > 0) {
    // Clamp after the offset, not before: a correction applied near an edge would
    // otherwise push the point outside the panel. Clamping rather than trusting
    // also covers the mirror above, which is `x_max - x` and overshoots by one at
    // the origin edge, and a controller reporting outside its configured range.
    s_last.x = clamp(point.x + kOffsetX, board::kLcdHRes - 1);
    s_last.y = clamp(point.y + kOffsetY, board::kLcdVRes - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  data->point = s_last;
  log_touch(point, s_last, points > 0);

  // When power/ lands, this is where an input that only wakes the screen gets
  // consumed rather than delivered — see the plan's note on power::consume_input().
}

}  // namespace

esp_err_t init() {
  if (s_indev != nullptr) return ESP_ERR_INVALID_STATE;

  i2c_master_bus_handle_t bus = board_i2c::bus();
  if (bus == nullptr) {
    ESP_LOGE(kTag, "I2C bus is not up — call board_i2c::init() first");
    return ESP_ERR_INVALID_STATE;
  }
  if (display::lvgl_display() == nullptr) {
    ESP_LOGE(kTag, "display is not up — call display::init() first");
    return ESP_ERR_INVALID_STATE;
  }

  // Probe before constructing anything, so a missing controller reports itself
  // plainly instead of surfacing as an I/O error from the driver's first write.
  if (i2c_master_probe(bus, board::kTouchI2cAddr, /*timeout_ms=*/20) != ESP_OK) {
    ESP_LOGE(kTag, "nothing answers at 0x%02X — touch is unavailable", board::kTouchI2cAddr);
    return ESP_ERR_NOT_FOUND;
  }

  // Spelled out rather than built from ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG().
  // That macro lists .scl_speed_hz before .control_phase_bytes, and the struct
  // declares them the other way round — which C tolerates and GCC's C++ rejects
  // outright. The values are the macro's, with the bus speed raised from its
  // hardcoded 100 kHz to the board's 400 kHz.
  const esp_lcd_panel_io_i2c_config_t io_cfg = {
      .dev_addr = board::kTouchI2cAddr,
      .control_phase_bytes = 1,
      .dc_bit_offset = 0,
      .lcd_cmd_bits = 8,
      .flags = {.disable_control_phase = 1},
      .scl_speed_hz = board::kI2cFreqHz,
  };

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "panel IO failed: %s", esp_err_to_name(err));
    return err;
  }

  const esp_lcd_touch_config_t touch_cfg = {
      // Width-1, not width. The mirror is computed as `x_max - x` over an
      // inclusive 0..239 range, so passing 240 maps the native origin to 240 —
      // one past the end. Everything would sit a pixel low and the top row of
      // the screen would be unreachable. Most examples pass the full width and
      // get away with it only because they never enable a mirror.
      .x_max = board::kPanelNativeW - 1,
      .y_max = board::kPanelNativeH - 1,
      .rst_gpio_num = static_cast<gpio_num_t>(board::kTouchReset),
      // GPIO17 is deliberately left unclaimed. Polling at LVGL's read rate is
      // plenty for a 2-point controller, and the pin is RTC-capable — keeping it
      // free means power/ can later take it as an ext0 wake source so a tap can
      // bring the board out of deep sleep.
      .int_gpio_num = GPIO_NUM_NC,
      .levels = {.reset = kResetActiveLevel, .interrupt = 0},
      .flags = {.swap_xy = kSwapXy, .mirror_x = kMirrorX, .mirror_y = kMirrorY},
  };

  err = esp_lcd_touch_new_i2c_ft5x06(io, &touch_cfg, &s_touch);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "FT5x06 driver init failed: %s — if the controller answered but "
                   "will not initialize, this is the FT6336G rejecting the FT5x06 "
                   "register writes; switch to cfscn/esp_lcd_touch_ft6x36",
             esp_err_to_name(err));
    esp_lcd_panel_io_del(io);
    return err;
  }

  if (!display::lock()) {
    ESP_LOGE(kTag, "could not take the LVGL lock");
    return ESP_ERR_TIMEOUT;
  }
  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, read_cb);
  lv_indev_set_display(s_indev, display::lvgl_display());
  display::unlock();

  ESP_LOGI(kTag, "FT6336 ready at 0x%02X (swap_xy=%d mirror_x=%d mirror_y=%d, "
                 "offset %+ld%+ld)",
           board::kTouchI2cAddr, kSwapXy, kMirrorX, kMirrorY,
           static_cast<long>(kOffsetX), static_cast<long>(kOffsetY));
  return ESP_OK;
}

lv_indev_t* indev() { return s_indev; }

}  // namespace thermopost::touch
