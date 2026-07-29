#pragma once

#include "esp_err.h"
#include "lvgl.h"

// FT6336G capacitive touch, registered with LVGL as a pointer input device.
//
// The panel is driven landscape by swapping X and Y in the display's MADCTL,
// which the touch controller knows nothing about — it keeps reporting in the
// glass's native portrait frame. Reconciling the two is this component's real
// job, and it cannot be derived from the display settings alone because the two
// parts have their own origin corners. See the flags in touch.cpp.
namespace thermopost::touch {

// Probe the controller, reset it, and attach an LVGL indev to the display.
// Requires board_i2c::init() and display::init() to have run.
//
// Returns ESP_ERR_NOT_FOUND if the controller does not answer — non-fatal, the
// device still boots and shows readings. ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// The LVGL input device, or nullptr before a successful init(). Needed by any
// code that wants lv_indev_get_point() or wants to reset a long press.
lv_indev_t* indev();

}  // namespace thermopost::touch
