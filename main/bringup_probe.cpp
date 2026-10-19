// ============================================================================
//  Thermopost — hardware bring-up probe
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
// The board's pin map (main/board_pins.hpp) was transcribed from lcdwiki.com,
// and rebranded modules do change controllers between production runs. Writing
// drivers against an unverified pin map is how bring-up turns into a two-day
// guessing game, so this probe answers the hardware questions and nothing else:
//
//   1. Is this really an ESP32-S3 N16R8 (16 MB flash + 8 MB PSRAM)?
//   2. Who is on the I²C bus? Expect the touch controller at 0x38 and the
//      ES8311 codec at 0x18 or 0x19.
//   3. Is the MCP9808 alive on that same bus, and does it read a plausible room
//      temperature?
//   4. What does the LCD controller report as its ID?
//   5. Does the backlight GPIO actually control the backlight?
//
// It deliberately depends on nothing outside the IDF — no LVGL, no panel driver,
// no touch driver — so it cannot fail for a reason unrelated to the hardware.
//
// It is NOT part of the normal boot: it is compiled in and run only when
// CONFIG_THERMOPOST_RUN_BRINGUP_PROBE is set, because it is DESTRUCTIVE — it
// erases the temperature log and resets settings to get a known start state.
// Turn it on from `idf.py menuconfig` → Thermopost when re-checking hardware.

#include "bringup_probe.hpp"

#include "alert_logic.hpp"
#include "board_pins.hpp"
#include "settings.hpp"
#include "storage.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <cstdio>

namespace {

constexpr const char* kTag = "probe";

// How long to sleep between "still parked" ticks after a failed gate.
constexpr uint32_t kFailParkMs = 10'000;

using namespace thermopost;

// --- What we expect to find, per the LCDwiki page. Anything that disagrees is
// --- the finding; this table is the hypothesis, not the truth. ---
constexpr size_t kExpectFlashBytes = 16u * 1024 * 1024;
constexpr size_t kExpectPsramBytes =  8u * 1024 * 1024;

// --- MCP9808 registers (datasheet DS20005095B §5.1) ---
constexpr uint8_t kMcpRegConfig  = 0x01;
constexpr uint8_t kMcpRegAmbient = 0x05;
constexpr uint8_t kMcpRegMfrId   = 0x06;  // expect 0x0054
constexpr uint8_t kMcpRegDevId   = 0x07;  // expect 0x04xx (device 0x04, revision xx)
constexpr uint16_t kMcpMfrId     = 0x0054;
constexpr uint8_t  kMcpDevId     = 0x04;

// --- LCD controller ID commands ---
// 0xD3 (RDID4) is the one that distinguishes an ILI9341 — it answers
// 0x00 0x93 0x41. 0x04 (RDDID) is read too because an ST7789 answers *that* one
// with 0x85 0x85 0x52 and ignores 0xD3, so the pair separates the two candidates.
constexpr int kLcdCmdRdId4 = 0xD3;
constexpr int kLcdCmdRdDid = 0x04;
// ILI9341 tops out around 6.6 MHz for reads even though writes run at 40 MHz.
constexpr int kLcdProbeFreqHz = 1'000'000;

// Tally so the closing summary can say GATE PASS / GATE FAIL rather than making
// the reader re-scan a wall of log lines.
int s_checks = 0;
int s_failures = 0;

void check(bool ok, const char* what, const char* detail) {
  ++s_checks;
  if (ok) {
    ESP_LOGI(kTag, "  [ ok ] %-28s %s", what, detail);
  } else {
    ++s_failures;
    ESP_LOGE(kTag, "  [FAIL] %-28s %s", what, detail);
  }
}

// ---------------------------------------------------------------------------
// 1. SoC identity
// ---------------------------------------------------------------------------
void probe_chip() {
  ESP_LOGI(kTag, "--- 1. SoC ---");

  esp_chip_info_t info{};
  esp_chip_info(&info);
  ESP_LOGI(kTag, "  %d core(s), silicon revision v%d.%d",
           info.cores, info.revision / 100, info.revision % 100);

  uint32_t flash_bytes = 0;
  char detail[64];
  if (esp_flash_get_size(nullptr, &flash_bytes) == ESP_OK) {
    std::snprintf(detail, sizeof(detail), "%lu MB", flash_bytes / (1024UL * 1024));
    check(flash_bytes >= kExpectFlashBytes, "flash >= 16 MB", detail);
  } else {
    check(false, "flash >= 16 MB", "size read failed");
  }

  const size_t psram_bytes = esp_psram_get_size();
  std::snprintf(detail, sizeof(detail), "%u MB",
                static_cast<unsigned>(psram_bytes / (1024 * 1024)));
  check(psram_bytes >= kExpectPsramBytes, "PSRAM >= 8 MB",
        psram_bytes ? detail : "none detected (check CONFIG_SPIRAM_MODE_OCT)");
}

// ---------------------------------------------------------------------------
// I²C helpers
// ---------------------------------------------------------------------------

// Full 7-bit scan, skipping the reserved low (0x00..0x07) and high (0x78..0x7F)
// ranges. Listing every address that ACKs — not just probing the ones we expect
// — is what turns "the sensor doesn't work" into "the sensor is at 0x1C, not
// 0x18" or "the bus is silent, so it's a wiring fault".
int scan_bus(i2c_master_bus_handle_t bus, uint8_t* found, int max_found) {
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; ++a) {
    if (i2c_master_probe(bus, a, /*timeout_ms=*/20) == ESP_OK) {
      ESP_LOGI(kTag, "    0x%02X ACKs", a);
      if (n < max_found) found[n] = a;
      ++n;
    }
  }
  if (n == 0) {
    ESP_LOGW(kTag, "    (silent — check 3V3, GND, and that SDA/SCL aren't swapped)");
  }
  return n;
}

