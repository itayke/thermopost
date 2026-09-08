#include "sensor.hpp"

#include "board_i2c.hpp"
#include "board_pins.hpp"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace thermopost::sensor {
namespace {

constexpr const char* kTag = "sensor";

// --- MCP9808 registers (datasheet DS20005095B §5.1) ---
constexpr uint8_t kRegConfig     = 0x01;
constexpr uint8_t kRegTUpper     = 0x02;
constexpr uint8_t kRegTLower     = 0x03;
constexpr uint8_t kRegTCrit      = 0x04;
constexpr uint8_t kRegAmbient    = 0x05;
constexpr uint8_t kRegMfrId      = 0x06;  // expect 0x0054
constexpr uint8_t kRegDevId      = 0x07;  // expect 0x04xx (device 0x04, revision xx)
constexpr uint8_t kRegResolution = 0x08;

constexpr uint16_t kMfrId = 0x0054;
constexpr uint8_t  kDevId = 0x04;

// Continuous conversion, every alert feature off. This is the power-on default,
// written explicitly so a warm reset from an earlier experiment can't leave the
// part in shutdown and make it look dead.
constexpr uint16_t kConfigRun = 0x0000;

// --- The hardware alert window (§5.1.1, §5.1.2) -----------------------------
// The point of this is deep sleep. The part compares every conversion against
// T_UPPER/T_LOWER in its own silicon, at about 200 uA, whether or not the SoC is
// running — so a post asleep between records is not blind between them. Wired to
// board::kSensorAlert it wakes the SoC within one conversion, which is faster than
// this firmware notices an excursion while fully awake.
//
// Bit 3 enables the ALERT output. Everything else stays zero deliberately:
// comparator mode (so the pin follows the temperature rather than latching until
// it is read), active low, alerting on the window as well as T_CRIT, and zero
// hardware hysteresis. The last one matters — alert_logic owns hysteresis, and a
// hardware band that recovered later than the software one would hold the pin
// asserted after the alarm had already cleared.
constexpr uint16_t kConfigAlertEnable = 0x0008;

// T_CRIT must be pushed out of the way, and this is the trap in the part. Its
// power-on value is 0 C and CRIT asserts on T_A >= T_CRIT, so a freshly reset
// MCP9808 has its alert asserted at any room temperature. The MCP9808 measures to
// +125 C, so a critical limit above that can never be reached.
constexpr int16_t kCritNeverC100 = 25500;

// Limit registers hold the temperature in bits 12:2 at 0.25 C per LSB — a quarter
// of the ambient register's resolution — with bit 12 the sign, in the same 13-bit
// two's complement the ambient read decodes. Bits 1:0 and 15:13 read as zero.
constexpr uint16_t kLimitFieldMask = 0x1FFF;
constexpr int32_t  kC100PerQuarter = 25;
constexpr int32_t  kTicksPerQuarter = 4;   // 0.25 C in units of 1/16 C
// What the 13-bit field can express, in quarter-degrees: bits 12:0 hold a two's
// complement count of 1/16 C, so -4096..4095 ticks, and a quarter is four of them.
constexpr int32_t  kQuarterMin = -1024;   // -256.00 C
constexpr int32_t  kQuarterMax = 1023;    // +255.75 C

// Ambient register bits 15..13: T_A >= T_CRIT, T_A > T_UPPER, T_A < T_LOWER. They
// track the comparison whether or not the ALERT output is enabled, so the window can
// be proved over I2C alone — before the line is soldered.
constexpr uint16_t kAmbientAlertMask = 0xE000;

// Resolution 0x03 = 0.0625 °C per LSB with a 250 ms conversion — the finest and
// slowest of the four, and the power-on default. At a one-second cadence there
// is nothing to buy with the time a coarser setting would give back.
constexpr uint8_t  kResolutionFinest = 0x03;
constexpr uint32_t kConversionMs = 250;

// --- Ambient register format (§5.1.3) ---
// Bits 15..13 are the alert flags, bit 12 is the sign, bits 11..0 are the
// magnitude in 1/16 °C.
constexpr uint16_t kAmbientMagMask = 0x0FFF;
constexpr uint16_t kAmbientSignBit = 0x1000;
constexpr int32_t  kAmbientSpan    = 0x1000;  // two's-complement wrap of the 13-bit field
// One LSB is 1/16 °C = 6.25 hundredths. As a fraction it stays exact in integers.
constexpr int32_t kC100PerTickNum = 25;
constexpr int32_t kC100PerTickDen = 4;

constexpr int kI2cTimeoutMs = 100;

// kSamplePeriodMs lives in sensor.hpp, not here: `recorder` derives its own tick
// from it, and a private copy is exactly how the two would drift apart.

// Consecutive failed reads before the reading is declared invalid. One miss is
// a bumped lead or a NACK; three in a row at a second apart is a real fault,
// and holding the last value across a single blip keeps the display from
// flickering to "--" for no reason.
constexpr uint16_t kFaultAfterMisses = 3;

// Once faulted, drop the device handle and go back to scanning. The straps are
// latched at power-up so the address cannot have moved, but re-probing is what
// makes a reseated connector recover on its own.
constexpr uint32_t kReprobePeriodMs = 5000;

constexpr int kTaskStack = 3072;
// Above the LVGL task (priority 2): a sample is a handful of short I²C
// transactions, and letting a repaint delay it would skew the cadence.
constexpr int kTaskPrio = 3;
// Core 0, leaving core 1 to LVGL — the flush path is the one thing on this
// board with a frame deadline.
constexpr int kTaskCore = 0;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
uint8_t s_addr = 0;
bool s_started = false;

// The published reading, and the spinlock guarding it. A mutex would work, but
// the critical section is a struct copy — no allocation, no failure path, and
// the LVGL task can never be made to wait on an I²C transaction, because the
// sampler only takes the lock after the bus work is done.
Reading s_latest = {kInvalidTempC100, false, 0, 0, 0, false};
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// The window the caller wants, and whether the part has been told about it yet.
// Guarded by s_lock, applied by the sampling task rather than by the setter: the
// task owns s_dev, and the registers have to be rewritten after every re-probe
// anyway, because a reseated sensor comes back at its power-on defaults.
int16_t s_win_min_c100 = 0;
int16_t s_win_max_c100 = 0;
bool    s_win_wanted = false;
bool    s_win_dirty  = false;

void publish(const Reading& r) {
  taskENTER_CRITICAL(&s_lock);
  s_latest = r;
  taskEXIT_CRITICAL(&s_lock);
}

// --- Register access ---

bool read_u16(uint8_t reg, uint16_t* out) {
  uint8_t rx[2] = {};
  if (i2c_master_transmit_receive(s_dev, &reg, 1, rx, sizeof(rx), kI2cTimeoutMs) != ESP_OK) {
    return false;
  }
  *out = static_cast<uint16_t>((rx[0] << 8) | rx[1]);
  return true;
}

bool write_u16(uint8_t reg, uint16_t val) {
  const uint8_t tx[3] = {reg, static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val)};
  return i2c_master_transmit(s_dev, tx, sizeof(tx), kI2cTimeoutMs) == ESP_OK;
}

