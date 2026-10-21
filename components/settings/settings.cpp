#include "settings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

namespace thermopost::settings {
namespace {

constexpr const char* kTag = "settings";
constexpr const char* kNvsNamespace = "settings";

// Short keys — NVS caps them at 15 characters and a dump stays readable.
constexpr const char* kKeyInterval = "iv";
constexpr const char* kKeyMin      = "lo";
constexpr const char* kKeyMax      = "hi";
constexpr const char* kKeyMinSheet = "ls";
constexpr const char* kKeyMaxSheet = "hs";
constexpr const char* kKeyHyst     = "hy";
constexpr const char* kKeyPoints   = "gp";
constexpr const char* kKeyUpload   = "ue";
constexpr const char* kKeyUploadDark = "ud";
constexpr const char* kKeyUnit     = "un";
constexpr const char* kKeySound    = "sn";
constexpr const char* kKeyVolume   = "vo";
constexpr const char* kKeyName     = "nm";
constexpr const char* kKeyPowerMode = "pm";
constexpr const char* kKeySleepAfter = "sa";

// Printable-ASCII bounds for the name. Space is allowed inside it, just not at
// either end.
constexpr unsigned char kAsciiFirst = 0x20;
constexpr unsigned char kAsciiLast  = 0x7E;

// --- Unit arithmetic, named rather than inlined ------------------------------
// °F = °C · 9/5 + 32. Spelled out as a fraction and an offset so the conversion
// reads as the formula it is, and so the span variants can borrow the fraction
// without the offset.
// Hundredths of a degree per editing step. The steppers move in tenths, so the
// four conversions below are the whole-degree ones with this constant changed —
// nothing else about them depends on the resolution.
constexpr int32_t kC100PerStep = 10;
constexpr int32_t kFNum        = 9;     // Fahrenheit degrees per Celsius degree
constexpr int32_t kFDen        = 5;
constexpr int32_t kFOffsetC100 = 3200;  // 32.00 °F is 0 °C

// Rounds to nearest for both signs. Integer division truncates towards zero,
// which would read -20.6 °C back as -20 while reading 20.6 as 21 — an asymmetry
// that shows up as a threshold that shifts when it crosses freezing.
constexpr int32_t div_round(int32_t num, int32_t den) {
  const int32_t half = den / 2;
  return num >= 0 ? (num + half) / den : (num - half) / den;
}

// Every conversion below is bounded by the sensor's own range, so this is a
// backstop for a corrupt stored value rather than an expected path.
constexpr int16_t clamp16(int32_t v) {
  return static_cast<int16_t>(std::clamp<int32_t>(v, INT16_MIN, INT16_MAX));
}

bool s_inited = false;

// The name initialiser is a literal rather than the MAC-derived default because
// that needs a runtime read; init() replaces it. Seeding it here instead of
// leaving it empty means the field is valid even for a caller that reads settings
// before the boot sequencer runs init(), so nothing downstream has to handle an
// unnamed post — and if esp_read_mac ever fails, this is what stands.
Settings s_cur = {
    .interval_s      = kIntervalDefaultS,
    .min_c100        = kMinDefaultC100,
    .max_c100        = kMaxDefaultC100,
    .min_from_sheet  = false,
    .max_from_sheet  = false,
    .hysteresis_c100 = kHysteresisDefaultC100,
    .graph_points    = kGraphPointsDefault,
    .upload_every_min = kUploadEveryDefaultMin,
    .upload_dark_min  = kUploadDarkDefaultMin,
    .unit            = TempUnit::Fahrenheit,
    .sound_enabled   = true,
    .volume_pct      = kVolumeDefaultPct,
    .power_mode      = kPowerModeDefault,
    .sleep_after_s   = kSleepAfterDefaultS,
    .name            = "Post-0000",
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

bool persist_str(const char* key, const char* value) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_str(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (!ok) ESP_LOGW(kTag, "failed to persist '%s'", key);
  return ok;
}

// --- the post name ---------------------------------------------------------

// Replace the seeded fallback with Post-XXXX from the low two bytes of the MAC.
// On failure the seed stands rather than leaving the post nameless.
void apply_default_name() {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    ESP_LOGW(kTag, "esp_read_mac failed; keeping the fallback name '%s'", s_cur.name);
    return;
  }
  std::snprintf(s_cur.name, sizeof(s_cur.name), "%s%02X%02X", kNamePrefix, mac[4], mac[5]);
}

// Copy `src` into `out`, keeping only printable ASCII, trimming spaces at both
// ends, and truncating to kNameMaxLen. Returns false if nothing usable survived.
//
// Sanitizing rather than rejecting is deliberate: this is the chokepoint both the
// web form and NVS load go through, and a form value is not to be trusted to have
// honoured its own maxlength. Dropping every byte >= 0x7F takes UTF-8 continuation
// bytes with the lead byte, so "café" becomes "caf" rather than a mojibake tail.
bool sanitize_name(const char* src, char* out, size_t out_size) {
  if (src == nullptr || out_size == 0) return false;
  size_t n = 0;
  for (const char* p = src; *p != '\0' && n + 1 < out_size; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < kAsciiFirst || c > kAsciiLast) continue;
    if (c == ' ' && n == 0) continue;  // no leading space
    out[n++] = static_cast<char>(c);
  }
  while (n > 0 && out[n - 1] == ' ') --n;  // no trailing space
  out[n] = '\0';
  return n > 0;
}

// --- clamping --------------------------------------------------------------
// Applied on both load and set, so a value written by an older firmware (or a
// hand-edited NVS partition) can never reach the sample task or the UI.

uint16_t clamp_interval(uint16_t v) { return std::clamp(v, kIntervalMinS, kIntervalMaxS); }
uint16_t clamp_points(uint16_t v)   { return std::clamp(v, kGraphPointsMin, kGraphPointsMax); }
// No floor: zero is a meaningful value here, not an unset one — it is "every record".
uint16_t clamp_upload(uint16_t v)   { return v > kUploadEveryMaxMin ? kUploadEveryMaxMin : v; }
int16_t  clamp_temp(int16_t v)      { return std::clamp(v, kTempFloorC100, kTempCeilC100); }
uint8_t  clamp_volume(uint8_t v)    { return v > kVolumeMaxPct ? kVolumeMaxPct : v; }
uint16_t clamp_sleep_after(uint16_t v) { return std::clamp(v, kSleepAfterMinS, kSleepAfterMaxS); }

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