bool list_has(const uint8_t* list, int n, uint8_t want) {
  for (int i = 0; i < n; ++i) {
    if (list[i] == want) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// 2 & 3. The I²C bus — touch, codec, and the MCP9808
// ---------------------------------------------------------------------------
// All three share one bus (the board's 4-pin "I2C" connector breaks it out), so
// one bus handle serves the whole section.

// Read a 16-bit big-endian MCP9808 register.
bool mcp_read_u16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t* out) {
  uint8_t rx[2] = {};
  if (i2c_master_transmit_receive(dev, &reg, 1, rx, 2, 100) != ESP_OK) return false;
  *out = static_cast<uint16_t>((rx[0] << 8) | rx[1]);
  return true;
}

// Find the MCP9808 by asking every address in its strap range who it is, rather
// than trusting a configured address.
//
// This matters because the ES8311 sits at 0x18 or 0x19, inside that same range.
// An address-only match would happily "find" the sensor at 0x18 and then read
// codec registers as temperatures. The manufacturer ID makes the two
// distinguishable no matter how the breakout's A0/A1/A2 are strapped, so the
// wiring can change without the firmware caring.
//
// Returns the address, or 0 if no MCP9808 answered. `*out_dev` is left attached
// on success for the caller to use and then remove.
uint8_t find_mcp9808(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t* out_dev) {
  for (uint8_t a = board::kSensorI2cAddrFirst; a <= board::kSensorI2cAddrLast; ++a) {
    if (i2c_master_probe(bus, a, /*timeout_ms=*/20) != ESP_OK) continue;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = a,
        .scl_speed_hz = board::kSensorI2cFreqHz,
    };
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) continue;

    uint16_t mfr = 0;
    if (mcp_read_u16(dev, kMcpRegMfrId, &mfr) && mfr == kMcpMfrId) {
      *out_dev = dev;
      return a;
    }
    ESP_LOGD(kTag, "    0x%02X answered but is not an MCP9808 (mfr 0x%04X)", a, mfr);
    i2c_master_bus_rm_device(dev);
  }
  return 0;
}

