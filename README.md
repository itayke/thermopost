# Thermo Post

A surface-temperature monitor built on the Hosyond 2.8" ESP32-S3 touchscreen
module. It samples an MCP9808 on a fixed cadence, shows the current reading with
a timeline graph of recent history, alerts on screen and through the onboard
speaker when the temperature leaves a configured min/max band, and mirrors every
reading to a Google Sheet over Wi-Fi.

By Itay Keren <itay@untame.com>. Copyright (c) 2026 Itay Keren.

> **Early-stage — the firmware is at bring-up and the pin map has not been
> verified against a physical board yet.** See [Status](#status).

## Hardware

**LCDwiki ES3C28P**, sold by Hosyond as "ESP32-S3 Touchscreen Module, 2.8"
240x320 IPS LCD". ESP32-S3 **N16R8** (16 MB flash, 8 MB OPI PSRAM).

| Block | Part | Interface |
|---|---|---|
| Display | ILI9341V, 240×320 (used rotated to 320×240 landscape) | SPI2 |
| Touch | FT6336G | I²C0 @ 0x38 |
| Audio | ES8311 codec → FM8002E amp → onboard speaker | I²S + I²C0 |
| Charging | TP4054, battery sense on GPIO9 | ADC1_CH8 |
| Storage | microSD (unused in v1) | SDIO |

**⚠️ The pin map in [`main/board_pins.hpp`](main/board_pins.hpp) is transcribed
from [lcdwiki.com](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display) and has
never been checked against a physical unit.** Vendor listings say "IPS" where the
wiki says ILI9341V, and rebranded boards do change controllers between production
runs. That is what the bring-up probe below exists to settle.

### Wiring the MCP9808

The temperature sensor is not part of the module. It goes on the board's 4-pin
**`I2C` connector** (silkscreened `3.3V / GND / IO15(SCL) / IO16(SDA)`), which
breaks out the same bus the touch controller and codec are on:

| MCP9808 breakout | Board |
|---|---|
| VCC | `3.3V` |
| GND | `GND` |
| SCL | `IO15(SCL)` |
| SDA | `IO16(SDA)` |
| **A1** | **`3.3V`** — see below |
| A0, A2 | `GND` |
| ALERT | leave open (optional: `IO21`, needs a pull-up) |

**Strapping A1 high is the one non-obvious step.** The MCP9808's address is
`0x18 + (A2<<2 | A1<<1 | A0)`, so it defaults to `0x18` — exactly where the
ES8311 codec lives. A1 high moves it to `0x1A`, clear of both `0x18` and `0x19`.
The straps are latched at power-up, so **a strap change needs a full power cycle,
not a reset button press.**

The firmware doesn't hardcode the address: it scans `0x18..0x1F` and identifies
the part by its manufacturer ID, so any strap combination works and the codec
can never be mistaken for the sensor. If you leave the straps alone, the probe
will tell you the address collided and what to do about it.

Sharing one bus is fine here — ESP-IDF's `i2c_master` driver takes
`scl_speed_hz` *per device*, so the sensor clocks at 100 kHz on its flying lead
while touch keeps its 400 kHz.

The expansion header (`IO2 / IO3 / IO14 / IO21`) carries signal pins only — no
3.3V or GND — so it can't host the sensor by itself. `IO21` is reserved for the
optional ALERT line because it is RTC-capable, which would let a hardware
over-temperature alarm wake the board from deep sleep. `IO3` is best left alone;
it is an ESP32-S3 strapping pin.

## Build & flash

Requires ESP-IDF ≥ v5.5 (`~/esp/esp-idf`), with `cmake` + `ninja` on PATH.

```sh
get_idf                          # put idf.py + xtensa toolchain on PATH
cd /Users/itaykeren/Code/GitHub/thermopost

idf.py set-target esp32s3        # first time only
idf.py build
idf.py -p /dev/cu.usbmodem<N> flash monitor   # Ctrl+] to exit
```

Editor red squigglies on `esp_err_t` and friends before the first build are
expected — clangd has no context until the build writes
`build/compile_commands.json`. The xtensa-only flag warnings (`-mlongcalls`, …)
are cosmetic and never go away.

## Usage

The current `app_main` is **a one-shot hardware probe, not the app**. Flash it,
watch the serial log, and it answers the five questions that everything else
depends on:

1. Is this really an N16R8 (16 MB flash, 8 MB PSRAM)?
2. Who is on the internal I²C bus? (expect touch at 0x38, ES8311 at 0x18 or 0x19)
3. Is the MCP9808 alive on its own bus, reading a plausible room temperature?
4. What does the LCD controller report as its ID?
5. Does GPIO45 actually control the backlight? *(watch the screen — it blinks 3×)*

It then self-tests `storage` and `settings`, which need no sensor or panel.

The log ends in a `GATE PASS` / `GATE FAIL` summary. **On a failure, fix
`board_pins.hpp` or the wiring before starting on the drivers** — writing drivers
against a wrong pin map is how bring-up turns into a two-day guessing game.

Two caveats:

- **The controller-ID read is advisory.** Those reads need controller-specific
  dummy clocks, so garbage is inconclusive, not a failure. The definitive test is
  whether the display step puts a correct image on screen.
- **The probe erases the log and resets settings** to get a known starting state.
  Harmless now; it must not survive into the real `app_main`.

If the MCP9808 isn't wired yet, everything else still reports — that one check
just fails.

## Host tests

The alarm decision logic is IDF-free by design, so it runs on the dev machine:

```sh
./tests/host/run.sh                # build + run all cases
./tests/host/run.sh "[hysteresis]" # filter by tag
```

Catch2 v2 is vendored under `tests/host/third_party/` (no install). Design rule:
if a test ever needs an IDF include in `alert_logic.*`, the test isn't wrong —
the code is. Push the IDF dependency into the `alert.cpp` wrapper instead.

## Project layout

```
.
├── partitions.csv          nvs + factory (4 MB) + littlefs (~12 MB)
├── sdkconfig.defaults      PSRAM, flash, partition table, FreeRTOS tick, console
├── main/                   bring-up probe + board_pins (becomes the boot sequencer)
├── components/
│   ├── storage/            LittleFS append-log + rotation with stable absolute indices
│   ├── settings/           NVS-backed interval / band / units / sound
│   └── alert/              alarm decision logic (IDF-free, host-tested)
└── tests/host/             Catch2 unit tests, no IDF
```

### Two design notes worth knowing up front

**Rotation and absolute indices.** Cloud sync tracks what it has uploaded with a
high-water mark, so a record's index has to mean the same thing forever. But at a
record a minute the log must be rotated, and rotation moves every record's file
position. `storage` keeps a persistent *base index* in NVS and advances it by
exactly as many records as each rotation drops, so absolute indices are still
assigned once and never reused. Because that makes rotation a two-store
transaction (a file rename plus an NVS write), the intent is journalled first and
`init()` rolls an interrupted rotation forward — see the comment on
`rotate_locked()` in [`storage.cpp`](components/storage/storage.cpp).

**Time comes from Wi-Fi.** This board has no battery-backed RTC, so SNTP sets the
system clock and the ESP32's own RTC timer holds it (including across deep
sleep). Records written before the first sync carry `epoch_s = 0`, which means
"order by `uptime_us` instead". A full power cycle loses the clock until Wi-Fi
returns.