bool write_u8(uint8_t reg, uint8_t val) {
  const uint8_t tx[2] = {reg, val};
  return i2c_master_transmit(s_dev, tx, sizeof(tx), kI2cTimeoutMs) == ESP_OK;
}

int16_t raw_to_c100(uint16_t raw) {
  int32_t ticks = raw & kAmbientMagMask;
  if (raw & kAmbientSignBit) ticks -= kAmbientSpan;  // sign bit set → below 0 °C
  const int32_t rounding = (ticks < 0 ? -kC100PerTickDen : kC100PerTickDen) / 2;
  return static_cast<int16_t>((ticks * kC100PerTickNum + rounding) / kC100PerTickDen);
}

// Quarter-degrees, rounded so the hardware window can only ever sit INSIDE the
// software band: T_UPPER down, T_LOWER up. A window wider than the band could let a
// reading raise an alarm on screen that the part never flagged — which is exactly the
// miss the ALERT line exists to prevent. Narrower only ever wakes the post early, and
// a false wake costs a few hundred milliseconds.
uint16_t limit_from_c100(int16_t c100, bool round_down) {
  const int32_t v = c100;
  int32_t q;
  if (round_down) {
    q = (v >= 0) ? v / kC100PerQuarter
                 : -((-v + kC100PerQuarter - 1) / kC100PerQuarter);
  } else {
    q = (v >= 0) ? (v + kC100PerQuarter - 1) / kC100PerQuarter
                 : -((-v) / kC100PerQuarter);
  }
  if (q < kQuarterMin) q = kQuarterMin;
  if (q > kQuarterMax) q = kQuarterMax;
  return static_cast<uint16_t>((q * kTicksPerQuarter) & kLimitFieldMask);
}