void probe_i2c() {
  ESP_LOGI(kTag, "--- 2. I2C0 bus (touch + codec + sensor, SDA=%d SCL=%d) ---",
           board::kI2cSda, board::kI2cScl);

  const i2c_master_bus_config_t cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = static_cast<gpio_num_t>(board::kI2cSda),
      .scl_io_num = static_cast<gpio_num_t>(board::kI2cScl),
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags = {.enable_internal_pullup = true},
  };
  i2c_master_bus_handle_t bus = nullptr;
  const esp_err_t err = i2c_new_master_bus(&cfg, &bus);
  if (err != ESP_OK) {
    check(false, "I2C0 bus init", esp_err_to_name(err));
    return;
  }

  uint8_t found[16];
  const int n = scan_bus(bus, found, 16);

  char detail[80];
  std::snprintf(detail, sizeof(detail), "0x%02X", board::kTouchI2cAddr);
  check(list_has(found, n, board::kTouchI2cAddr), "touch controller present", detail);

  const bool codec_low  = list_has(found, n, board::kEs8311I2cAddrLow);
  const bool codec_high = list_has(found, n, board::kEs8311I2cAddrHigh);
  std::snprintf(detail, sizeof(detail), "%s",
                codec_low  ? "0x18 (CE low)"
                : codec_high ? "0x19 (CE high)"
                             : "not at 0x18 or 0x19");
  check(codec_low || codec_high, "ES8311 codec present", detail);

  ESP_LOGI(kTag, "--- 3. MCP9808 (identified by manufacturer ID, not address) ---");

  i2c_master_dev_handle_t dev = nullptr;
  const uint8_t addr = find_mcp9808(bus, &dev);
  if (addr == 0) {
    // Distinguish "not wired" from "wired, but hiding behind the codec" — the
    // one failure mode the shared bus introduces, and the one whose fix (strap
    // A1 high) is not obvious from a bare "not found".
    const bool range_busy = codec_low || codec_high;
    check(false, "MCP9808 present",
          range_busy ? "no MCP9808 in 0x18..0x1F — if it is wired, strap A1 high "
                       "(the codec owns 0x18/0x19) and POWER CYCLE"
                     : "nothing in 0x18..0x1F — is it wired up yet?");
    i2c_del_master_bus(bus);
    return;
  }

  std::snprintf(detail, sizeof(detail), "0x%02X%s", addr,
                addr == board::kSensorI2cAddrDefault ? " (A1 strapped high, as expected)" : "");
  check(true, "MCP9808 present", detail);

  uint16_t dev_id = 0;
  if (mcp_read_u16(dev, kMcpRegDevId, &dev_id)) {
    std::snprintf(detail, sizeof(detail), "device 0x%02X rev 0x%02X",
                  dev_id >> 8, dev_id & 0xFF);
    check((dev_id >> 8) == kMcpDevId, "MCP9808 device ID", detail);
  } else {
    check(false, "MCP9808 device ID", "read failed");
  }

  // Config 0x0000 = continuous conversion, alerts off — the power-on default,
  // written explicitly so a warm reset from an earlier experiment can't leave
  // the part in shutdown and make it look dead.
  const uint8_t cfg_write[3] = {kMcpRegConfig, 0x00, 0x00};
  i2c_master_transmit(dev, cfg_write, sizeof(cfg_write), 100);
  vTaskDelay(pdMS_TO_TICKS(300));  // t_CONV is 250 ms at the default 0.0625 °C resolution

  uint16_t raw = 0;
  if (mcp_read_u16(dev, kMcpRegAmbient, &raw)) {
    // Bits 15..13 are the alert flags, bit 12 is sign, bits 11..0 are the
    // magnitude in 1/16 °C (datasheet §5.1.3).
    const uint8_t upper = static_cast<uint8_t>(raw >> 8) & 0x1F;
    const uint8_t lower = static_cast<uint8_t>(raw & 0xFF);
    float c = (upper & 0x0F) * 16.0f + lower / 16.0f;
    if (upper & 0x10) c -= 256.0f;  // sign bit set → below 0 °C

    std::snprintf(detail, sizeof(detail), "%.2f C / %.2f F", c, c * 1.8f + 32.0f);
    // A plausible indoor reading is the real check — the IDs prove the part is
    // an MCP9808, this proves it is actually converting.
    check(c > -40.0f && c < 85.0f, "MCP9808 reads a temperature", detail);
  } else {
    check(false, "MCP9808 reads a temperature", "ambient register read failed");
  }

  i2c_master_bus_rm_device(dev);
  i2c_del_master_bus(bus);
}