## Status

**Bring-up. The hardware has not been verified yet.**

| | |
|---|---|
| ✅ Built | Repo skeleton, bring-up probe, `storage`, `settings`, `alert` decision logic, host tests |
| ⛔️ Blocked | The bring-up gate — needs the board plugged in (see [Usage](#usage)) |
| ⬜️ Not started | display, touch, sensor, UI, alerts, cloud, power |

Everything built so far is deliberately independent of the unverified pin map.
Build order from here: display → touch → sensor → main screen → menu → alerts →
cloud → power. Each step ends at something flashable and observable.

### Open questions

- **Power model.** The plan is always-on first (dim → backlight off → deep sleep
  on long idle, with a timer wake so readings continue), then *measure* before
  deciding whether deeper duty-cycling is worth the complexity. The numbers that
  decide it: average current with the screen on, dimmed, backlight off, and
  deep-sleeping between one-minute readings.
- **Backlight dimming.** The wiki documents GPIO45 as a plain high/low enable. If
  LEDC PWM doesn't actually dim it, `set_brightness()` collapses to on/off and
  the power model loses its "dim" tier.
- **The ES8311 tone.** Driving a codec over I²S to produce a simple alert beep is
  the least certain part of the build. It is kept behind a two-function API so a
  failure degrades to visual-only alerts rather than blocking anything else.
- **Default alarm band.** Currently 15–30 °C — deliberately generic, enough to
  make a fresh device sane and the alarm path exercisable. Expected to be the
  first thing a user changes.