// Push the window to the part. Called from the sampling task only.
bool apply_alert_window() {
  int16_t lo, hi;
  bool wanted;
  taskENTER_CRITICAL(&s_lock);
  lo = s_win_min_c100;
  hi = s_win_max_c100;
  wanted = s_win_wanted;
  taskEXIT_CRITICAL(&s_lock);

  if (!wanted) return write_u16(kRegConfig, kConfigRun);

  // T_CRIT first: it is the one that is asserted by default, and writing the limits
  // before the config means the ALERT pin is never briefly enabled against a stale
  // window that would drive it low for no reason.
  if (!write_u16(kRegTCrit, limit_from_c100(kCritNeverC100, /*round_down=*/true))) return false;
  if (!write_u16(kRegTUpper, limit_from_c100(hi, /*round_down=*/true))) return false;
  if (!write_u16(kRegTLower, limit_from_c100(lo, /*round_down=*/false))) return false;
  return write_u16(kRegConfig, kConfigAlertEnable);
}

// --- Discovery ---

void detach() {
  if (s_dev != nullptr) {
    i2c_master_bus_rm_device(s_dev);
    s_dev = nullptr;
  }
  s_addr = 0;
}

// Ask every address in the strap range who it is, rather than trusting a
// configured address.
//
// This is not paranoia: the ES8311 codec sits at 0x18 or 0x19, inside that same
// range. An address-only match would happily "find" the sensor at 0x18 and then
// read codec registers as temperatures. The manufacturer ID separates them no
// matter how the breakout's A0/A1/A2 are strapped, so the wiring can change
// without the firmware caring.
bool find_device() {
  for (uint8_t a = board::kSensorI2cAddrFirst; a <= board::kSensorI2cAddrLast; ++a) {
    if (i2c_master_probe(s_bus, a, /*timeout_ms=*/20) != ESP_OK) continue;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = a,
        .scl_speed_hz = board::kSensorI2cFreqHz,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) continue;

    uint16_t mfr = 0;
    if (read_u16(kRegMfrId, &mfr) && mfr == kMfrId) {
      s_addr = a;
      return true;
    }
    ESP_LOGD(kTag, "0x%02X answered but is not an MCP9808 (mfr 0x%04X)", a, mfr);
    detach();
  }
  return false;
}

// Put a freshly found part into the state we sample it in.
bool configure() {
  if (!write_u16(kRegConfig, kConfigRun)) return false;
  if (!write_u8(kRegResolution, kResolutionFinest)) return false;
  // A part that has just been found is at its power-on defaults, so a window set
  // before it was plugged in has to be written again here rather than waiting for
  // someone to set it a second time.
  if (!apply_alert_window()) return false;
  taskENTER_CRITICAL(&s_lock);
  s_win_dirty = false;
  taskEXIT_CRITICAL(&s_lock);
  return true;
}

bool attach() {
  if (!find_device()) return false;

  uint16_t dev_id = 0;
  if (read_u16(kRegDevId, &dev_id)) {
    ESP_LOGI(kTag, "MCP9808 at 0x%02X (device 0x%02X rev 0x%02X)",
             s_addr, dev_id >> 8, dev_id & 0xFF);
    if ((dev_id >> 8) != kDevId) {
      ESP_LOGW(kTag, "unexpected device ID — the manufacturer ID matched, so this is "
                     "an MCP98xx variant; readings should still be valid");
    }
  }

  if (!configure()) {
    ESP_LOGW(kTag, "found the sensor at 0x%02X but could not configure it", s_addr);
    detach();
    return false;
  }
  return true;
}

void log_not_found() {
  // Distinguish "not wired" from "wired, but hiding behind the codec" — the one
  // failure mode the shared bus introduces, and the one whose fix is not
  // obvious from a bare "not found".
  const bool codec_here =
      i2c_master_probe(s_bus, board::kEs8311I2cAddrLow, 20) == ESP_OK ||
      i2c_master_probe(s_bus, board::kEs8311I2cAddrHigh, 20) == ESP_OK;
  if (codec_here) {
    ESP_LOGE(kTag, "no MCP9808 in 0x%02X..0x%02X. If it is wired, strap A1 high — the "
                   "codec owns 0x18/0x19 — and POWER CYCLE (straps latch at power-up).",
             board::kSensorI2cAddrFirst, board::kSensorI2cAddrLast);
  } else {
    ESP_LOGE(kTag, "nothing answers in 0x%02X..0x%02X — check the 4-pin I2C lead "
                   "(3.3V / GND / IO%d SCL / IO%d SDA).",
             board::kSensorI2cAddrFirst, board::kSensorI2cAddrLast,
             board::kI2cScl, board::kI2cSda);
  }
}

// --- Sampling ---