// ---------------------------------------------------------------------------
// 4. LCD controller ID
// ---------------------------------------------------------------------------
void probe_lcd() {
  ESP_LOGI(kTag, "--- 4. LCD controller (SPI2: SCLK=%d MOSI=%d MISO=%d CS=%d DC=%d) ---",
           board::kLcdSclk, board::kLcdMosi, board::kLcdMiso,
           board::kLcdCs, board::kLcdDc);

  const spi_bus_config_t bus_cfg = {
      .mosi_io_num = board::kLcdMosi,
      .miso_io_num = board::kLcdMiso,
      .sclk_io_num = board::kLcdSclk,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 64,
  };
  const esp_err_t bus_err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
  if (bus_err != ESP_OK) {
    check(false, "SPI2 bus init", esp_err_to_name(bus_err));
    return;
  }

  const esp_lcd_panel_io_spi_config_t io_cfg = {
      .cs_gpio_num = board::kLcdCs,
      .dc_gpio_num = board::kLcdDc,
      .spi_mode = 0,
      .pclk_hz = kLcdProbeFreqHz,
      .trans_queue_depth = 4,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
  };
  esp_lcd_panel_io_handle_t io = nullptr;
  const esp_err_t io_err = esp_lcd_new_panel_io_spi(
      static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &io_cfg, &io);
  if (io_err != ESP_OK) {
    check(false, "LCD panel IO", esp_err_to_name(io_err));
    spi_bus_free(SPI2_HOST);
    return;
  }

  // Read both candidate ID commands and print raw bytes. Interpretation is
  // deliberately advisory: these reads need dummy clocks that vary by
  // controller, so garbage here is inconclusive — it is NOT proof the panel is
  // wrong. The definitive test is the display driver putting a correct image on screen.
  uint8_t id4[4] = {};
  const esp_err_t e4 = esp_lcd_panel_io_rx_param(io, kLcdCmdRdId4, id4, sizeof(id4));
  ESP_LOGI(kTag, "  RDID4 (0xD3) -> %02X %02X %02X %02X  (%s)",
           id4[0], id4[1], id4[2], id4[3], esp_err_to_name(e4));

  uint8_t did[4] = {};
  const esp_err_t ed = esp_lcd_panel_io_rx_param(io, kLcdCmdRdDid, did, sizeof(did));
  ESP_LOGI(kTag, "  RDDID (0x04) -> %02X %02X %02X %02X  (%s)",
           did[0], did[1], did[2], did[3], esp_err_to_name(ed));

  // ILI9341 answers RDID4 with 00 93 41; the 93/41 pair may land at any offset
  // depending on how many dummy bytes the read consumed, so search for it.
  bool looks_ili9341 = false;
  for (size_t i = 0; i + 1 < sizeof(id4); ++i) {
    if (id4[i] == 0x93 && id4[i + 1] == 0x41) looks_ili9341 = true;
  }
  bool looks_st7789 = false;
  for (size_t i = 0; i + 1 < sizeof(did); ++i) {
    if (did[i] == 0x85 && did[i + 1] == 0x85) looks_st7789 = true;
  }

  if (looks_ili9341) {
    ESP_LOGI(kTag, "  => signature matches ILI9341 — proceed with esp_lcd_ili9341");
  } else if (looks_st7789) {
    ESP_LOGW(kTag, "  => signature matches ST7789, NOT ILI9341 — switch the driver "
                   "in main/idf_component.yml before writing the display driver");
  } else {
    ESP_LOGW(kTag, "  => no known signature. Inconclusive, not a failure: ID reads "
                   "need controller-specific dummy clocks. Try ILI9341 in "
                   "the display driver and judge by whether the image is correct.");
  }

  esp_lcd_panel_io_del(io);
  spi_bus_free(SPI2_HOST);
}

