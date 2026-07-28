#pragma once

#include <cstdint>

// Pin map for the LCDwiki **ES3C28P** 2.8" ESP32-S3 display board — the module
// Hosyond sells as "ESP32-S3 Touchscreen Module, 2.8\" 240x320 IPS LCD".
//
// Source: https://www.lcdwiki.com/2.8inch_ESP32-S3_Display
//
// ⚠️  UNVERIFIED until the bring-up probe (app_main.cpp) is run on the actual
// unit. Vendor listings say "IPS" where the wiki says ILI9341V, and rebranded
// boards do quietly change controllers between production runs. If a pin behaves
// unexpectedly, re-run the probe before assuming a code bug — that is exactly
// what it is for.

namespace thermopost::board {

// ---------------------------------------------------------------------------
// LCD — ILI9341V, 4-line SPI on SPI2_HOST
// ---------------------------------------------------------------------------
inline constexpr int kLcdSclk = 12;
inline constexpr int kLcdMosi = 11;
inline constexpr int kLcdMiso = 13;  // wired, so the controller ID is readable
inline constexpr int kLcdCs   = 10;
inline constexpr int kLcdDc   = 46;
// The panel's reset is tied to the module's own reset rail, not a GPIO — the
// esp_lcd panel config takes -1 and skips its reset pulse.
inline constexpr int kLcdReset = -1;
// Backlight enable. GPIO45 is an ESP32-S3 strapping pin (VDD_SPI select) and is
// NOT in the RTC GPIO range (0..21), so it cannot be held through deep sleep —
// it falls low and the backlight goes off, which is the behavior we want.
// Driven by LEDC PWM for dimming; the wiki documents it only as a high/low
// enable, so verify dimming actually works before relying on the "dim" power tier.
inline constexpr int kLcdBacklight = 45;

// The glass is 240×320 portrait. The UI is landscape, produced by swapping X/Y
// in the panel's MADCTL — so LVGL and every UI constant see 320×240.
inline constexpr int kPanelNativeW = 240;
inline constexpr int kPanelNativeH = 320;
inline constexpr int kLcdHRes = 320;
inline constexpr int kLcdVRes = 240;

inline constexpr int kLcdSpiFreqHz = 40'000'000;
inline constexpr int kLcdBitsPerPixel = 16;  // RGB565

// ---------------------------------------------------------------------------
// Internal I²C bus (I2C0) — capacitive touch + the ES8311 codec's control port
// ---------------------------------------------------------------------------
inline constexpr int kI2cSda = 16;
inline constexpr int kI2cScl = 15;
inline constexpr int kI2cFreqHz = 400'000;

// --- Touch (FT6336G) ---
inline constexpr int kTouchInt   = 17;  // RTC-capable (0..21) → usable as an ext0 wake source
inline constexpr int kTouchReset = 18;
inline constexpr uint8_t kTouchI2cAddr = 0x38;  // FT6x36 family default

// --- ES8311 audio codec (control port) ---
// Address is 0x18 with CE strapped low, 0x19 with it high. The probe reports
// which — and note it collides with the MCP9808's default 0x18, which is the
// reason the sensor gets its own bus below rather than sharing this one.
inline constexpr uint8_t kEs8311I2cAddrLow  = 0x18;
inline constexpr uint8_t kEs8311I2cAddrHigh = 0x19;

// ---------------------------------------------------------------------------
// Audio — ES8311 codec into an FM8002E amp driving the onboard speaker
// ---------------------------------------------------------------------------
inline constexpr int kI2sMclk = 4;
inline constexpr int kI2sBclk = 5;
inline constexpr int kI2sDout = 6;  // ESP32-S3 → codec (playback: the alert tone)
inline constexpr int kI2sLrck = 7;
inline constexpr int kI2sDin  = 8;  // codec → ESP32-S3 (mic; unused in v1)
// Amplifier enable. Held low except while a tone is playing — the FM8002E idles
// at a few mA and audibly pops if it is left enabled.
inline constexpr int kAudioAmpEnable = 1;

// ---------------------------------------------------------------------------
// External expansion header — free GPIOs: 2, 3, 14, 21
// ---------------------------------------------------------------------------
// MCP9808 surface-temperature sensor on its OWN I²C bus (I2C1), deliberately not
// the internal bus. Two reasons, in order of weight:
//   1. The ES8311 answers at 0x18/0x19 and the MCP9808's default is 0x18 — a
//      direct address collision on the shared bus.
//   2. Electrically cleaner: the sensor sits on a flying lead at the measured
//      surface, and a long stub off the bus that also clocks touch at 400 kHz is
//      asking for trouble.
// GPIO3 is skipped on purpose — it is an ESP32-S3 strapping pin (JTAG source
// select), so it is a poor choice for a line that idles high with a pull-up.
inline constexpr int kSensorSda = 2;
inline constexpr int kSensorScl = 14;
inline constexpr int kSensorI2cFreqHz = 100'000;  // conservative for a flying lead

// MCP9808 address is 0x18..0x1F via the A0/A1/A2 straps; breakouts ship all-low.
inline constexpr uint8_t kSensorI2cAddr = 0x18;

// MCP9808 ALERT — open-drain, needs a pull-up (Adafruit-style breakouts have
// one). Unused in v1, but wired now because GPIO21 is RTC-capable: it lets a
// hardware over-temperature alarm wake the SoC from deep sleep via ext1 without
// the firmware sampling at all.
inline constexpr int kSensorAlert = 21;

// GPIO3 intentionally left free for future expansion.

// ---------------------------------------------------------------------------
// Battery — TP4054 charger; divided battery voltage sensed on GPIO9 (ADC1_CH8)
// ---------------------------------------------------------------------------
inline constexpr int kBatteryAdc = 9;

// ---------------------------------------------------------------------------
// microSD (SDIO) — present on the board, unused in v1. Listed so these pins are
// visibly spoken for and don't get reassigned.
// ---------------------------------------------------------------------------
inline constexpr int kSdClk = 38;
inline constexpr int kSdCmd = 40;
inline constexpr int kSdD0  = 39;
inline constexpr int kSdD1  = 41;
inline constexpr int kSdD2  = 48;
inline constexpr int kSdD3  = 47;

}  // namespace thermopost::board
