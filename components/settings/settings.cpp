#include "settings.hpp"

#include <algorithm>

#include "esp_log.h"
#include "nvs.h"

namespace thermopost::settings {
namespace {

constexpr const char* kTag = "settings";
constexpr const char* kNvsNamespace = "settings";

// Short keys — NVS caps them at 15 characters and a dump stays readable.
constexpr const char* kKeyInterval = "iv";
constexpr const char* kKeyMin      = "lo";
constexpr const char* kKeyMax      = "hi";
constexpr const char* kKeyHyst     = "hy";
constexpr const char* kKeyPoints   = "gp";
constexpr const char* kKeyUnit     = "un";
constexpr const char* kKeySound    = "sn";
constexpr const char* kKeyVolume   = "vo";

bool s_inited = false;

Settings s_cur = {
    .interval_s      = kIntervalDefaultS,
    .min_c100        = kMinDefaultC100,
    .max_c100        = kMaxDefaultC100,
    .hysteresis_c100 = kHysteresisDefaultC100,
    .graph_points    = kGraphPointsDefault,
    .unit            = TempUnit::Fahrenheit,
    .sound_enabled   = true,
    .volume_pct      = kVolumeDefaultPct,
};

// --- NVS plumbing ----------------------------------------------------------

template <typename T>
bool persist(const char* key, T value) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err;
  if constexpr (sizeof(T) == 1) {
    err = nvs_set_u8(h, key, static_cast<uint8_t>(value));
  } else {
    err = nvs_set_u16(h, key, static_cast<uint16_t>(value));
  }
  const bool ok = err == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (!ok) ESP_LOGW(kTag, "failed to persist '%s'", key);
  return ok;
}

// int16 temperatures ride in NVS as u16 — the bit pattern round-trips exactly,
// and nvs_set_i16 would work equally well; this just keeps one code path.
bool persist_i16(const char* key, int16_t v) {
  return persist<uint16_t>(key, static_cast<uint16_t>(v));
}

// --- clamping --------------------------------------------------------------
// Applied on both load and set, so a value written by an older firmware (or a
// hand-edited NVS partition) can never reach the sample task or the UI.

uint16_t clamp_interval(uint16_t v) { return std::clamp(v, kIntervalMinS, kIntervalMaxS); }
uint16_t clamp_points(uint16_t v)   { return std::clamp(v, kGraphPointsMin, kGraphPointsMax); }
int16_t  clamp_temp(int16_t v)      { return std::clamp(v, kTempFloorC100, kTempCeilC100); }
uint8_t  clamp_volume(uint8_t v)    { return v > 100 ? 100 : v; }

// Hysteresis is bounded here only against nonsense; alert::sanitize() applies
// the real "must be able to recover" cap against the live band, because that
// depends on min/max and this may run before both have been loaded.
int16_t clamp_hysteresis(int16_t v) {
  if (v < 0) return 0;
  return std::min<int16_t>(v, kTempCeilC100);
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;

  nvs_handle_t h;
  const esp_err_t open_err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
  if (open_err == ESP_ERR_NVS_NOT_FOUND) {
    // Namespace has never been written — first boot. Defaults stand; don't
    // write them, so "never configured" stays distinguishable from "configured
    // to the same values as the defaults".
    s_inited = true;
    ESP_LOGI(kTag, "no stored settings; using defaults");
    return ESP_OK;
  }
  if (open_err != ESP_OK) {
    s_inited = true;  // run on defaults rather than failing the boot
    ESP_LOGW(kTag, "nvs_open failed (%s); using defaults, changes won't persist",
             esp_err_to_name(open_err));
    return open_err;
  }

  uint16_t u16 = 0;
  uint8_t  u8  = 0;
  if (nvs_get_u16(h, kKeyInterval, &u16) == ESP_OK) s_cur.interval_s = clamp_interval(u16);
  if (nvs_get_u16(h, kKeyMin, &u16) == ESP_OK) s_cur.min_c100 = clamp_temp(static_cast<int16_t>(u16));
  if (nvs_get_u16(h, kKeyMax, &u16) == ESP_OK) s_cur.max_c100 = clamp_temp(static_cast<int16_t>(u16));
  if (nvs_get_u16(h, kKeyHyst, &u16) == ESP_OK) s_cur.hysteresis_c100 = clamp_hysteresis(static_cast<int16_t>(u16));
  if (nvs_get_u16(h, kKeyPoints, &u16) == ESP_OK) s_cur.graph_points = clamp_points(u16);
  if (nvs_get_u8(h, kKeyUnit, &u8) == ESP_OK && u8 <= 1) s_cur.unit = static_cast<TempUnit>(u8);
  if (nvs_get_u8(h, kKeySound, &u8) == ESP_OK) s_cur.sound_enabled = (u8 != 0);
  if (nvs_get_u8(h, kKeyVolume, &u8) == ESP_OK) s_cur.volume_pct = clamp_volume(u8);
  nvs_close(h);

  s_inited = true;
  ESP_LOGI(kTag, "interval %us, band %.2f..%.2f C (hyst %.2f), %u graph points, "
                 "%s, sound %s @ %u%%",
           static_cast<unsigned>(s_cur.interval_s),
           static_cast<double>(c100_to_c(s_cur.min_c100)),
           static_cast<double>(c100_to_c(s_cur.max_c100)),
           static_cast<double>(c100_to_c(s_cur.hysteresis_c100)),
           static_cast<unsigned>(s_cur.graph_points),
           s_cur.unit == TempUnit::Celsius ? "C" : "F",
           s_cur.sound_enabled ? "on" : "off",
           static_cast<unsigned>(s_cur.volume_pct));
  return ESP_OK;
}

Settings get() { return s_cur; }

esp_err_t set_interval_s(uint16_t v) {
  v = clamp_interval(v);
  if (v == s_cur.interval_s) return ESP_OK;
  s_cur.interval_s = v;
  return persist<uint16_t>(kKeyInterval, v) ? ESP_OK : ESP_FAIL;
}

esp_err_t set_band(int16_t min_c100, int16_t max_c100) {
  min_c100 = clamp_temp(min_c100);
  max_c100 = clamp_temp(max_c100);
  // Take min/max as a pair rather than two setters: editing them independently
  // lets the UI pass through a transient inverted band, and every consumer would
  // then need to defend against it. alert::sanitize() would recover, but storing
  // a band the user can see is inverted is worse than fixing it at the door.
  if (min_c100 > max_c100) std::swap(min_c100, max_c100);
  if (min_c100 == s_cur.min_c100 && max_c100 == s_cur.max_c100) return ESP_OK;
  s_cur.min_c100 = min_c100;
  s_cur.max_c100 = max_c100;
  const bool ok = persist_i16(kKeyMin, min_c100) && persist_i16(kKeyMax, max_c100);
  return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t set_hysteresis_c100(int16_t v) {
  v = clamp_hysteresis(v);
  if (v == s_cur.hysteresis_c100) return ESP_OK;
  s_cur.hysteresis_c100 = v;
  return persist_i16(kKeyHyst, v) ? ESP_OK : ESP_FAIL;
}

esp_err_t set_graph_points(uint16_t v) {
  v = clamp_points(v);
  if (v == s_cur.graph_points) return ESP_OK;
  s_cur.graph_points = v;
  return persist<uint16_t>(kKeyPoints, v) ? ESP_OK : ESP_FAIL;
}

esp_err_t set_unit(TempUnit u) {
  if (u == s_cur.unit) return ESP_OK;
  s_cur.unit = u;
  return persist<uint8_t>(kKeyUnit, static_cast<uint8_t>(u)) ? ESP_OK : ESP_FAIL;
}

esp_err_t set_sound_enabled(bool on) {
  if (on == s_cur.sound_enabled) return ESP_OK;
  s_cur.sound_enabled = on;
  return persist<uint8_t>(kKeySound, on ? 1 : 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t set_volume_pct(uint8_t pct) {
  pct = clamp_volume(pct);
  if (pct == s_cur.volume_pct) return ESP_OK;
  s_cur.volume_pct = pct;
  return persist<uint8_t>(kKeyVolume, pct) ? ESP_OK : ESP_FAIL;
}

alert::Thresholds thresholds() {
  return alert::sanitize({s_cur.min_c100, s_cur.max_c100, s_cur.hysteresis_c100});
}

esp_err_t reset_to_defaults() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
  }
  s_cur = Settings{
      .interval_s      = kIntervalDefaultS,
      .min_c100        = kMinDefaultC100,
      .max_c100        = kMaxDefaultC100,
      .hysteresis_c100 = kHysteresisDefaultC100,
      .graph_points    = kGraphPointsDefault,
      .unit            = TempUnit::Fahrenheit,
      .sound_enabled   = true,
      .volume_pct      = kVolumeDefaultPct,
  };
  ESP_LOGW(kTag, "settings reset to defaults");
  return ESP_OK;
}

float display_value(int16_t c100) {
  return s_cur.unit == TempUnit::Celsius ? c100_to_c(c100) : c100_to_f(c100);
}

const char* unit_suffix() {
  return s_cur.unit == TempUnit::Celsius ? "°C" : "°F";
}

}  // namespace thermopost::settings