// ---------------------------------------------------------------------------
// 5. Backlight control
// ---------------------------------------------------------------------------
// Plain GPIO here rather than LEDC: this only answers "does this pin control the
// backlight at all". Whether it *dims* under PWM is a display-driver question, and
// the answer decides whether the power model keeps its "dim" tier.
void probe_backlight() {
  ESP_LOGI(kTag, "--- 5. backlight (GPIO%d) ---", board::kLcdBacklight);

  const gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << board::kLcdBacklight,
      .mode = GPIO_MODE_OUTPUT,
  };
  if (gpio_config(&cfg) != ESP_OK) {
    check(false, "backlight GPIO config", "gpio_config failed");
    return;
  }

  ESP_LOGI(kTag, "  blinking 3x — WATCH THE SCREEN, it should flash on and off");
  for (int i = 0; i < 3; ++i) {
    gpio_set_level(static_cast<gpio_num_t>(board::kLcdBacklight), 1);
    vTaskDelay(pdMS_TO_TICKS(400));
    gpio_set_level(static_cast<gpio_num_t>(board::kLcdBacklight), 0);
    vTaskDelay(pdMS_TO_TICKS(400));
  }
  gpio_set_level(static_cast<gpio_num_t>(board::kLcdBacklight), 1);  // leave it on
  ESP_LOGI(kTag, "  left ON. If it never changed, GPIO%d is not the backlight.",
           board::kLcdBacklight);
}

