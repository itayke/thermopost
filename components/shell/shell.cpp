#include "shell.hpp"

#include "esp_console.h"
#include "esp_log.h"

namespace thermopost::shell {
namespace {

constexpr const char* kTag = "shell";

// Distinct from espressopost's "esp>" on purpose: the two boards share a bench
// and a USB hub, and the prompt is the fastest way to tell which one a terminal
// is attached to.
constexpr const char* kPrompt = "post>";

bool s_ready = false;

}  // namespace

esp_err_t init() {
  if (s_ready) return ESP_ERR_INVALID_STATE;

  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_cfg.prompt = kPrompt;

  // The board's only USB port is the ESP32-S3's native USB-Serial-JTAG (no UART
  // bridge — see sdkconfig.defaults), so the REPL has to read from there. A UART0
  // REPL would print its prompt to the mirrored console and then never see a
  // keystroke, which looks like a hung shell rather than a misconfigured one.
  esp_console_dev_usb_serial_jtag_config_t hw_cfg =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

  esp_err_t e = esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl);
  if (e != ESP_OK) {
    ESP_LOGW(kTag, "repl init failed: %s; serial commands unavailable", esp_err_to_name(e));
    return e;
  }
  e = esp_console_start_repl(repl);
  if (e != ESP_OK) {
    ESP_LOGW(kTag, "repl start failed: %s", esp_err_to_name(e));
    return e;
  }

  s_ready = true;
  return ESP_OK;
}

bool ready() { return s_ready; }

}  // namespace thermopost::shell