void sample_task(void*) {
  TickType_t next_wake = xTaskGetTickCount();
  uint32_t since_reprobe_ms = 0;

  for (;;) {
    // DelayUntil, not Delay: the cadence is anchored to a wall-clock-ish tick
    // count, so a slow read doesn't push every later sample out with it.
    xTaskDelayUntil(&next_wake, pdMS_TO_TICKS(kSamplePeriodMs));

    if (s_dev == nullptr) {
      since_reprobe_ms += kSamplePeriodMs;
      if (since_reprobe_ms < kReprobePeriodMs) continue;
      since_reprobe_ms = 0;
      if (!attach()) continue;
      ESP_LOGI(kTag, "sensor is back");
    }

    bool dirty;
    taskENTER_CRITICAL(&s_lock);
    dirty = s_win_dirty;
    taskEXIT_CRITICAL(&s_lock);
    if (dirty) {
      const bool ok = apply_alert_window();
      taskENTER_CRITICAL(&s_lock);
      s_win_dirty = !ok;
      taskEXIT_CRITICAL(&s_lock);
      if (!ok) ESP_LOGW(kTag, "could not write the alert window; will retry");
    }

    Reading r = latest();
    uint16_t raw = 0;
    if (read_u16(kRegAmbient, &raw)) {
      const bool was_alert = r.alert;
      r.temp_c100 = raw_to_c100(raw);
      r.valid = true;
      r.uptime_us = esp_timer_get_time();
      r.misses = 0;
      ++r.samples;
      // Only meaningful once a window has been programmed: until then the limits are
      // at their power-on zeroes and every one of these bits reads set at room
      // temperature.
      bool wanted;
      taskENTER_CRITICAL(&s_lock);
      wanted = s_win_wanted;
      taskEXIT_CRITICAL(&s_lock);
      r.alert = wanted && (raw & kAmbientAlertMask) != 0;
      // Logged on the edge, which is what makes the window testable over I2C alone:
      // warm the sensor past the band and this says so, wire or no wire.
      if (r.alert != was_alert) {
        ESP_LOGI(kTag, "hardware alert %s at %.2f C", r.alert ? "asserted" : "cleared",
                 static_cast<double>(r.temp_c100) / 100.0);
      }
      // Deliberately silent on a good read. This logged every sample during
      // bring-up, which was the point then; now the screen shows the value live
      // and `recorder` logs one line per stored record, so a line a second only
      // buries anything else on the console. Failures below still speak up.
    } else if (++r.misses >= kFaultAfterMisses) {
      // Give up on this handle and go back to scanning — a reseated connector
      // then recovers without a reboot.
      ESP_LOGW(kTag, "%u reads failed in a row; rescanning the bus", r.misses);
      r.valid = false;
      r.temp_c100 = kInvalidTempC100;
      // A part that is gone is not a part that is quiet about the band: an alert
      // left true here would read as "the temperature is fine" to anything checking
      // why the post woke.
      r.alert = false;
      detach();
      since_reprobe_ms = 0;
    }
    publish(r);
  }
}

}  // namespace

esp_err_t init() {
  if (s_started) return ESP_ERR_INVALID_STATE;

  s_bus = board_i2c::bus();
  if (s_bus == nullptr) {
    ESP_LOGE(kTag, "I2C bus is not up — call board_i2c::init() first");
    return ESP_ERR_INVALID_STATE;
  }

  const bool found = attach();
  if (!found) log_not_found();

  if (xTaskCreatePinnedToCore(sample_task, "sensor", kTaskStack, nullptr,
                              kTaskPrio, nullptr, kTaskCore) != pdPASS) {
    detach();
    return ESP_ERR_NO_MEM;
  }
  s_started = true;

  if (!found) return ESP_ERR_NOT_FOUND;

  // The first conversion is only valid one t_CONV after the part starts
  // converting, so wait it out here rather than letting the task publish a
  // stale register on its first pass.
  vTaskDelay(pdMS_TO_TICKS(kConversionMs));
  return ESP_OK;
}

Reading latest() {
  taskENTER_CRITICAL(&s_lock);
  const Reading r = s_latest;
  taskEXIT_CRITICAL(&s_lock);
  return r;
}

uint8_t address() { return s_addr; }

void set_alert_window(int16_t min_c100, int16_t max_c100) {
  taskENTER_CRITICAL(&s_lock);
  const bool changed =
      !s_win_wanted || min_c100 != s_win_min_c100 || max_c100 != s_win_max_c100;
  s_win_min_c100 = min_c100;
  s_win_max_c100 = max_c100;
  s_win_wanted = true;
  if (changed) s_win_dirty = true;
  taskEXIT_CRITICAL(&s_lock);
}

}  // namespace thermopost::sensor