// ---------------------------------------------------------------------------
// 6. Storage — LittleFS log, absolute indices, rotation transaction
// ---------------------------------------------------------------------------
// Not a hardware question, but it rides along because it needs no sensor, no
// panel and no network — and because the rotation path is otherwise unreachable
// without writing 100,000 records and waiting a week for the cap to trip.
// storage::force_rotate() exists precisely so this can exercise the real
// transaction against a 10-record log.
//
// DESTRUCTIVE: erases the log to get a known starting state. That is fine on a
// bring-up build and must not survive into the real app_main.
void probe_storage() {
  ESP_LOGI(kTag, "--- 6. storage (LittleFS log + rotation) ---");

  const esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // First boot on a fresh partition, or an NVS format bump. Both are expected
    // once; erase and retry rather than treating it as a failure.
    nvs_flash_erase();
    check(nvs_flash_init() == ESP_OK, "NVS init", "after erase");
  } else {
    check(nvs_err == ESP_OK, "NVS init", esp_err_to_name(nvs_err));
  }

  const esp_err_t st_err = storage::init();
  if (st_err != ESP_OK) {
    check(false, "LittleFS mount", esp_err_to_name(st_err));
    return;
  }
  check(true, "LittleFS mount", "/littlefs");

  char detail[80];
  storage::erase_all();

  // Ten records with temperatures 20.00, 20.50, ... so a record's value
  // identifies which one it is after rotation shuffles the file.
  constexpr int kN = 10;
  for (int i = 0; i < kN; ++i) {
    storage::TempRecord r = {};
    r.temp_c100 = storage::temp_to_c100(20.0f + 0.5f * i);
    // Flat interval — a synthetic record has no spread to summarise, so its mean
    // is also its minimum and maximum.
    r.min_c100 = r.temp_c100;
    r.max_c100 = r.temp_c100;
    r.epoch_s = 1'700'000'000u + i;
    r.uptime_s = static_cast<uint32_t>(i);
    if (storage::append(r) != ESP_OK) {
      check(false, "append 10 records", "append failed");
      return;
    }
  }
  std::snprintf(detail, sizeof(detail), "count=%u first=%u next=%u",
                static_cast<unsigned>(storage::count()),
                static_cast<unsigned>(storage::first_index()),
                static_cast<unsigned>(storage::next_index()));
  check(storage::count() == kN && storage::first_index() == 0 &&
            storage::next_index() == kN,
        "append 10 records", detail);

  // read_recent must return the TAIL in chronological order.
  storage::TempRecord buf[16] = {};
  size_t got = storage::read_recent(buf, 5);
  const bool recent_ok =
      got == 5 && buf[0].temp_c100 == storage::temp_to_c100(22.5f) &&
      buf[4].temp_c100 == storage::temp_to_c100(24.5f);
  std::snprintf(detail, sizeof(detail), "got %u, first=%.2f last=%.2f",
                static_cast<unsigned>(got),
                static_cast<double>(storage::c100_to_temp(buf[0].temp_c100)),
                static_cast<double>(storage::c100_to_temp(buf[4].temp_c100)));
  check(recent_ok, "read_recent(5) = records 5..9", detail);

  // Now the real subject: rotate away the oldest 3. Absolute indices must NOT
  // renumber — index 3 has to still mean the record written third.
  const storage::TempRecord before_rotate = buf[0];  // absolute index 5
  if (storage::force_rotate(3) != ESP_OK) {
    check(false, "force_rotate(3)", "returned an error");
    return;
  }
  std::snprintf(detail, sizeof(detail), "count=%u first=%u next=%u (expect 7/3/10)",
                static_cast<unsigned>(storage::count()),
                static_cast<unsigned>(storage::first_index()),
                static_cast<unsigned>(storage::next_index()));
  check(storage::count() == 7 && storage::first_index() == 3 &&
            storage::next_index() == kN,
        "rotation advances base index", detail);

  // The surviving oldest record must be the one originally written third
  // (20.0 + 0.5*3 = 21.5 °C) — proof the right block was dropped.
  uint32_t first_read = 0;
  got = storage::read_from(3, buf, 16, &first_read);
  std::snprintf(detail, sizeof(detail), "got %u from index %u, oldest=%.2f (expect 21.50)",
                static_cast<unsigned>(got), static_cast<unsigned>(first_read),
                static_cast<double>(storage::c100_to_temp(buf[0].temp_c100)));
  check(got == 7 && first_read == 3 &&
            buf[0].temp_c100 == storage::temp_to_c100(21.5f),
        "read_from(3) after rotation", detail);

  // Index 5 must still resolve to the same record it did before the rotation —
  // this is the invariant cloud sync's high-water mark rests on.
  got = storage::read_from(5, buf, 1, &first_read);
  check(got == 1 && first_read == 5 &&
            buf[0].temp_c100 == before_rotate.temp_c100,
        "absolute index survives rotation", "index 5 still reads 22.50");

  // A read below the rotated-away boundary must clamp AND say so, rather than
  // silently returning the wrong records under the requested index.
  got = storage::read_from(0, buf, 16, &first_read);
  std::snprintf(detail, sizeof(detail), "asked 0, started at %u",
                static_cast<unsigned>(first_read));
  check(got == 7 && first_read == 3, "stale read clamps and reports", detail);

  storage::erase_all();
  check(storage::count() == 0 && storage::next_index() == 0, "erase_all", "log empty, base reset");
}

