#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// The board's one I²C bus (I2C0), shared by three devices: the FT6336 touch
// controller, the ES8311 codec's control port, and the MCP9808 on the 4-pin
// "I2C" connector.
//
// It lives in its own component because whoever initializes the bus first would
// otherwise own it, and every other driver would have to know which one that
// was. Here nobody owns it: each driver calls init() (harmless if someone
// already did) and adds its own device handle with its own scl_speed_hz —
// ESP-IDF's i2c_master driver clocks per device, so the sensor can run at
// 100 kHz on its flying lead while touch keeps 400 kHz on the PCB traces.
namespace thermopost::board_i2c {

// Create the master bus on the pins in board_pins.hpp. Returns
// ESP_ERR_INVALID_STATE if the bus is already up, which callers should treat as
// success — see the note above.
esp_err_t init();

// The bus handle, or nullptr before a successful init().
i2c_master_bus_handle_t bus();

}  // namespace thermopost::board_i2c
