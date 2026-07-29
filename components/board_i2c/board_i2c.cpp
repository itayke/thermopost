#include "board_i2c.hpp"

#include "board_pins.hpp"
#include "esp_log.h"

namespace thermopost::board_i2c {
namespace {

constexpr const char* kTag = "i2c";

// The ESP32-S3's internal pull-ups are weak (~45 kΩ), which is marginal for
// 400 kHz. They are enabled only as a safety net: the module fits its own
// pull-ups on the touch/codec traces, and MCP9808 breakouts fit theirs. If the
// bus ever misbehaves at speed, this is the first thing to suspect.
constexpr bool kInternalPullup = true;

// Glitch filter length in bus-clock cycles; 7 is the IDF default and filters
// the sub-µs spikes a flying lead picks up.
constexpr int kGlitchIgnoreCnt = 7;

i2c_master_bus_handle_t s_bus = nullptr;

}  // namespace

esp_err_t init() {
  if (s_bus != nullptr) return ESP_ERR_INVALID_STATE;

  const i2c_master_bus_config_t cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = static_cast<gpio_num_t>(board::kI2cSda),
      .scl_io_num = static_cast<gpio_num_t>(board::kI2cScl),
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = kGlitchIgnoreCnt,
      .flags = {.enable_internal_pullup = kInternalPullup},
  };

  const esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "bus init failed: %s", esp_err_to_name(err));
    s_bus = nullptr;
    return err;
  }

  ESP_LOGI(kTag, "I2C0 up (SDA=%d SCL=%d)", board::kI2cSda, board::kI2cScl);
  return ESP_OK;
}

i2c_master_bus_handle_t bus() { return s_bus; }

}  // namespace thermopost::board_i2c