// ---------------------------------------------------------------------------
// 7. Settings — NVS round-trip and the alarm band it hands the state machine
// ---------------------------------------------------------------------------
// The alarm *logic* is covered exhaustively by tests/host/ (./tests/host/run.sh,
// 50 assertions) since it is IDF-free. What can only be checked on device is the
// NVS path: that a written setting survives, and that a deliberately hostile
// band still comes back as something alert::step() can recover from.
void probe_settings() {
  ESP_LOGI(kTag, "--- 7. settings (NVS round-trip) ---");

  const esp_err_t err = settings::init();
  check(err == ESP_OK, "settings init", esp_err_to_name(err));

  char detail[80];
  settings::reset_to_defaults();
  const settings::Settings d = settings::get();
  std::snprintf(detail, sizeof(detail), "interval=%us band=%d..%d",
                static_cast<unsigned>(d.interval_s), d.min_c100, d.max_c100);
  check(d.interval_s == settings::kIntervalDefaultS &&
            d.min_c100 == settings::kMinDefaultC100 &&
            d.max_c100 == settings::kMaxDefaultC100,
        "defaults applied", detail);

  // Write-through, then read back from the cache.
  settings::set_interval_s(300);
  settings::set_unit(settings::TempUnit::Celsius);
  check(settings::get().interval_s == 300 &&
            settings::get().unit == settings::TempUnit::Celsius,
        "setters take effect", "interval=300s, unit=C");

  // Out-of-range values must clamp rather than propagate.
  settings::set_interval_s(1);
  settings::set_volume_pct(200);
  std::snprintf(detail, sizeof(detail), "interval=%u (min %u), volume=%u",
                static_cast<unsigned>(settings::get().interval_s),
                static_cast<unsigned>(settings::kIntervalMinS),
                static_cast<unsigned>(settings::get().volume_pct));
  check(settings::get().interval_s == settings::kIntervalMinS &&
            settings::get().volume_pct == 100,
        "out-of-range values clamp", detail);

  // An inverted band must be stored the right way round, not just tolerated.
  settings::set_band(3000, 1000);
  std::snprintf(detail, sizeof(detail), "stored as %d..%d",
                settings::get().min_c100, settings::get().max_c100);
  check(settings::get().min_c100 == 1000 && settings::get().max_c100 == 3000,
        "inverted band is swapped", detail);

  // And a hysteresis wider than the band must come back capped, or the alarm
  // could never clear — the one settings bug that would make the device buzz
  // until it is unplugged.
  settings::set_hysteresis_c100(9000);
  const alert::Thresholds t = settings::thresholds();
  std::snprintf(detail, sizeof(detail), "hyst %d for a %d-wide band",
                t.hysteresis_c100, t.max_c100 - t.min_c100);
  check(t.hysteresis_c100 <= (t.max_c100 - t.min_c100) / 2,
        "hysteresis stays escapable", detail);

  settings::reset_to_defaults();
}

}  // namespace

namespace thermopost::bringup {

void run() {
  ESP_LOGI(kTag, "hardware bring-up probe");
  ESP_LOGI(kTag, "board: LCDwiki ES3C28P / Hosyond 2.8\" ESP32-S3");
  ESP_LOGI(kTag, "");

  probe_chip();
  probe_i2c();
  probe_lcd();
  probe_backlight();
  probe_storage();
  probe_settings();

  ESP_LOGI(kTag, "");
  ESP_LOGI(kTag, "============================================================");
  if (s_failures == 0) {
    ESP_LOGI(kTag, " GATE PASS — %d/%d checks. board_pins.hpp matches the hardware.",
             s_checks, s_checks);
  } else {
    ESP_LOGE(kTag, " GATE FAIL — %d of %d checks failed.", s_failures, s_checks);
    ESP_LOGE(kTag, " Fix main/board_pins.hpp (or the wiring) before writing any drivers.");
  }
  ESP_LOGI(kTag, " The LCD controller ID above is advisory — confirm the panel by");
  ESP_LOGI(kTag, " whether the display driver draws a correct image.");
  ESP_LOGI(kTag, "============================================================");

  // Park on failure. The probe is a one-shot report and a failed gate means the
  // pin map is wrong; letting boot continue would bury the finding under driver
  // errors caused by it.
  if (s_failures != 0) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(kFailParkMs));
  }
}

}  // namespace thermopost::bringup
