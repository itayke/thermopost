#pragma once

#include "esp_err.h"

// The screens. Builds the main readout and drives it from a repeating LVGL timer;
// the menu and its editors land with M6.
//
// Everything here takes the LVGL lock itself, so callers do not have to know that
// LVGL runs on its own task. Nothing in this component reads the sensor or the
// filesystem directly — the readout comes from `sensor`, the graph from
// `recorder`'s RAM ring, and the numbers around them from `settings`.
namespace thermopost::ui {

// Build the main screen and start its refresh timer. Requires display::init(),
// settings::init() and recorder::init() to have run; touch is optional (the
// screen is still readable without it).
//
// Safe to call once; ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

}  // namespace thermopost::ui