  // Before the NVS read, so an unnamed post gets its MAC-derived name on every
  // boot without that default ever being written to flash.
  apply_default_name();

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
  if (nvs_get_u8(h, kKeyMinSheet, &u8) == ESP_OK) s_cur.min_from_sheet = (u8 != 0);
  if (nvs_get_u8(h, kKeyMaxSheet, &u8) == ESP_OK) s_cur.max_from_sheet = (u8 != 0);
  if (nvs_get_u16(h, kKeyHyst, &u16) == ESP_OK) s_cur.hysteresis_c100 = clamp_hysteresis(static_cast<int16_t>(u16));
  if (nvs_get_u16(h, kKeyPoints, &u16) == ESP_OK) s_cur.graph_points = clamp_points(u16);
  if (nvs_get_u16(h, kKeyUpload, &u16) == ESP_OK) s_cur.upload_every_min = clamp_upload(u16);
  if (nvs_get_u16(h, kKeyUploadDark, &u16) == ESP_OK) s_cur.upload_dark_min = clamp_upload(u16);
  if (nvs_get_u8(h, kKeyUnit, &u8) == ESP_OK && u8 <= 1) s_cur.unit = static_cast<TempUnit>(u8);
  if (nvs_get_u8(h, kKeySound, &u8) == ESP_OK) s_cur.sound_enabled = (u8 != 0);
  if (nvs_get_u8(h, kKeyVolume, &u8) == ESP_OK) s_cur.volume_pct = clamp_volume(u8);
  // Range-checked rather than clamped: an unknown mode from a newer firmware has
  // no nearest valid neighbour, and falling back to the default is the only
  // reading of it that is not a guess about what the user meant.
  if (nvs_get_u8(h, kKeyPowerMode, &u8) == ESP_OK &&
      u8 <= static_cast<uint8_t>(PowerMode::AlwaysSleep)) {
    s_cur.power_mode = static_cast<PowerMode>(u8);
  }
  if (nvs_get_u16(h, kKeySleepAfter, &u16) == ESP_OK) {
    s_cur.sleep_after_s = clamp_sleep_after(u16);
  }

