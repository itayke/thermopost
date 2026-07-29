# Thermo Post

A surface-temperature monitor built on the Hosyond 2.8" ESP32-S3 touchscreen
module. It samples an MCP9808 on a fixed cadence, shows the current reading with
a timeline graph of recent history, alerts on screen and through the onboard
speaker when the temperature leaves a configured min/max band, and mirrors every
reading to a Google Sheet over Wi-Fi.

By Itay Keren <itay@untame.com>. Copyright (c) 2026 Itay Keren.

> **Early-stage.** The pin map, display and sensor are confirmed on hardware and
> the main screen is up. The menu, alerts and cloud sync are not built yet. See
> [Status](#status).

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

The pin map in [`main/board_pins.hpp`](main/board_pins.hpp) is transcribed from
[lcdwiki.com](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display) and **confirmed
on hardware** — the bring-up probe checked every pin except the panel, and the
display driver confirmed that by drawing. The controller takes the ILI9341
command set; it is one of the "IPS" variants, so it needs `INVON` (without it
the image comes up as a photographic negative).

The Type-C port is the ESP32-S3's native USB-Serial-JTAG, not a bridge chip, so
it enumerates as `/dev/cu.usbmodem*`.

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

**Tie A0 and A2 to GND rather than leaving them open**, unless you have
confirmed your breakout has pull-down resistors (Adafruit's does; the CJMCU-9908
is undocumented — measure A0-to-GND, ~10 kΩ means they are fitted). The bare
chip does not: the datasheet (DS20005095B §3.5) calls them plain "device address
input pins", and the DC spec lists only a ±5 µA leakage current with CMOS
thresholds, which is a high-impedance input with nothing holding it. Floating
them doesn't read as zero — it reads as undefined, and the address can then shift
with noise, board leakage or flux residue, including *between power cycles*. That
failure presents as a sensor that intermittently vanishes or moves, which is a
far worse afternoon than two extra solder joints.

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

Flashing brings the board up through `display → nvs → storage → settings → i2c →
sensor → touch → recorder → ui` and draws the main screen:

```
 (☰ Menu)      72.4°F            ↑ 86.0
 14:32                           ↓ 59.0
 ┌────────────────────────────────────┐
 │- - - - - - - - - - - - - - - - - - │   max, dashed
 │        ╭─╮      ╭──╮               │
 │   ╱  ╱   ╲____╱    ╲___╱╲___       │
 │- - - - - - - - - - - - - - - - - - │   min, dashed
 └────────────────────────────────────┘
  -2h            -1h             now
 North Tank
 #recs live 37  total 4213  uploaded --
```

The big number is the current reading in the configured unit (°F by default) and
it changes colour with the band — white inside it, warm above the maximum, cold
below the minimum, amber when the sensor isn't answering. The colour says which
way the temperature went before the digits are legible, which is what a screen
read across a room actually conveys.

**Tapping the reading swaps °C and °F**, thresholds included, and the choice is
saved. Nothing stored changes: every value on the device is hundredths of a degree
Celsius and `settings::display_value()` is the only thing that converts, so the
unit is a property of the screen and never of the log or the Sheet.

The two threshold chips put their arrows at a fixed offset from the right edge and
centre the numbers beside them. Formatting arrow and number into one right-aligned
label is the obvious way to do it and it looks wrong: the arrow slides sideways as
the number's width changes, so the two chips visibly fail to line up with each
other.

The graph plots one point per logged record, newest at the right, with the
thresholds as dashed lines. Its y range always covers both the data and the
band, so the limits stay visible even when the reading is nowhere near them. The
left-hand caption is the window the plot spans — `graph_points × interval`, so
120 points at a minute each is `-2h`. Until the ring fills, the left of the plot
is empty rather than stretched: a short history shouldn't imply a window that
hasn't elapsed yet.

The clock sits under the menu glyph and reads `no clock` until SNTP lands —
showing a 1970 timestamp would be a bug report rather than a fact. The phrase is
short because at Montserrat 14 the accurate one, "clock not set", is wide enough
to reach under the centred readout; the menu will have room to say it properly.

The status strip has two rows: the post's name, and three record counters that
answer three different questions.

| | |
|---|---|
| `live` | appended since boot — **frozen means logging has stopped** |
| `total` | ever written, across every boot and rotation |
| `uploaded` | how far the Sheet has caught up (`--` until M8) |

`live` counts completed writes, not attempts. That distinction is the whole point:
a counter that keeps climbing while the filesystem is full would defeat the only
job it has, which is to tell a stopped logger apart from a very stable room when
the temperature itself isn't moving. If sampling is what you want to watch rather
than logging, the graph still gains a point on a failed append — the ring is
updated before the write, so a broken filesystem costs the log and not the picture.

`total` is the absolute index `storage` assigns, so it is directly comparable to
cloud sync's high-water mark and to the `#n` in the serial log. It is *not* the
number of records on disk, which is smaller once rotation has dropped a block.

Two cadences, deliberately not one: the sensor is sampled every second so the
readout is always fresh, while a record is written on the configured *interval*
(a minute by default) so flash lasts and the Sheet stays readable. Only the
second is a setting. If the sensor is missing or the lead is pulled, the readout
says so and the sampler keeps rescanning, so reseating the connector recovers
without a reboot.

The menu pill is espressopost's, scaled for a 320 px screen: an amber outlined
capsule holding a hamburger glyph and the word *Menu* in a centred flex row.
Outline, label and glyph all tint green under a finger, and the tap is logged, but
it has nowhere to go until M6. The glyph is painted as three 1 px bars rather than
set as `LV_SYMBOL_BARS`, which the built-in font does carry — espressopost's
reasoning applies here too: a painted glyph tints and scales with the layout
instead of being pinned to whatever the font designer chose.

Because the pill holds a word as well as a glyph, the readout is centred in the
gap *between* the side columns rather than on the screen. That is nine pixels off
true centre, which is invisible, and it means even the widest value the MCP9808 can
report can't reach under the pill or the chips.

That tap, and the unit toggle, are also the standing check on the touch
orientation: the flags in [`touch.cpp`](components/touch/touch.cpp) can't be
derived from the display's, because the panel and the touch glass have their own
origin corners. Dragging settled them — the horizontal axis was already correct
and the vertical inverted, which is `mirror_x`, because the mirrors act in the
controller's frame that `swap_xy` then transposes. The two targets now sit in
opposite corners, which is a stricter check than one dot ever was.

### The post's name

Thermo Posts are meant to run in a group, one per tank, so each carries a name
that is sent with every reading — a row in a shared Sheet is meaningless without
knowing which tank produced it. Until one is set the post calls itself
`Post-XXXX` after the low bytes of its MAC, which is unique across a group with
no user action and answers "which of these five identical boxes am I configuring".

The name is the one setting the device menu will *not* change. It is the only
free-text field, and an on-screen keyboard on a 2.8" panel is the worst possible
way to enter one, so it belongs to the web config form (M8) — which is being
filled in from a phone or laptop with a real keyboard anyway. Everything else is
a number or a toggle and will be editable from both.

24 characters, printable ASCII. The length is the status strip's share of a
320 px line; the ASCII restriction is not stylistic — LVGL's built-in Montserrat
covers `0x20-0x7F`, `°`, `•` and a fixed FontAwesome subset, so an accented
letter or an emoji renders as a blank box on the very screen the name exists to
label. It also has to survive slugification into an mDNS hostname later. Both
limits are enforced in `settings::set_name()` rather than at the form, because a
submitted value can't be trusted to have honoured its own `maxlength`.

A settings reset keeps the name. It is the post's identity, not a tunable — a
device that forgets which tank it watches because someone reset the alarm band
would be a surprise.

### The hardware bring-up probe

A separate diagnostic answers the questions the drivers depend on. It is off by
default:

```sh
idf.py menuconfig       # Thermo Post → Run the hardware bring-up probe at boot
idf.py flash monitor
```

(`idf.py -D CONFIG_…` does *not* work — that sets CMake cache variables, not
Kconfig options, and is silently ignored.)

It reports, in order:

1. Is this really an N16R8 (16 MB flash, 8 MB PSRAM)?
2. Who is on the I²C bus? (expect touch at `0x38`, ES8311 at `0x18`/`0x19`)
3. Is the MCP9808 alive, reading a plausible room temperature?
4. What does the LCD controller report as its ID?
5. Does GPIO45 actually control the backlight? *(watch the screen — it blinks 3×)*

It then self-tests `storage` and `settings`, and ends in a `GATE PASS` /
`GATE FAIL` summary. **On a failure it parks the boot** rather than letting the
drivers fail downstream of a wrong pin map.

Two caveats:

- **The controller-ID read is advisory.** Those reads need controller-specific
  dummy clocks, so garbage is inconclusive, not a failure — and on this board it
  *is* garbage. The definitive test is whether the display draws a correct image.
- **The probe is destructive.** It erases the log and resets settings to get a
  known starting state. That is why it is opt-in.

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
├── sdkconfig.defaults      PSRAM, flash, partition table, FreeRTOS tick, console, LVGL
├── main/                   boot sequencer, board_pins, opt-in bring-up probe
├── components/
│   ├── board_i2c/          the one shared I²C bus (touch + codec + sensor)
│   ├── display/            ILI9341 over SPI + LVGL + LEDC backlight
│   ├── sensor/             MCP9808 driver + 1 Hz sampling task
│   ├── touch/              FT6336 capacitive touch as an LVGL indev
│   ├── recorder/           the one log writer + the graph's RAM ring
│   ├── ui/                 theme + the main screen
│   ├── storage/            LittleFS append-log + rotation with stable absolute indices
│   ├── settings/           NVS-backed name / interval / band / units / sound
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

**Bring-up passed. Drivers next.**

| | |
|---|---|
| ✅ Built | Repo skeleton, bring-up probe, `storage`, `settings`, `alert` decision logic, host tests |
| ✅ Verified | The gate: N16R8 (16 MB flash / 8 MB PSRAM), touch `0x38`, ES8311 `0x18`, MCP9808 `0x1A` reading room temperature |
| ✅ Verified | `display` — ILI9341 + LVGL drawing correctly at 320×240, backlight under LEDC PWM |
| ✅ Verified | `board_i2c` + `sensor` — MCP9808 found by manufacturer ID, sampled at 1 Hz, shown on screen |
| ✅ Built | `touch` — FT6336 via the FT5x06 driver, registered as an LVGL pointer indev |
| ✅ Built | `recorder` + `ui` — one record per interval, graph backfilled from the log on boot, main screen with readout / chart / status strip |
| ⬜️ Not started | menu + editors, alerts, cloud + config service, power |

Build order: display → sensor → touch → main screen → menu → alerts → cloud →
power. Each step ends at something flashable and observable. (Sensor jumped
ahead of touch — with the part wired up, confirming it reads correctly was worth
more than an input device nothing yet listens to.)

`recorder` was not in the original plan as its own step. It landed with the main
screen because a chart needs a source: nothing was writing a `TempRecord` on the
settings interval, so the graph had no history to draw and none would have
survived a reboot. Making it one component with one job also gives M7's alarm
state machine and M9's RTC-memory mirror an obvious place to attach.

### Open questions

- **Power model.** The plan is always-on first (dim → backlight off → deep sleep
  on long idle, with a timer wake so readings continue), then *measure* before
  deciding whether deeper duty-cycling is worth the complexity. The numbers that
  decide it: average current with the screen on, dimmed, backlight off, and
  deep-sleeping between one-minute readings.
- **Backlight dimming.** GPIO45 drives the backlight and LEDC PWM lights it, but
  only 100% has been exercised so far. The wiki documents the pin as a plain
  high/low enable, so if the circuit gates a fixed-current driver the
  intermediate duty cycles may not actually dim — in which case
  `set_brightness()` collapses to on/off and the power model loses its "dim"
  tier. Settles the first time the idle policy steps it down.
- **The ES8311 tone.** Driving a codec over I²S to produce a simple alert beep is
  the least certain part of the build. It is kept behind a two-function API so a
  failure degrades to visual-only alerts rather than blocking anything else.
- **Default alarm band.** Currently 15–30 °C — deliberately generic, enough to
  make a fresh device sane and the alarm path exercisable. Expected to be the
  first thing a user changes.
