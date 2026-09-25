#pragma once

#include <cstdint>

// Pin map for the LCDwiki **ES3C28P** 2.8" ESP32-S3 display board — the module
// Hosyond sells as "ESP32-S3 Touchscreen Module, 2.8\" 240x320 IPS LCD".
//
// Source: https://www.lcdwiki.com/2.8inch_ESP32-S3_Display
//
// VERIFIED on hardware by the bring-up probe (22/22): N16R8 confirmed (16 MB
// flash, 8 MB octal PSRAM), I2C0 answers at 0x18 / 0x1A / 0x38, and the MCP9808
// reads a plausible room temperature.
//
// The probe could not settle the LCD controller — both ID reads came back
// degenerate (0xD3 -> FF FF FF FF, 0x04 -> 00 00 00 00), because those reads
// need controller-specific dummy clocks that esp_lcd's generic parameter read
// does not issue. The display driver settled it instead: the ILI9341 command
// set drives this panel correctly, so the wiki is right. It is one of the "IPS"
// variants and needs INVON (see kInvertColor in display.cpp).

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
// I²C bus (I2C0) — capacitive touch, the ES8311 codec's control port, and the
// MCP9808 hanging off the board's 4-pin "I2C" connector.
//
// Confirmed against the physical board: the silkscreen beside that connector
// reads 3.3V / GND / IO15(SCL) / IO16(SDA).
// ---------------------------------------------------------------------------
inline constexpr int kI2cSda = 16;
inline constexpr int kI2cScl = 15;
inline constexpr int kI2cFreqHz = 400'000;

// --- Touch (FT6336G) ---
inline constexpr int kTouchInt   = 17;  // RTC-capable (0..21) → usable as an ext0 wake source
inline constexpr int kTouchReset = 18;
inline constexpr uint8_t kTouchI2cAddr = 0x38;  // FT6x36 family default

// --- ES8311 audio codec (control port) ---
// Address is 0x18 with CE strapped low, 0x19 with it high. This board straps it
// low, so the codec sits at 0x18 — the MCP9808's default address, which is why
// the sensor's A1 pin is strapped high (see below).
inline constexpr uint8_t kEs8311I2cAddrLow  = 0x18;
inline constexpr uint8_t kEs8311I2cAddrHigh = 0x19;

// ---------------------------------------------------------------------------
// Audio — ES8311 codec into an FM8002E amp driving the onboard speaker
// ---------------------------------------------------------------------------
// Three of the five values below are NOT what the LCDwiki page says, and each was
// established by playing a tone on every combination until one made a sound. The
// page's names are written from the CODEC's point of view, not the ESP32's, which
// is the trap: taken literally they send playback to the microphone pin.
//
// Every wrong combination fails the same way — the codec configures perfectly over
// I²C, reports itself powered, unmuted and correctly clocked, and the speaker stays
// silent. There is no register that reveals any of this, which is why it took a
// sweep rather than a reading.
inline constexpr int kI2sBclk = 5;
inline constexpr int kI2sLrck = 7;
// Named from the ESP32's point of view, so the wiki's "DOUT=6 / DIN=8" is inverted
// here. IO8 carries playback to the codec; IO6 is the microphone's return path.
inline constexpr int kI2sDout = 8;  // ESP32-S3 → codec (playback: the alert tone)
inline constexpr int kI2sDin  = 6;  // codec → ESP32-S3 (mic; unused in v1)

// The wiki lists MCLK on IO4, and the codec will not run from it. Whether the trace
// is absent or just wrong, the ES8311 can derive its own master clock from BCLK and
// does, so nothing needs this pin — see kI2sMclkRouted.
inline constexpr int kI2sMclk = 4;
// False until something proves otherwise: with it false the pin is left alone
// entirely, so IO4 is effectively spare. Flip it and the codec is told to expect an
// external MCLK on IO4 — which is the one-line experiment that would settle it.
inline constexpr bool kI2sMclkRouted = false;

// Amplifier enable — active LOW on this board, the opposite of what the FM8002E's
// datasheet default and the wiki both imply. Driven by the codec driver, which
// raises it on open and drops it on close.
inline constexpr int  kAudioAmpEnable = 1;
inline constexpr bool kAudioAmpActiveLow = true;

// ---------------------------------------------------------------------------
// MCP9808 surface-temperature sensor — on the shared I²C bus above, via the
// board's 4-pin "I2C" connector (3.3V / GND / IO15 / IO16).
// ---------------------------------------------------------------------------
// An earlier revision put this on a private I2C1 bus over the expansion header,
// to dodge an address collision: the ES8311 answers at 0x18/0x19 and the
// MCP9808's default is 0x18. Two facts off the physical board retire that plan.
//
// First, the expansion header carries ONLY IO2/IO3/IO14/IO21 — no 3.3V, no GND.
// A private bus would still have to steal power from the I2C connector, so it
// costs two connectors instead of one and buys nothing.
//
// Second, the collision is a non-problem: the breakout exposes A0/A1/A2, which
// exist precisely to move the device. Strapping A1 high puts it at 0x1A, clear
// of both codec addresses. Sharing is otherwise harmless — ESP-IDF's i2c_master
// driver takes scl_speed_hz *per device*, so the sensor can clock at 100 kHz on
// its flying lead while touch keeps its 400 kHz.
//
// Address = 0x18 + (A2<<2 | A1<<1 | A0), latched at power-up — a strap change
// needs a full power cycle, not a reset. The driver does not hardcode this: it
// scans 0x18..0x1F and identifies the part by its manufacturer ID, so any strap
// configuration works and the codec can never be mistaken for the sensor.
//
// All three strap pins must be tied to something. Per the datasheet
// (DS20005095B §3.5 plus the DC spec's ±5 µA leakage and CMOS thresholds) they
// are high-impedance inputs with no internal pull-down, so an open pin is
// undefined rather than zero — the address can then move with noise or board
// leakage, even between power cycles. Some breakouts fit their own pull-downs;
// not all do.
inline constexpr uint8_t kSensorI2cAddrDefault = 0x1A;  // A1 strapped high
inline constexpr uint8_t kSensorI2cAddrFirst   = 0x18;  // scan range, inclusive
inline constexpr uint8_t kSensorI2cAddrLast    = 0x1F;
inline constexpr int kSensorI2cFreqHz = 100'000;  // conservative for a flying lead

// MCP9808 ALERT — open-drain, needs a pull-up. RTC-capable, so it can wake the SoC
// from deep sleep via ext1 while the sensor watches the band on its own.
inline constexpr int kSensorAlert = 21;
// Active low: sensor.cpp's alert config leaves the polarity bit clear.
inline constexpr int kSensorAlertActiveLevel = 0;

// ---------------------------------------------------------------------------
// Expansion header — IO2, IO3, IO14, IO21. Signal pins only, no power rail.
// GPIO3 is an ESP32-S3 strapping pin (JTAG source select), so it is a poor
// choice for anything that idles high.
// ---------------------------------------------------------------------------
inline constexpr int kExtIo2  = 2;
inline constexpr int kExtIo3  = 3;
inline constexpr int kExtIo14 = 14;
inline constexpr int kExtIo21 = 21;  // == kSensorAlert when the ALERT line is wired

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