  // Sanitized on the way out of NVS as well as in, so a value written by hand or
  // by a firmware with a wider limit can't reach the UI. A stored name longer
  // than the buffer makes nvs_get_str return ESP_ERR_NVS_INVALID_LENGTH without
  // touching name_buf, which leaves the default standing — the right outcome.
  char name_buf[kNameMaxLen + 1] = {};
  size_t name_len = sizeof(name_buf);
  if (nvs_get_str(h, kKeyName, name_buf, &name_len) == ESP_OK) {
    char clean[kNameMaxLen + 1];
    if (sanitize_name(name_buf, clean, sizeof(clean))) {
      std::snprintf(s_cur.name, sizeof(s_cur.name), "%s", clean);
    }
  }
  nvs_close(h);

  s_inited = true;
  // Sized for what the compiler must assume — ten digits per %u — not for the four
  // that clamp_upload() can actually produce.
  char upload_text[64];
  std::snprintf(upload_text, sizeof(upload_text), "%umin lit / %umin dark (0=every)",
                static_cast<unsigned>(s_cur.upload_every_min),
                static_cast<unsigned>(s_cur.upload_dark_min));
  ESP_LOGI(kTag, "'%s': interval %us, band %.2f..%.2f C%s (hyst %.2f), %u graph points, "
                 "upload %s, %s, sound %s @ %u%%",
           s_cur.name,
           static_cast<unsigned>(s_cur.interval_s),
           static_cast<double>(c100_to_c(s_cur.min_c100)),
           static_cast<double>(c100_to_c(s_cur.max_c100)),
           s_cur.min_from_sheet || s_cur.max_from_sheet ? " from the Sheet" : "",
           static_cast<double>(c100_to_c(s_cur.hysteresis_c100)),
           static_cast<unsigned>(s_cur.graph_points),
           upload_text,
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

esp_err_t set_band_from_sheet(SheetBound min, SheetBound max) {
  const int16_t lo = min.set ? clamp_temp(min.c100) : s_cur.min_c100;
  const int16_t hi = max.set ? clamp_temp(max.c100) : s_cur.max_c100;
  // Refused rather than repaired: swapping or widening would store a band nobody wrote,
  // and the Sheet's band column shows the post kept its old one.
  if (hi - lo < kSheetBandMinGapC100) {
    ESP_LOGW(kTag, "band %.2f..%.2f C from the Sheet refused: inverted or too narrow",
             static_cast<double>(c100_to_c(lo)), static_cast<double>(c100_to_c(hi)));
    return ESP_ERR_INVALID_ARG;
  }
  bool ok = true;
  if (min.set != s_cur.min_from_sheet) {
    s_cur.min_from_sheet = min.set;
    ok = persist<uint8_t>(kKeyMinSheet, min.set ? 1 : 0) && ok;
  }
  if (max.set != s_cur.max_from_sheet) {
    s_cur.max_from_sheet = max.set;
    ok = persist<uint8_t>(kKeyMaxSheet, max.set ? 1 : 0) && ok;
  }
  if (lo != s_cur.min_c100 || hi != s_cur.max_c100) {
    ESP_LOGI(kTag, "band set from the Sheet: %.2f..%.2f C",
             static_cast<double>(c100_to_c(lo)), static_cast<double>(c100_to_c(hi)));
  }
  ok = set_band(lo, hi) == ESP_OK && ok;
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

esp_err_t set_upload_every_min(uint16_t v) {
  v = clamp_upload(v);
  if (v == s_cur.upload_every_min) return ESP_OK;
  s_cur.upload_every_min = v;
  return persist<uint16_t>(kKeyUpload, v) ? ESP_OK : ESP_FAIL;
}

esp_err_t set_upload_dark_min(uint16_t v) {
  v = clamp_upload(v);
  if (v == s_cur.upload_dark_min) return ESP_OK;
  s_cur.upload_dark_min = v;
  return persist<uint16_t>(kKeyUploadDark, v) ? ESP_OK : ESP_FAIL;
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

esp_err_t set_power_mode(PowerMode m) {
  if (m > PowerMode::AlwaysSleep) m = kPowerModeDefault;
  if (m == s_cur.power_mode) return ESP_OK;
  s_cur.power_mode = m;
  return persist<uint8_t>(kKeyPowerMode, static_cast<uint8_t>(m)) ? ESP_OK : ESP_FAIL;
}

esp_err_t set_sleep_after_s(uint16_t v) {
  v = clamp_sleep_after(v);
  if (v == s_cur.sleep_after_s) return ESP_OK;
  s_cur.sleep_after_s = v;
  return persist<uint16_t>(kKeySleepAfter, v) ? ESP_OK : ESP_FAIL;
}

esp_err_t set_volume_pct(uint8_t pct) {
  pct = clamp_volume(pct);
  if (pct == s_cur.volume_pct) return ESP_OK;
  s_cur.volume_pct = pct;
  return persist<uint8_t>(kKeyVolume, pct) ? ESP_OK : ESP_FAIL;
}

esp_err_t set_name(const char* s) {
  char clean[kNameMaxLen + 1];
  if (!sanitize_name(s, clean, sizeof(clean))) {
    // A blank submission means "go back to the default", which is more useful
    // than an error the form would have to render.
    apply_default_name();
  } else {
    if (std::strcmp(clean, s_cur.name) == 0) return ESP_OK;
    std::snprintf(s_cur.name, sizeof(s_cur.name), "%s", clean);
  }
  ESP_LOGI(kTag, "name set to '%s'", s_cur.name);
  return persist_str(kKeyName, s_cur.name) ? ESP_OK : ESP_FAIL;
}

alert::Thresholds thresholds() {
  return alert::sanitize({s_cur.min_c100, s_cur.max_c100, s_cur.hysteresis_c100});
}

esp_err_t reset_to_defaults() {
  // The name deliberately survives a reset. It is the post's identity, not a
  // tunable: a device that forgets which tank it is watching because someone
  // reset the alarm band is surprising, and recovering the name costs a trip to
  // the web form. nvs_erase_all takes the key with everything else, so it is
  // written back afterwards.
  char keep_name[kNameMaxLen + 1];
  std::snprintf(keep_name, sizeof(keep_name), "%s", s_cur.name);

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
      .min_from_sheet  = false,
      .max_from_sheet  = false,
      .hysteresis_c100 = kHysteresisDefaultC100,
      .graph_points    = kGraphPointsDefault,
      .upload_every_min = kUploadEveryDefaultMin,
      .upload_dark_min  = kUploadDarkDefaultMin,
      .unit            = TempUnit::Fahrenheit,
      .sound_enabled   = true,
      .volume_pct      = kVolumeDefaultPct,
      .power_mode      = kPowerModeDefault,
      .sleep_after_s   = kSleepAfterDefaultS,
      .name            = {},
  };
  std::snprintf(s_cur.name, sizeof(s_cur.name), "%s", keep_name);
  persist_str(kKeyName, s_cur.name);

  ESP_LOGW(kTag, "settings reset to defaults (name '%s' kept)", s_cur.name);
  return ESP_OK;
}

float display_value(int16_t c100) {
  return s_cur.unit == TempUnit::Celsius ? c100_to_c(c100) : c100_to_f(c100);
}

const char* unit_suffix() {
  return s_cur.unit == TempUnit::Celsius ? "°C" : "°F";
}

int16_t display_tenths(int16_t c100) {
  if (s_cur.unit == TempUnit::Celsius) return clamp16(div_round(c100, kC100PerStep));
  // (c100·9/5 + 3200) / 10, folded into one division so the scale factor and the
  // step rounding don't each round separately and compound.
  return clamp16(div_round(static_cast<int32_t>(c100) * kFNum + kFOffsetC100 * kFDen,
                           kC100PerStep * kFDen));
}

int16_t c100_from_tenths(int16_t tenths) {
  const int32_t c100 =
      s_cur.unit == TempUnit::Celsius
          ? static_cast<int32_t>(tenths) * kC100PerStep
          : div_round((static_cast<int32_t>(tenths) * kC100PerStep - kFOffsetC100) * kFDen,
                      kFNum);
  return static_cast<int16_t>(std::clamp<int32_t>(c100, kTempFloorC100, kTempCeilC100));
}

int16_t display_span_tenths(int16_t c100) {
  if (s_cur.unit == TempUnit::Celsius) return clamp16(div_round(c100, kC100PerStep));
  // No +32: an offset belongs to a point on the scale, not to a distance along it.
  return clamp16(div_round(static_cast<int32_t>(c100) * kFNum, kC100PerStep * kFDen));
}

int16_t c100_from_span_tenths(int16_t tenths) {
  if (s_cur.unit == TempUnit::Celsius) return clamp16(static_cast<int32_t>(tenths) * kC100PerStep);
  return clamp16(div_round(static_cast<int32_t>(tenths) * kC100PerStep * kFDen, kFNum));
}

}  // namespace thermopost::settings
