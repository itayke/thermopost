# Thermopost

A surface-temperature monitor built on the Hosyond 2.8" ESP32-S3 touchscreen
module. It samples an MCP9808 on a fixed cadence, shows the current reading with
a timeline graph of recent history, alerts on screen and through the onboard
speaker when the temperature leaves a configured min/max band, and mirrors every
reading to a Google Sheet over Wi-Fi.

By Itay Keren <itay@untame.com>. Copyright (c) 2026 Itay Keren.

Source-available under the [PolyForm Noncommercial License 1.0.0](LICENSE.md):
free to read, build, modify and share for personal, research, educational and other
noncommercial use. Commercial use needs a separate license, so get in touch.

**Using a post rather than building one?** Start with
[Getting Started](GETTING_STARTED.md), a short guide to the screen, the alarm, Wi-Fi
and the Sheet.

> **Working, and not finished.** Everything the device does day to day is built and
> most of it is confirmed on hardware: the screens, the settings, the alarm, the
> Sheet upload, the idle ladder and battery sense. Sleeping between records is built
> too — a quiet timer wake that records and goes back down — but not yet armed: it
> waits on a bench run and a measured sleep current. See [Status](#status).

## Hardware

**LCDwiki ES3C28P**, sold by Hosyond as "ESP32-S3 Touchscreen Module, 2.8"
240x320 IPS LCD". ESP32-S3 **N16R8** (16 MB flash, 8 MB OPI PSRAM).

| Block | Part | Interface |
|---|---|---|
| Display | ILI9341V, 240×320 (used rotated to 320×240 landscape) | SPI2 |
| Touch | FT6336G | I²C0 @ 0x38 |
| Audio | ES8311 codec → FM8002E amp → onboard speaker | I²S (see below) + I²C0 @ 0x18 |
| Charging | TP4054, battery sense on GPIO9 | ADC1_CH8 |
| Storage | microSD (unused in v1) | SDIO |

The pin map in [`main/board_pins.hpp`](main/board_pins.hpp) is transcribed from
[lcdwiki.com](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display) and confirmed on
hardware **except for the audio block, where the page is wrong in three places** —
the bring-up probe checked every pin it can reach over I²C, and the display driver
confirmed the panel by drawing, but neither can see an I²S wire.

Those three corrections, because they cost a long afternoon: the page's `DOUT`/`DIN`
are named from the **codec's** point of view, so playback goes out on **IO8**, not
IO6; **MCLK on IO4 does not work**, and the ES8311 derives its master clock from
BCLK instead; and the amplifier's enable on IO1 is **active low**, not high. Each
wrong combination fails identically and silently — the codec accepts its whole
register sequence over I²C and reports itself powered, unmuted and correctly
clocked, with no register anywhere that reveals the fault. Worth knowing before
trusting any other line of that table. The controller takes the ILI9341
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
idf.py -p /dev/cu.usbmodem<N> flash monitor   # the everyday loop; Ctrl+] to exit
```

That one command is the whole edit loop — `flash` compiles first, so a separate
`idf.py build` is redundant:

| target | compiles? | what it does |
|---|---|---|
| `set-target esp32s3` | — | writes `sdkconfig` for the chip. Once per checkout; it **overwrites an existing `sdkconfig`**, so don't repeat it casually |
| `build` | yes | compiles into `build/`; a near-no-op when nothing changed |
| `flash` | **yes** | compiles, then writes bootloader + partition table + app over USB |
| `monitor` | **no** | attaches to the serial port and prints what the device says |

**Why `flash` compiles is worth knowing, because it isn't `idf.py` doing it.**
`idf.py` has its own notion of ordering — that is what makes `idf.py build flash
monitor` run in that order rather than the order it feels like — but those are
*order-only* dependencies: they reorder commands you typed and never add one you
didn't. The build comes from the layer below. `idf.py flash` shells out to ninja's
`flash` target, and that target depends on `app`, `bootloader` and
`partition_table_bin`:

```
$ ninja -t query flash
flash:
  input: phony
    CMakeFiles/flash
    app
    bootloader
    esp-idf/partition_table/partition_table_bin
```

So the guarantee is real, and it comes from CMake rather than from the `idf.py`
front end.

`monitor` sits outside all of that: it runs no ninja target, just reads
`project_description.json` and the ELF and opens the port. Two consequences:

- It is **safe on its own** — `idf.py -p /dev/cu.usbmodem<N> monitor` watches a
  board that is already running, without reflashing it.
- It reads the ELF **from `build/`, not from the device**, which is what lets it turn
  a panic backtrace into function names. Build without flashing and those names come
  from firmware the board isn't running — the one case where a decoded backtrace can
  quietly lie to you.

`-p` matters only to the targets that talk to the board (`flash`, `monitor`,
`erase-flash`); `build` never touches the port. Omitted, `idf.py` guesses, and gets
it right with one board plugged in.

Occasionally useful:

```sh
idf.py fullclean            # after an sdkconfig or partition-table change acts strangely
idf.py menuconfig           # the config UI; see the bring-up probe section
./tests/host/run.sh         # the IDF-free logic, on the host — no board needed
```

Editor red squigglies on `esp_err_t` and friends before the first build are
expected — clangd has no context until the build writes
`build/compile_commands.json`. The xtensa-only flag warnings (`-mlongcalls`, …)
are cosmetic and never go away.

### Moving a post to the OTA layout (once, over USB)

The flash now holds two 3 MB app slots, so an update is written beside the running
image rather than over it, and the bootloader rolls back an update that never proves
itself. The partition table and the bootloader are only ever written over USB, so
every post needs this one cable visit before over-the-air updates can reach it.

Rollback is set in `sdkconfig.defaults`, which does not override an `sdkconfig` that
already exists. Check before flashing:

```sh
grep BOOTLOADER_APP_ROLLBACK_ENABLE sdkconfig   # must read =y
idf.py set-target esp32s3                       # if not: regenerates sdkconfig from the defaults
```

Then flash as usual. What survives depends on whether the flash is erased first:

| command | keeps | loses |
|---|---|---|
| `idf.py flash` | Wi-Fi, name, settings, the upload mark — `nvs` kept its offset | the on-device log, so the graph starts empty — anything not yet uploaded too, so check `cloud status` shows nothing pending first |
| `idf.py erase-flash flash` | nothing | everything, and the post comes back unprovisioned |

After a plain `flash`, the log resumes numbering **above the upload mark**
(`storage::raise_index_floor`, called by cloud sync) instead of restarting at zero.
That matters because the Sheet dedupes on `device` + `index`: reused indices would be
dropped as duplicates of rows it already holds, and counted as sent. After
`erase-flash` the upload mark is gone too, so delete that post's rows from the Sheet
first, or its new readings will be dropped the same way.

### Releases, and updating a fleet over the air

**The version** is `THERMOPOST_VERSION` in the top-level `CMakeLists.txt`. Only
`tools/release.sh` builds it as is; every other build reports it with `-dev` on the
end, and a dev build is never replaced from the Sheet, so a post on the bench is not
overwritten a minute after it was flashed. The Network page's last line shows the
version, and `ota` on the serial console adds which slot is running and whether it
has proved itself.

**Releasing.** Bump the version, commit, then:

```sh
tools/release.sh "what changed"
```

It refuses a dirty tree, an unpushed commit or a version already released, builds into
`build-release/`, publishes `thermopost-<version>.bin` as a GitHub release of this repo
tagged at the commit it was built from (`$THERMOPOST_RELEASES_REPO` names another), and
puts the row for the Sheet's `firmware` tab on the clipboard, ready to paste. The repo has to be public: a
post downloads the release without credentials.

**Rolling out** is two cells, both read on every upload, so nothing is redeployed:

1. Put the new version in one post's `fw_pin` cell in `posts`, and watch its
   `firmware` column change.
2. Set `fw_target` in `config`, and clear the pin. Every post moves on its next upload.

Rolling back is setting `fw_target` to the older version. See the Apps Script guide's
**5. Firmware updates** for the tabs.

**What a post does with an offer.** The upload reply names the release the Sheet wants
when the post runs something else. The post takes it only when it is a release build,
on external power or a well-charged battery, with no alarm open, and not already
proving a new image. It streams the image into the idle slot, checks the sha256 the
Sheet gave and the version the image itself declares, and reboots into it. Uploads
wait while it downloads.

**A new image has to prove itself** by making one successful upload. Until then it is
pending: a deadline (a margin plus one record interval) rolls it back to the image it
replaced, and so does any reboot, deep sleep included, so the post stays awake while
it proves itself. A rolled-back release is reported in the `fw_rejected` column and
not offered to that post again for a day of uptime. A download that fails is retried
after a pause, not on every upload. Two consequences worth knowing: **Turn Off** on a
post still proving itself brings it back on the old image, and a post doing quiet
wakes takes an update only when it next comes up properly.

**On the bench**, `ota <url> <sha256>` updates from any URL (power and alarm checks
still apply, version checks do not), `ota confirm` keeps a pending image by hand, and
`ota rollback` goes back to the other slot.

**Trust.** Integrity is HTTPS plus the sha256 in the Sheet, so whoever can edit the
Sheet chooses what the fleet runs. Signed images (secure boot) are the step that
removes that, and belong with leaving demo mode.

## Usage

Flashing brings the board up through `display → nvs → storage → settings → i2c →
sensor → touch → audio → recorder → ui → shell → cloud → alert → power` and draws
the main screen:

```
 (☰ Menu)      72.4°F            ↑ 86.0
 [███░]⚡14:32                    ↓ 59.0
 ┌────────────────────────────────────┐
 │- - - - - - - - - - - - - - - - - - │   max, dashed
 │       ╭─╮      ╭──╮                │
 │   ___╱   ╲____╱    ╲___╱╲___       │
 │- - - - - - - - - - - - - - - - - - │   min, dashed
 └────────────────────────────────────┘
  -2h            -1h             now
 North Tank
 #recs live 37  total 4213  uploaded --
```

The big number is the current reading in the configured unit (°F by default) and
it changes colour with the alarm state — white inside the band, warm above the
maximum, cold below the minimum, amber when the sensor isn't answering. The colour
says which way the temperature went before the digits are legible, which is what a
screen read across a room actually conveys.

That colour comes from the alarm state machine, not from comparing the reading to
the band on the spot. The difference is the recovery margin: a bare compare
re-decides on every sample, so a temperature parked on the limit flickers the
readout between two colours several times a minute while the alarm itself — which
has hysteresis — is not changing its mind at all.

Every screen asks for a *role* rather than a face — `kFontBody` for captions,
chips and the status strip, `kFontTitle` for headings and button labels,
`kFontReadout` for the big number — and the three are bound to LVGL's Montserrat
in one block at the top of `ui_theme.hpp`. Setting the UI in something else is
those three lines plus the line heights beneath them, which the layout needs as
literals because a font's metrics are only known at link time; `ui::init()` checks
them against the linked faces at boot and warns rather than letting a swap quietly
crowd the rows. `sdkconfig.defaults` carries the note on converting a TTF, whose
one real trap is that `LV_SYMBOL_*` are icon-font codepoints no text face has.

The unit rides after the digits at the small text size, sitting on their baseline.
That offset is computed from the two fonts' own metrics — a baseline is
`line_height - base_line` below a box's top — rather than dialled in by eye, so
changing either font keeps them aligned. The pair lives in a flex row and is centred
as a group, because centring the number alone and hanging the unit off its right
would shift the whole readout off-centre by half the unit's width.

**Tapping the reading swaps °C and °F**, thresholds included, and the choice is
saved. Nothing stored changes: every value on the device is hundredths of a degree
Celsius and `settings::display_value()` is the only thing that converts, so the
unit is a property of the screen and never of the log or the Sheet. The unit stays
on screen even with no reading, so an unplugged sensor doesn't make the toggle look
broken.

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

**A record is an aggregate, not a snapshot.** The sensor is read once a second but
a record is written once per interval, so storing whichever reading happened to be
current at the boundary would throw away the other 59 — and hide any excursion
between them, on a device whose whole job is noticing excursions. Each record
therefore carries the interval's **mean, minimum and maximum**, and the graph draws
all three: the mean as the bright trace, min and max as a dimmer envelope either
side. A one-second spike shows up as a spike in the envelope even though the mean
barely moves.

The alarm flags stored with a record are tested against the *extremes*, not the
mean, for the same reason — a minute that averaged its way back inside the band
while its own maximum says otherwise is exactly the case this is here to catch.
Both the high and low bits can be set on one record: a minute that swung all the
way through the band genuinely breached both.

Extremes rather than, say, the largest deviation from some midpoint: a deviation
depends on what the reference was when it was written, so the same physical spike
would encode differently after the band was edited, and the record would stop being
re-derivable from its own numbers. min and max are facts about the interval and
nothing else.

The clock sits under the menu glyph and reads `no clock` until SNTP lands —
showing a 1970 timestamp would be a bug report rather than a fact. The phrase is
short because at Montserrat 14 the accurate one, "clock not set", is wide enough
to reach under the centred readout; the menu screen has the room and says it in
full.

The status strip has two rows: the post's name, and three record counters that
answer three different questions.

| | |
|---|---|
| `live` | appended since boot — **frozen means logging has stopped** |
| `total` | ever written, across every boot and rotation |
| `uploaded` | how far the Sheet has caught up (`--` when no endpoint is set) |

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

The menu pill is espressopost's, scaled for a 320 px screen: a solid amber capsule
holding a hamburger glyph and the word *Menu*, both knocked out in black, in a
centred flex row. Every button on the device is built this way — a filled capsule
in the control's own colour, its label in black — and a press darkens the fill
rather than the label, so the two do not look like separate things reacting.

The glyph is painted as three 1 px bars rather than set as `LV_SYMBOL_BARS`, which
the built-in font does carry — espressopost's reasoning applies here too: a painted
glyph tints and scales with the layout instead of being pinned to whatever the font
designer chose.

**The same pill is in the top-left corner of every screen**, showing a left
chevron and *Back* everywhere but here. Same corner, same size, so the way out
never has to be looked for.

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

### When the band is left

Once a reading leaves the band, a panel opens over the top of the graph, just
under the number that caused it:

```
 (☰ Menu)      86.4°F            ↑ 86.0
 [███░]⚡14:32                    ↓ 59.0
 ┌────────────────────────────────────┐
 │  ┌──────────────────────────────┐  │
 │  │           TOO HOT            │  │
 │  │  (  Snooze 5m  ) (   Off   ) │  │
 │  └──────────────────────────────┘  │
 │- - - - - - - - - - - - - - - - - - │
 └────────────────────────────────────┘
```

It says **TOO HOT**, **TOO COLD** or **NO READING**, in the same colour the
readout has just turned. Deliberately not full-screen: a post that hides its own
temperature the moment the temperature starts mattering has the instinct exactly
backwards, so the panel covers the top of the graph and leaves the number, the
threshold chips and the recent trace where they were.

The speaker joins in — the same beeping tone the Sound page's test button plays,
in bursts rather than continuously. Continuous is louder without being more
informative, and a room with an unattended post in it still has to be a room
someone can work in; the burst is what makes ignoring the alarm a decision rather
than an impossibility. Muting alarms on the Sound page silences the tone and
nothing else. A muted post still shows the panel, because a mute is a statement
about noise and not about whether the tank matters.

**Two buttons, and the difference between them is how long they last.**

| | |
|---|---|
| **Snooze** | Quiet for `THERMOPOST_ALERT_SNOOZE_MINUTES` (5 by default), then say it again if the reading is still out of band. The one to press when you are already dealing with it. |
| **Off** | Quiet until the temperature changes what it is doing — recovers, or goes out the other side. The one to press when you have decided this excursion is fine. |

Both are scoped to the *episode*, not to the alarm. Any change of state clears
them, so a post that was told "Off" while too hot still shouts when it later goes
too cold, and a snooze bought for one excursion is not spent on the next one. The
tint stays either way: a silenced alarm is still an alarm, and the number goes on
saying so after the panel has been dismissed.

Snoozing lives in `alert_logic.cpp` rather than in the runtime around it, which is
why it is a deadline in the `Status` rather than a timer somewhere. That put the
awkward cases under host tests instead of under a stopwatch and a warm hand on the
sensor: a snooze that outlives its episode, a High→Low flip inside the snoozed
span, and an acknowledgement absorbing a running snooze all have cases in
`tests/host/test_alert_logic.cpp`.

The panel belongs to the main screen, so an alarm that fires while a settings page
is open is audible but not visible until Back. Left that way on purpose: yanking
the screen out from under someone mid-edit to show them a number they can already
hear is a worse trade than a few seconds of delay.

One predicate — `alert::should_alert()` — decides both outputs. The panel is up
exactly when it is true, and the speaker sounds exactly when it is true *and*
alarms are unmuted. Two predicates would be two things to keep in step, and the
failure mode is the one nobody would report: a panel offering a Snooze button for
a tone that already stopped.

### The hardware alert window

The MCP9808 does not only convert — it compares. `T_UPPER` and `T_LOWER` hold a band
in its own registers, and it drives an open-drain **ALERT** pin whenever a conversion
lands outside them, in silicon, at roughly 200 µA, **whether or not the SoC is
running**. `alert::init()`'s tick pushes the configured band down to it every second,
so the part is always judging the same limits the firmware is.

That is what lets a sleeping post keep watching. A post asleep between records is
blind between them; a post whose *sensor* is awake is not. Wired to
`board::kSensorAlert` (`IO21`, RTC-capable, on the expansion header) the line becomes
a wake source, and an excursion reaches the post within one 250 ms conversion —
**sooner than this firmware notices while fully awake**, since a running post only
looks once a second.

Three details that had to be right:

- **The window is a trigger, not a verdict.** It carries zero hardware hysteresis and
  the limits are rounded *inward*, so it can only ever sit inside the software band
  and trip early. `alert_logic` still makes the actual call, with the real hysteresis.
  A false wake costs a few hundred milliseconds; a missed one is the thing being
  prevented.
- **`T_CRIT` is a trap.** Its power-on value is 0 °C and CRIT asserts on `T_A >=
  T_CRIT`, so a freshly reset MCP9808 has its alert asserted at any room temperature.
  It is pushed to 255 °C, past anything the part can measure.
- **The window is rewritten after every re-probe.** The sensor is on a flying lead; a
  reseated connector comes back at power-on defaults, and a window nobody rewrote
  would be a post that thinks it is protected and is not.

**Proving the wire, and that it wakes the board.** With the line fitted, `power`
prints what the pin reads beside what the sensor says over I²C, and names the fault
when they disagree — `sensor asserts but the pin is high` is a missing or broken
wire. Agreement while the reading is in band proves nothing, so warm the sensor past
the band and look for `line works`. Then `power nap 60` deep-sleeps with three wake
sources — touch on `ext0`, the ALERT line on `ext1`, and the timer — and the next
boot logs which one it was (`woke: sensor ALERT`). The same command is how the
sleep current is metered, long before anything sleeps on its own.

Turn Off keeps touch as its only wake. Off means off, and a tank going out of band
does not switch a post back on that someone switched off. A nap armed while ALERT
is already low leaves `ext1` out: comparator mode holds the line down for as long as
the reading stays out of band, so arming it would wake the board immediately.

**It can be proved before the wire is soldered.** The part packs its own comparison
into bits 15..13 of every ambient read, and those track the limits whether or not the
ALERT output is enabled — so `sensor` logs `hardware alert asserted` / `cleared` on
the edge. Warm the sensor past the band with the console open and the window
demonstrates itself over I²C alone.

### Going quiet when nobody is looking

Idle time walks the screen down a ladder, and nothing below the first rung is
visible to the parts of the device that matter — the sensor keeps sampling, the
recorder keeps writing, the alarm keeps watching. A dark post is a working post.

| Rung | After | What changes |
|---|---|---|
| **Active** | — | Full brightness |
| **Dim** | `THERMOPOST_POWER_DIM_SECONDS` (60) | Backlight to `THERMOPOST_POWER_DIM_PERCENT` (20%) |
| **Dark** | `THERMOPOST_POWER_DARK_SECONDS` (180) | Panel blanked with `set_on(false)` — it stops scanning, but the framebuffer survives, so coming back is the screen that was already there rather than a repaint from nothing |
| **Asleep** | the power mode's sleep delay | Deep sleep between records, waking quietly to take each one. Built, not yet armed; see below |

The main screen carries the charge as a **drawn meter at the top left**, before
the clock, with a bolt between them when a charger is detected. Drawn rather than
taken from LVGL's battery glyphs, which come in five fixed steps — a bar that moves
continuously says "draining", where a five-state icon says nothing for an hour and
then jumps. It goes amber under a quarter, because a quarter of the voltage range
left on a lithium pack is much less than a quarter of the runtime.

**No number beside it**, on purpose. A percentage on a lithium pack is an estimate
off a published curve, and printing an estimate to the digit invites it to be read
as a measurement; a bar carries exactly the precision the estimate has. With no
pack fitted the meter is struck through with a slash rather than merely drawn
empty — a post running on USB alone is a normal way to run one, and an empty meter
would read as a flat battery instead of no battery. How the post knows a pack is
missing is below, under external power.

Idle time comes from LVGL's own inactivity counter, which the indev layer resets
on a real press — so the ladder counts *taps* rather than interrupt edges, and
this component never has to know anything about the touch controller.

**The tap that wakes the screen does nothing else.** Once the ladder has stepped
below Active, the next touch is consumed in `touch.cpp` and never handed to LVGL:
the user is looking at a screen they have not read yet, and on the main screen
almost every pixel is a button, so delivering it would mean waking the post into
whichever page the finger happened to land on. The decision is `power::consume_touch()`
and it is asked once, on the press edge, then *latched for the whole gesture* —
the ladder is back at Active within one 200 ms tick, so deciding per read would
swallow the press and deliver the release a moment later, which is exactly the
click this prevents. An alarm is unaffected: it pins the ladder at Active, so
Snooze and Off are live on the first tap.

**A live alarm outranks the ladder and lights the screen**, rather than merely
holding it where it was. The panel an alarm raises is useless on a dark display,
and a post that beeps in the dark makes someone hunt for a device instead of
reading it. Snooze and Off therefore pick up a second meaning for free: they are
also what lets the screen go back down.

#### Sleeping on idle: built, and gated until proved

Deep sleep itself is wired and works: **Turn Off** is a real one, with a real wake
source. The *automatic* fourth rung — the post putting itself to sleep — is built as
well, and held behind `kAutoSleepReady = false` in
[`power.cpp`](components/power/power.cpp) until it has run on the bench and the sleep
current has been metered. The gate is a constant rather than missing code so the
reason travels with it.

**A sleeping post wakes three ways, and only one of them lights the screen.**

| wake | what runs | then |
|---|---|---|
| **timer**, every record | storage, settings, the sensor, battery sense — no display, no LVGL, no touch, no codec | one record, the alarm judged against the episode it slept with, and back to sleep |
| **timer**, dark upload due | the same, plus Wi-Fi in station mode and the upload queue — no setup AP, no form, no mDNS | drains to the Sheet, then sleeps |
| **touch** or **ALERT** | the full boot | the UI, the ladder, and down again when idle |

The timer wake is `quiet_wake()` in [`app_main.cpp`](main/app_main.cpp). It sleeps
again for the interval less the time it was awake, so records keep their spacing,
and it **comes up properly** — continues into the full boot — when it finds
something a person has to see:

- **The alarm needs raising.** The reading is stepped through `alert_logic` against
  the mirrored episode, so an acknowledged excursion that is still out of band stays
  asleep, while a new one, a sensor fault or an expired snooze comes up and sounds.
  A flagged record that escalates is handed to the uploader as urgent.
- **The supply no longer sleeps.** In *USB on*, a pack that has risen to
  the charger's voltage means external power, and the post stays up. The USB-host
  signal is not trusted this early in a boot — it reads "connected" until a few ticks
  pass — so a post that has met a host keeps the verdict it slept with, and a touch is
  how it notices a computer.

**A quiet upload that fails backs off.** If the link or the Sheet does not answer
within `kQuietUploadTimeoutMs`, the next try is a dark cadence away rather than on the next wake — an AP
that is down overnight would otherwise bring the radio up every minute until morning.

**Without the ALERT wire, a sleeping post sees the band only at each record.** An
excursion that starts and ends between two wakes produces no alarm and no row. That
trade is accepted for now, because fitting the wire means changing the housing. The
firmware does not need it: an unwired `IO21` sits high on its pull-up, so `ext1`
simply never fires, and fitting the wire later starts it working with no change here.
`kAutoSleepReady` is about the bench, not the wire.

**What a wake keeps.** Deep sleep loses RAM, and sleeping to save power has to come
back as the same device it went down as. Each component that owns such state keeps
its own `RTC_DATA_ATTR` mirror and registers a hook with
[`sleepstate`](components/sleepstate/include/sleepstate.hpp), which fills them all
just before a sleep and *seals* them; the next boot trusts a mirror only behind that
seal. Turn Off never seals — Off is meant to forget, and coming back is honestly a
fresh boot — which is exactly why it could ship first.

| kept | owner | why it matters |
|---|---|---|
| alarm `state`, `acked`, snooze deadline | `alert` | an acknowledged alarm that forgets re-alerts on every wake, and `step()` treats a changed state as a new episode |
| next upload deadline | `cloud` | lost, every wake finds the upload due and brings the radio up |
| `written()` | `recorder` | a post that boots every minute would show 0 or 1 forever on the "is it still recording?" counter |
| the supply verdict, whether a USB host ever answered, and whether a pack is fitted | `power` | lost, a woken post judges its supply by pack voltage alone, and one with no battery sense assumes external and never sleeps |

A quiet wake that never starts a component leaves its mirror as it was, which is
still right — nothing changed it. The two it does change without a full init, the
alarm and the counter, write their mirrors straight back.

**Deadlines are the trap.** Everything runs on `esp_timer`, which restarts at zero on
every deep-sleep wake, so a deadline mirrored as-is is meaningless — a snooze would
expire instantly and the upload cadence would collapse to "every wake". They travel
as absolute *wall-clock* times, which IDF keeps running through deep sleep, and are
converted back at the boundary. Absolute rather than remaining, so a deadline
survives any number of later wakes that never touched it. `alert_logic` never sees
the difference, which is what kept the host tests untouched.

The graph's ring needs no mirror: `recorder::init()` rebuilds it from the log, as on
a cold boot. Nor does the recorder's in-flight interval: one quiet wake is one record,
so the sleep *is* the interval.

**Rehearsing it.** `power nap 60` seals and sleeps like the automatic rung, and its
timer brings the post back quietly — from there it keeps sleeping between records
until touched, exactly as an armed post would. Watch it with the console: each wake
logs `woke: timer`, the record, and `sleeping N s`. Acknowledge an alarm first and it
stays acknowledged across wakes; let a snooze run out while asleep and the post comes
up sounding. On USB in *USB on* mode a nap records once and comes up, because
that mode does not sleep on external power — use *Always sleep* to watch the loop on
the bench.

#### The divider, and why USB presence is a guess

The ratio is **2** — two equal resistors — inferred rather than metered, and the
inference is sound rather than convenient: a working 1S pack read 2012 mV at the
pin, and only a ratio of 2 puts that anywhere lithium can be (4.02 V). The next
divider anyone fits would claim 3.02 V, a nearly flat cell that would not have
been running the board, or 6.04 V, which this chemistry does not reach. One meter
reading across the pack terminals confirms it, and it is one line in
`power.cpp` if it disagrees.

**External power is a heuristic, not a fact.** The TP4054 holds a charging pack at
its 4.2 V constant-voltage point and leaves it near there after termination, while
a pack under load falls away within minutes — so a threshold separates the two
most of the time.

So the pack is the *fallback*. The primary signal is IDF's USB monitor, which
watches for start-of-frame packets on a tick hook and therefore knows within
milliseconds whether a USB **host** is on the cable — exact and instant, where the
pack voltage is neither. Its limit is in its own documentation: a wall charger or a
power bank sends no SOF packets and "will never be considered as connected". So it
is authoritative when true and silent when false, which is why the voltage is still
consulted underneath rather than replaced.

**Once a host has ever answered, the host signal is the only one consulted** — in
both directions, and that stickiness is what makes unplugging register at all.

It took three wrong thresholds to see why. The first chattered: a pack resting near
a single line crossed it back and forth on ADC noise and on the load steps the
backlight and radio make, so the bolt blinked. The second latched: a hysteresis band
of 4.05–4.15 sat *on top of* the discharge curve rather than between two states, and
a pack an hour off the charger rests around 4.10, so "external" could never clear.
The third was subtler and fatal — **a just-unplugged pack sits at about 4.18 V and a
charging one at 4.20.** Twenty millivolts, under the noise floor. The pack simply
cannot answer whether a charger is attached, and no threshold ever will.

So the voltage path stops being consulted the moment something better exists, and
survives only for posts that meet nothing but dumb chargers. Its readings are still
averaged over sixteen conversions, because the S3's SAR wanders by more than a
millivolt at 12 dB and the divider doubles whatever it wanders by.

Known limit: a post moved from a computer to a wall charger inside one boot reads as
unplugged, because the sticky flag outlives the cable. A reboot clears it. `power`
prints `usb host` and `external` separately, and the transition is logged, so which
input is driving the answer is always visible.

The obvious alternative does not work, and its own documentation says so.
`usb_serial_jtag_is_connected()` detects a USB *host* — "connected to a power bank
will never be considered as connected", for lack of SOF packets. A post on a wall
charger would read as unplugged and put itself to sleep on mains, which is the
opposite of what "On when connected" promises. A real answer needs VBUS divided
onto one of the expansion header's spare pins.

**A missing pack is told by its restlessness, not its level.** With no pack, the
TP4054 charges its own output capacitor, stops, and lets it sag, so GPIO9 reads like
a full cell, around 4.1 V. A fitted cell holds still whether charging or full. So the
post keeps the range of its averaged samples over a few seconds and calls the pack
missing only when the reading is both high and swinging, for long enough that a burst
of radio load on a real pack cannot pass for it; the slash follows a pulled pack by
that hold, not at once. Single reads were tried first and
are useless here: their ADC noise is as wide as the swing. A missing pack also means
external power by definition, whatever the bare charger's level says to the
threshold. The meter takes its slash, the Power page reads "no pack", and uploads
send `batt_mv` and `batt_pct` as null. So does an upload in the first seconds after
boot, before the verdict has had its window and hold: the meter can show a pack
optimistically for that long, but the Sheet should not record one that may not exist. Not yet measured against a *full* pack, the
case that matters; `power` prints the range and the verdict, so it is one line to check.

`power` on the serial console is still the instrument for checking any of it:

```
post> power                                  # tier, idle, raw counts, pin mV
post> power wake                             # light the screen from the console
```

The dim rung is also the outstanding backlight question answering itself: if
GPIO45 turns out to gate a fixed-current driver rather than accept PWM, the Dim
tier will visibly do nothing and collapse into on/off.

### The Power page

Three modes and one number, because the interesting question is not how long to
wait but who decides whether waiting is allowed at all.

| Mode | What it does |
|---|---|
| **Always on** | Never sleeps. On battery it runs until flat — for a post that is watched more than it is left. |
| **On when connected** | The default: full behaviour on external power, the ladder and sleep once the cable is out. |
| **Always sleep** | Sleeps on any supply. The longest-running mode, and the one that makes a mains-powered post behave like a battery one. |

The mode is a cycling capsule whose label *is* the value, the same idiom as the
unit and the mute. Beside the sleep-delay stepper sits the one cell on any page
that cannot be tapped: **the estimated percentage and the voltage it came from,
one above the other**. Both, deliberately — the percentage is read off a published
lithium curve and the volts are the measurement, and showing only the friendly
number would hide which of the two can be trusted.

**Turn Off** shares the top row with the mode. It takes two taps — the label
becomes "Sure?" for four seconds — because one stray tap would otherwise blank the
device until someone came back to it. It deep-sleeps with exactly one wake source,
the touch controller's interrupt on `IO17`, so **a touch on the screen is the way
back and nothing else is**.

**The touch has to be held.** Anything brushing the glass of a post in a bag would
otherwise turn it on. A wake from Off brings up only the I²C bus and reads the
controller's touch count directly, and goes straight back to sleep unless the finger
is still down a second later, before the display has lit. If the controller can't be
read, the post turns on anyway rather than refusing to.

It started out waking on USB instead, and that was wrong in both directions:
switching off with a part-charged pack plugged in turned the post back on partway
through the charge. The mistake was testing a *level* where the question is an
*edge* — a pack being charged is a moving target, and the pack simply cannot answer
"has a charger just appeared". Only a wire from VBUS to a spare expansion pin
could, and this board has none. Touch is better on every axis anyway: off means off
on USB and battery alike, and with no polling there is no wake current to pay.

It is as off as software gets here, which is not all the way — without cutting the
rail there is residual draw, and how much is the same unmeasured number the rest of
the power model waits on.

That it can exist before the automatic fourth rung is the point: **switching off
needs none of the RTC-memory mirroring that sleeping does.** Sleeping to save power
has to come back as the same device it was, with its alarm acknowledgement and its
interval intact. Being switched off does not — that state is *meant* to be gone,
and coming back is honestly a fresh boot.

**The sleep delay is the whole policy, not a third of it.** Dim and dark are
scaled to fall at a third and two thirds of it rather than keeping their own
Kconfig delays, so a 60-second sleep dims at 20 and blanks at 40. Three
independent numbers that all had to agree would have been three ways to configure
a ladder that skips its own rungs.

Two of the three modes are mostly promises until the post can put itself to sleep,
and the page's footer says so outright rather than letting them look finished.

### The menu and the settings screens

Tapping *Menu* cross-fades to the hub. Its heading is **the post's name**, not the
word "MENU" — in a rack of identical boxes, the useful thing to say at the top of a
settings screen is which box this is, and the chevron in the corner already says
what the screen is for. Under it sit five pages and a line carrying the full date,
the time and the total record count. The grid is two columns wide; if an odd
number of pages ever lands there, the last one centres on a row of its own
rather than sitting flush left with a hole beside it.

```
 (‹ Back)          North Tank
        Rename from Network setup page
   (      Power     ) (   Temperature  )
   (    Recording   ) (      Sound     )
   (     Network    )
          Tue 28 Jul   14:32   4213 records
```

| page | controls | footer says |
|---|---|---|
| Temperature | alarm below, alarm above, recovery margin, **unit** | the exact stored hundredths, or which bounds the Sheet sets |
| Recording | interval, points plotted, **upload every**, **when dark** | the graph's span, and how stale the Sheet may get |
| Sound | alarm sound on/off, volume, **test alarm** | whether alarms will be audible |
| Power | mode, sleep delay, **battery volts** | what this mode actually does, or that sleep is not armed |
| Network | Configure, Forget; link, address and sync status above | where the name and Sheet URL are edited |

Every value is edited with espressopost's stepper — a caption, and `(-) value (+)`
with two amber discs either side — the same capsule as every other button, at the
aspect ratio that makes it a circle. It earns its place on a 2.8" panel because it is
unmissable, needs no drag precision, and has no hit target smaller than a fingertip.
Five things changed in the port, each forced by the data: the value is an `int16_t`
(the range is −40…125 °C, signed and wider than a byte in °F), the suffix is a
`const char*` (`"°C"` is three bytes, not a char), the step is explicit, the
`"--"`-until-touched gate is gone (a threshold always has a value, so the gate could
only add a state that cannot happen), and the bounds are re-derived rather than fixed.

**The bounds are the interesting part.** The two thresholds are coupled: `(+)` on the
minimum simply stops below the maximum, rather than letting the pair cross and having
`alert::sanitize()` quietly rewrite the input afterwards. A control that accepts a tap
and *then* changes the number is worse than one that doesn't move. The recovery margin
is coupled to both, because `sanitize()` caps it at half the band — so narrowing the
band lowers that stepper's ceiling, and the new value is written rather than left on
screen as a number the alarm logic has stopped using.

**Thresholds step in tenths of a degree, and that is a consequence of the form's
scope, not a preference.** The web form carries the network and the post's identity
and nothing else, so on the post the menu is the *only* way to set a threshold — and a
resolution the menu cannot reach is a resolution the product does not have. Whole
degrees would have made 21.5 °C unreachable on the post. (The Sheet can also set the
band, in any resolution; see **Controlling a fleet**.)

Two things follow:

- The **stored value stays hundredths of a °C**. Each tap converts the displayed
  tenth back, which keeps the standing promise that °F exists only at the point of
  display. A band set in °F therefore isn't a round number of °C — correct behaviour,
  not drift, and the footer prints the stored Celsius to two decimals so it is
  visible rather than mysterious.
- **Holding a disc repeats**, at LVGL's 100 ms cadence, and shifts to ten times the
  step after two seconds of holding. Without it, 0.1 per tap is 1650 taps across the
  sensor's range — a resolution that exists on paper and not in the hand. Repeats are
  opt-in per stepper: the interval is a ten-entry table and the volume is twenty taps
  end to end, and a repeat on either would overshoot faster than a finger can lift.
- **NVS is written 400 ms after the last tap**, not on each one. A held disc steps ten
  times a second, and persisting every step would be ten flash writes a second for as
  long as a finger is down. While the page is open the steppers are the working copy;
  leaving it flushes, so an edit inside the window is never dropped.

The unit label sits on its own line under the number rather than after it, for a
reason worth recording: the discs leave 66 px of clear space between them, `"21.5"`
fits that at the title font and `"21.5°C"` does not — and moving the discs apart
would push the control past the width the two-column layout asserts it can hold.

**The °C/°F toggle lives on this page, beside the band it governs.** It used to be a
Display page of its own, where a tap changed a suffix and nothing visibly followed;
here the same tap re-renders all three thresholds, which is what the setting actually
does. It also puts the choice next to the values at the moment someone is deciding
what to type into them.

That merge has one ordering constraint, and it is the kind that would have been a
silent bug rather than a visible one. The steppers hold tenths in whatever unit was
on screen when they were painted, and the debounced write reads them back by asking
settings what the current unit is — so a pending write has to be flushed *before* the
unit changes. Set the unit first and 21.5 °C is re-read as 21.5 °F, storing a band
eleven degrees from the one on screen, inside a 400 ms window nobody can see. The
toggle flushes, then switches, then reloads from the stored hundredths.

Switching also spends the footer for a couple of seconds on "tapping the reading does
this too" before falling back to the stored values. That shortcut was the old Display
page's footer, and the moment someone has just used the slow path is the one moment
it is worth saying.

The interval stepper walks a table — 10 s, 15 s, 30 s, 1 m, 2 m, 5 m, 10 m, 15 m,
30 m, 1 h — instead of counting seconds. Ten taps covers the whole range; any fixed
step over 10…3600 s is either hundreds of taps or too coarse to reach the fast end.

The Sound page also carries a **Test alarm** button: two seconds of the real tone at
the real volume, so the speaker can be judged without waiting for a threshold to be
crossed. It is the same code path the alarm itself uses, which is the point — a
test that exercises a second implementation proves nothing about the first. A second tap
stops it rather than queueing another two seconds. Its length is
`THERMOPOST_AUDIO_TEST_SECONDS`, raisable to a minute, which is what makes the pins
meterable while a tone is held.

It also earned its keep before it ever worked: it was the only way to make the
board's mis-documented audio wiring observable, since the fault mode is silence and
the codec reports itself healthy throughout. `THERMOPOST_AUDIO_DEBUG` still logs the
seven registers that separate "the codec is not playing" from "the codec was never
configured", which is the distinction that eventually cornered it.

It is greyed and unclickable in exactly two cases, and the footer says which: when
the master mute is off, and when no codec answered at boot. The mute case is
deliberate — the button exists to demonstrate what the alarm will do, and while
alarms are muted the truthful demonstration is silence. A button that beeped anyway
would be testing something the device never actually does.

Screens cross-fade rather than cut, using espressopost's two-phase engine: fade the
outgoing widgets out, swap in one frame at the midpoint, fade the incoming ones in.
Two details carried over because both were learned the hard way there — each widget
gets its own opacity animation instead of one on the container (fading a container
redraws the empty space between the widgets every frame), and a no-op animation owns
the phase clock so a phase ends after exactly one fade duration however many widgets
took part. A transparent click-eater covers the screen for the swap's duration, so a
stray tap can't land on a page that is already half gone.

Each page re-reads its values at the midpoint, while it is still hidden. Nothing else
writes a setting today — the web form carries the network and the post's identity
only — but the moment config arrives on the upload response, a page left open would
otherwise show, and then clobber, a stale number. Every writer goes through the same
`settings::set_*` setters, so NVS stays the single source of truth.

### The post's name

Thermoposts are meant to run in a group, one per tank, so each carries a name
that is sent with every reading — a row in a shared Sheet is meaningless without
knowing which tank produced it. Until one is set the post calls itself
`Post-XXXX` after the low bytes of its MAC, which is unique across a group with
no user action and answers "which of these five identical boxes am I configuring".

The name is the one setting the device menu will *not* change. It is the only
free-text field, and an on-screen keyboard on a 2.8" panel is the worst possible
way to enter one, so it belongs to the web config form — which is being filled
in from a phone or laptop with a real keyboard anyway. Everything else is a
number or a toggle and is editable from both.

24 characters, printable ASCII. The length is the status strip's share of a
320 px line; the ASCII restriction is not stylistic — LVGL's built-in Montserrat
covers `0x20-0x7F`, `°`, `•` and a fixed FontAwesome subset, so an accented
letter or an emoji renders as a blank box on the very screen the name exists to
label. It also has to survive slugification into an mDNS hostname — "North
Fermenter #3" becomes `north-fermenter-3.local`. Both
limits are enforced in `settings::set_name()` rather than at the form, because a
submitted value can't be trusted to have honoured its own `maxlength`.

A settings reset keeps the name. It is the post's identity, not a tunable — a
device that forgets which tank it watches because someone reset the alarm band
would be a surprise.

### Network and the clock

The post has no battery-backed RTC, so the radio and the wall clock come up
together: SNTP over Wi-Fi is the *only* source of time on this board. That is why
they are one component. Until the first sync every record carries `epoch_s = 0`
and the screens read "clock not set"; the moment a link comes up the clock line
starts ticking and records begin carrying real epochs.

Credentials normally arrive through the captive portal below. The serial console
is the same path, not a lesser one — both call `cloud::set_credentials`:

```
post> cloud wifi "North Tank AP" hunter2   # store and connect
post> cloud status                          # link, address, host, RSSI, clock
post> cloud portal                          # reopen the setup AP
post> cloud forget                          # erase and go idle
```

The credentials are kept by the Wi-Fi driver in NVS, not by this component, so
there is one copy and it is the one the radio actually uses. They survive a
reflash of the app, which is worth knowing when a post seems to join a network
nobody configured it for.

**Reconnection never gives up, it only slows down** — 2 s, then doubling to a
five-minute ceiling, forever. espressopost stopped after six tries, which was
fine for a device on a counter with its owner beside it; a post sits on a tank
and the AP it wants may be rebooting, or out of range while someone moves a
pallet. Nothing is lost by waiting: records keep being written locally, and both
the clock and the upload backlog recover on their own when the link returns. The
status word turns from `connecting` to `offline` after three failed attempts,
because past that "connecting" stops being the honest word — but it is not a
terminal state and nothing has stopped trying.

### The config form, and why it outlives provisioning

An unprovisioned post comes up as an **open access point named after itself** —
`Post-A1B2` until someone renames it — with a DNS responder that answers every
lookup with its own address. A phone joins it, the OS captive check fails against
the wildcard handler, and the setup form pops without anyone typing an address.
That much is espressopost's portal, ported.

**Joining that AP is a scan, not a spelling exercise.** Network → Configure opens
the setup network and shows a `WIFI:T:nopass;S:<name>;;` QR code, which both iOS
and Android cameras read as "join this network" — and since the AP is open there is
no password to convey, so nothing on screen is worth photographing. The network's
name is printed under the code as well, so a camera that will not focus still
leaves someone able to pick it out of a Wi-Fi list by hand.

That page owns the AP's lifetime from the user's side. Back closes the setup
network instead of leaving an open AP up behind the page that opened it, and the
page leaves by itself the moment the post has a link — the `GOT_IP` handler tears
the AP down, so **the AP disappearing *is* the signal that provisioning worked**,
and nothing else has to be watched for.

**What is different here is the lifetime.** espressopost's HTTP server existed only
while the setup AP was up: the first `GOT_IP` tore down the AP, the DNS responder
and the server together. Here the same form is also where a deployed post is renamed
and repointed at a different Sheet, and neither should cost a credential round trip —
so the two halves are split:

| | lifetime |
|---|---|
| the HTTP server and the form | the device's whole life, on every interface |
| the SoftAP and the DNS hijack | until credentials land, then gone |

One server serves both, because it binds `INADDR_ANY` — the AP is a *way to reach*
the form, not a second copy of it. After provisioning the post is reachable at
`http://<name>.local/` over the LAN, and the Network screen shows the raw IP
beside the `.local` name because some networks drop multicast and a post that could
only offer a name it can't resolve has no address to give at all.

**The form carries only what a touchscreen can't do**, and the rule is worth stating
because it is what keeps the two surfaces from overlapping:

> The form owns what you can't type on a panel, and what identifies the post. The
> menu owns what you tune.

So: the post's name, the network, the password, the `/exec` URL and the token — and
nothing else. Thresholds, the interval, the graph window, the unit, sound and volume
are numbers and toggles, which a 2.8" touchscreen does well, and they live under
MENU. Every field is **pre-filled from the current value**, so the form doubles as a
way to read a post's identity; the token is the one exception and is never echoed
back.

That boundary is why the settings fields are gone rather than never written — they
were there, and they were the wrong answer to the right problem. See the fleet
section below.

One behaviour worth knowing: **re-submitting the network you are already on does
nothing.** The field comes back prefilled and re-applying credentials drops the link,
so fixing a typo in the name would otherwise cost a reconnect every time.

The server outliving the AP now buys exactly two things: renaming a deployed post,
and repointing it at a different Sheet, neither of which should cost a credential
round trip. That is a smaller justification than it had when the form wrote settings,
and it is the right size for what is left — an always-listening HTTP server on plain
HTTP with no auth, whose blast radius is now a post's name and its endpoint. It
should not grow anything reachable from outside the LAN without revisiting that.

### Controlling a fleet

**A per-device web page is not a fleet tool, and that is what the settings fields on
the form were trying to be.** Five tanks meant five tabs, five round trips, and five
chances to type a different number — which is precisely the case that wanted "the
same band everywhere".

The shape that fits is **config on the upload reply**. Every post already POSTs a
batch on its own cadence; that channel is already outbound, already survives NAT,
already retries, and already knows which device is speaking. The reply carries what
the Sheet wants, and a post applies it through the same `settings::set_*` setters
everything else uses. Two things travel this way today: the release a post should run
(see **Releases, and updating a fleet over the air**) and the alarm band.

**The band has two levels, like the firmware target.** `band_min_c` and `band_max_c`
in the `config` tab set it for every post; the same two columns in a post's `posts`
row override it for that post, which is the case of one or two tanks on a different
material. Each bound is resolved on its own, and a bound blank in both places is left
to the post. The full table is in
[`tools/cloud_apps_script.md`](tools/cloud_apps_script.md#6-the-alarm-band).

Settled along the way:

- **The Sheet wins while a bound is set.** A bound from the Sheet is locked on the
  Temperature page: its discs go muted and stop responding, and the footer says the
  Sheet set it. Clearing the cell hands it back, at the value it last had. A generation
  counter would have let both sides edit, but a band that changes depending on who
  touched it last is not one a fleet manager can reason about.
- **A pushed config is untrusted input**, exactly like a form post. The band goes
  through the setters, which clamp it to the sensor's range; one that is inverted or
  narrower than a degree is refused whole and logged, and the post keeps its old band.
- **The Sheet can see the result.** Each upload reports the band the post alarms on,
  shown in the `posts` tab's `band` column and in alert emails, including the posts
  that set their own.
- **Two things can never arrive this way.** Wi-Fi credentials, because they *are* the
  channel; and the endpoint URL, since a wrong one cannot deliver its own correction.
  Those stay on the form permanently, which is a better argument for the form's
  existence than "you can also set thresholds there".

### Mirroring to a Sheet

Every record is mirrored into a Google Sheet through an Apps Script Web App —
no server, no billing, and the full setup is in
[`tools/cloud_apps_script.md`](tools/cloud_apps_script.md). Point a post at it
with `cloud url https://…/exec`; the token has a default, so that one line is
usually the whole configuration.

Upload is a **durable queue, not a fire-and-forget send**. A high-water mark in
NVS records the absolute index of the next record to upload, and a background
task drains everything above it whenever the link is up. Nothing is lost by
being offline: records go to flash first and the backlog is sent whenever the
network returns, which is why the reconnect policy can afford to be patient. The
mark only advances on a confirmed `"ok":true`, so a response lost in transit
costs a repeated batch rather than a hole in the history.

**How often the radio comes up is a setting, because it is the biggest power
decision the post makes.** Association, DHCP and a TLS handshake cost far more than
taking a reading, and the expense is per *session*, not per POST — the drain loop
keeps sending batches until the queue is empty once the link is up. So an hourly
cadence costs roughly a twelfth of the radio time of a five-minute one, and
`kBatchSize` is already one hour of history per request.

The Recording page carries two figures, `Upload every` and `When dark`, and **the
split is on the idle tier first.** What makes a radio session expensive is the post
being otherwise idle: a lit post is burning tens of milliamps regardless and one more
handshake is marginal, while a dark one is doing almost nothing else, so the handshake
is the entire budget.

**`When dark` applies only on battery.** A dark post on external power keeps the
`Upload every` figure, because it has no battery to save. `on_external_power()` is a
heuristic that cannot tell a just-unplugged pack from a charging one for minutes, but
misjudging it costs only a few minutes of the other cadence. A post waking from sleep
batches whatever its supply, since sleeping was its power mode's choice. The `gap_s`
each batch reports stays the longer of the two figures, so unplugging a post does not
set off a false silence alert before its first batch on battery.

`Upload every` defaults to *every record* — a post someone is standing in front of
should have a Sheet that matches its screen. `When dark` defaults to **15 minutes**.
Zero would have been the change-nothing default, but it is the wrong one to ship: the
dark cadence exists precisely for the deployment nobody is going to tune, and a fleet
left at its defaults would spend its whole radio budget on a Sheet nobody is watching.
Fifteen minutes is short enough that a post still looks alive to someone checking on
it, and long enough to cut the sessions by more than an order of magnitude at the
default record interval.

So the upgrade is not behaviour-neutral, deliberately: an existing post will start
batching once its screen goes out. Anything that actually needs to be seen sooner is
flagged, and flagged records ignore the cadence entirely.

Three details make it behave:

- **They are minutes, not a record count.** A count would mean something different on
  every post in a fleet, because the span is count × interval — "every 60 records" is
  an hour at the default cadence and two and a half days at the slowest one.
- **A flagged record ignores both.** A record that breached the band, or that the
  sensor failed to produce, is uploaded the moment it is written. A saving that also
  delayed the alarm would be the one saving not worth making.
- **Waking drains the backlog.** A pending deadline is pulled in whenever the
  applicable cadence gets shorter — which is what happens when a dark post lights up —
  so walking over to a post gives you a current Sheet rather than the schedule it went
  dark under.

The deadline advances from the previous deadline rather than from when a drain
finished, so the seconds an association and a handshake take don't accumulate until a
whole cadence has been swallowed. And nothing here runs on a clock: the cadence is a
floor checked when a record lands, which is the shape a post that sleeps between
records will need, because a sleeping post has no clock to run.

**Every batch carries the post's pack voltage, and that is a fleet feature.** The
envelope sends `batt_mv`, `batt_pct` and `ext` alongside the device id and name, so
the Sheet can answer "which posts need a visit?" without anyone walking a tank room.
The Apps Script keeps a `posts` tab — one row per post, rewritten every batch — which
is what its alerts read; scanning a hundred thousand `readings` rows for each post's
latest voltage is the query you do not want to write.

**`readings` holds the last month, and older rows move to an archive.** Each night the
Apps Script moves readings older than `archive_after_days` into one spreadsheet per
month in the same Drive folder. A fleet recording every minute would otherwise fill a
spreadsheet's ten million cells in weeks, and every upload's duplicate check reads the
whole tab. See **7. The archive** in the guide.

**The Sheet emails the fleet manager.** Out of band or a sensor fault, a battery low
off the charger, and a post gone silent — one email when each opens, one when it
clears, a reminder while it stays open, with thresholds and recipients in a `config`
tab. Setup and the rules are in [`tools/cloud_apps_script.md`](tools/cloud_apps_script.md).
Silence is the one a post cannot report itself, so every batch also carries `gap_s`,
the longest the post means to go between uploads; the Sheet calls a post silent
against its own cadence rather than a fixed threshold that an hourly post would trip
all day.

Three things about it are deliberate:

- **It is in the envelope, not in the record.** `TempRecord` is nailed to 16 bytes
  because record N lives at offset N × 16, so growing it would invalidate every
  absolute index and every high-water mark in the fleet. But it would have been the
  wrong place regardless: a record is a fact about one interval, and the pack is a
  fact about the post *now*. One value per POST beats sixty near-identical copies,
  and a backlog drained after a night offline then carries this morning's voltage,
  which is the useful answer.
- **Millivolts and percent both, and `null` for neither.** The volts are the
  measurement; the percent comes off a published lithium curve and is good to about
  ten points, which is what a dashboard actually thresholds on. A post with no
  divider or no answer from the ADC sends `null`, not `-1` and not `0` — a flat pack
  reports `0`, and the two must not look alike to a rule that pages someone.
- **`ext` is what keeps the alert quiet.** A post at 3.6 V on a charger is
  recovering; the same post discharging needs a visit. Voltage alone cannot tell
  them apart, so the useful rule is `batt_pct < 20 AND ext = FALSE`.

**The dedupe key is `device` + `index`, and that is a fleet decision.** Each post
numbers its records from zero, so an index alone collides across a group on the
first day. The post's *name* can't be the key either — it is renameable from the
config form by design, and a key that changes when someone fixes a typo is not a
key. So the key is the board's MAC, which never changes, and the name rides along
as a display column the Sheet can be filtered by. `cloud status` prints the
device id, which is how a row gets matched back to a box on a wall.

One consequence worth stating: a rename retags the queued backlog, because
records are labelled at upload rather than at write. That is the behaviour you
want — renaming a post should relabel that post, not leave a seam in the middle
of its history.

**Rotation can outrun the uploader**, and the code says so out loud. A post that
is offline long enough for the log to pass its 100k-record cap will have its
oldest records dropped before they were ever sent. The read reports where it
actually started rather than where it was asked to, so the gap becomes a gap —
logged once, permanent — instead of stalling every later record behind a record
that no longer exists.

The JSON is built by `cloud_json.cpp`, which has no IDF includes and is
host-tested against exact strings. Temperatures are formatted from the stored
hundredths with integer arithmetic, never through a float, so `20.90` is
`20.90`; an interval with no valid reading sends `null` rather than a sentinel
that a chart would happily plot as -327.68 °C.

### The hardware bring-up probe

A separate diagnostic answers the questions the drivers depend on. It is off by
default:

```sh
idf.py menuconfig       # Thermopost → Run the hardware bring-up probe at boot
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

### Screenshots

The screens in this README are captures, not photographs. `shot` on the serial
console prints the active screen and `tools/screenshot.py` turns it into a PNG:

```sh
. $HOME/esp/esp-idf/export.sh     # pyserial lives in IDF's environment
python tools/screenshot.py -o captures/main-screen.png
```

`captures/` is git-ignored and the directory is created if it is missing, so
capturing is scratch work by default; a shot meant for the README is committed by
path rather than by where it landed. Close `idf.py monitor` first — the port takes
one reader at a time, and a monitor still holding it looks exactly like a board
that will not answer.

**Navigate to the screen you want, then run the command.** Connecting does not
reset the post, so it captures whatever it is showing. That is not free: on the
S3's native USB-Serial-JTAG, DTR and RTS *are* the reset and boot lines, so the
script leaves them exactly as pyserial opened them — driving them, even to
deassert, reboots the board, and a rebooting board can only ever be photographed
on the screen it boots into.

It is a **re-render, not a framebuffer read**, because there is no framebuffer to
read: the draw buffers are 40 lines each and the only complete image lives inside
the ILI9341, which this wiring cannot read back. `lv_snapshot_take()` draws the
active screen again into a full-size buffer in PSRAM, which costs about 1.4 KB of
flash and nothing at all until someone asks. Two consequences worth knowing:

- It captures **LVGL's state, not the panel's**, so a screen the idle ladder has
  blanked still yields exactly what a wake would put back on it — and equally,
  backlight level, the Dim rung and viewing angle are invisible here. For those, a
  phone photo is still the honest picture.
- Every data line is prefixed `S:` because the REPL and `ESP_LOG` share the one
  USB serial channel. Without the marker an INFO line landing between two chunks
  would corrupt the image; with it the host keeps only what it asked for.

The frame goes over as base64 in 768-byte chunks — the largest multiple of 3 that
gives a round 1024-character line, and base64 only concatenates on 3-byte
boundaries. The header carries the stride alongside the width, so a change to
`CONFIG_LV_DRAW_BUF_STRIDE_ALIGN` cannot silently shear the image.

A capture takes about 3 seconds, and two things had to be true for that. `shot`
waits for the USB TX FIFO to drain before it returns: a frame is ~200 KB, and
returning into the REPL with that still queued means its prompt goes into a full
FIFO and is dropped, after which the console ignores typed commands for ten to
fifteen seconds. And the REPL task gets an 8 KB stack rather than esp_console's
default 4 KB, because a snapshot recurses through the whole widget tree — at the
default it is not a slow capture but a stack-overflow panic and a reboot the
moment the command is typed. Both are worth knowing before adding any other
console command that does real work.

## Host tests

Two pieces are IDF-free by design and run on the dev machine: the alarm decision
logic, and the JSON the uploader puts on the wire.

```sh
./tests/host/run.sh                # build + run all cases
./tests/host/run.sh "[hysteresis]" # filter by tag
./tests/host/run.sh "[json]"       # …or by component
```

Catch2 v2 is vendored under `tests/host/third_party/` (no install), and its
`main()` sits in its own `main.cpp` so every `test_*.cpp` is a plain source file
— adding one costs a line of `CMakeLists.txt`. Design rule: if a test ever needs
an IDF include in `alert_logic.*` or `cloud_json.*`, the test isn't wrong, the
code is. Push the IDF dependency into the wrapper beside it.

## Project layout

```
.
├── GETTING_STARTED.md      the guide for someone using a post
├── partitions.csv          nvs + otadata + two 3 MB app slots + littlefs (~9.9 MB)
├── sdkconfig.defaults      PSRAM, flash, partition table, FreeRTOS tick, console, LVGL
├── main/                   boot sequencer, board_pins, opt-in bring-up probe
├── components/
│   ├── board_i2c/          the one shared I²C bus (touch + codec + sensor)
│   ├── cloud/              Wi-Fi + SNTP clock + the durable upload queue
│   ├── ota/                the running version, checked updates, and the rollback self-test
│   ├── audio/              ES8311 + I²S alarm tone (the only thing that makes noise)
│   ├── display/            ILI9341 over SPI + LVGL + LEDC backlight
│   ├── sensor/             MCP9808 driver + 1 Hz sampling task
│   ├── touch/              FT6336 capacitive touch as an LVGL indev
│   ├── recorder/           the one log writer + the graph's RAM ring
│   ├── ui/                 theme, shared chrome, screen fade engine, the screens
│   ├── storage/            LittleFS append-log + rotation with stable absolute indices
│   ├── settings/           NVS-backed name / interval / band / units / sound
│   ├── shell/              the USB serial REPL commands are registered into
│   ├── alert/              alarm decision logic (IDF-free, host-tested) + its tick
│   ├── power/              the idle ladder, the battery sense, deep sleep and its wakes
│   └── sleepstate/         what survives a deep sleep, and the clock it is kept on
├── tools/                  the Apps Script sink for the Sheet, the release script, the screenshot grabber
└── tests/host/             Catch2 unit tests, no IDF
```

### Two design notes worth knowing up front

**A schema change that cost nothing.** The record went from v1 (one sample, plus a
64-bit microsecond uptime) to v2 (mean/min/max, plus a 32-bit *second* uptime) and
stayed exactly 16 bytes — the narrowed uptime pays for the two new fields, and 32
bits of seconds is 136 years for a field whose only job is ordering records at least
ten seconds apart. Because the size is unchanged, record *N* is still at offset
*N* × 16, so v1 and v2 records coexist in one file, absolute indices and any cloud
high-water mark stay valid, and no history had to be rewritten or discarded.
`storage`'s reads upgrade v1 records on the way out — a v1 record becomes its own
min and max, which is honest: one sample really is all that interval ever
captured — so nothing above `storage` knows the older layout exists. That is what
the `version` byte was reserved for, and it only works if the size holds, which is
why it was worth narrowing a field rather than appending four bytes.

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
"order by `uptime_s` instead". A full power cycle loses the clock until Wi-Fi
returns.

## Status

**Everything is built. Sleeping on idle is held back until it has been proved on the bench.**

| | |
|---|---|
| ✅ Built | Repo skeleton, bring-up probe, `storage`, `settings`, `alert` decision logic, host tests |
| ✅ Verified | The gate: N16R8 (16 MB flash / 8 MB PSRAM), touch `0x38`, ES8311 `0x18`, MCP9808 `0x1A` reading room temperature |
| ✅ Verified | `display` — ILI9341 + LVGL drawing correctly at 320×240, backlight under LEDC PWM |
| ✅ Verified | `board_i2c` + `sensor` — MCP9808 found by manufacturer ID, sampled at 1 Hz, shown on screen |
| ✅ Built | `touch` — FT6336 via the FT5x06 driver, registered as an LVGL pointer indev |
| ✅ Built | `recorder` + `ui` — one record per interval, graph backfilled from the log on boot, main screen with readout / chart / status strip |
| ✅ Built | menu hub + three settings pages, ported stepper with coupled bounds, cross-fade between screens |
| ✅ Verified | the backlight genuinely dims under LEDC PWM — the wiki documents GPIO45 as a plain high/low enable, and it is not, so the ladder's Dim rung is real |
| ✅ Verified | `audio` — ES8311 over I²S, beeping alarm tone out of the speaker, driven from the Sound page's test button |
| ✅ Built | `cloud` + `shell` — Wi-Fi with unbounded backoff, SNTP clock, `cloud` serial command |
| ✅ Built | the upload queue — batched POSTs to Apps Script, NVS high-water mark, host-tested JSON |
| ✅ Built | the upload cadence — separate lit and dark floors on how often the radio comes up, bypassed by a flagged record |
| ✅ Built | battery in the upload envelope — pack volts, percent and supply per batch, with a `posts` tab to alert from |
| ✅ Built | manager alerts — the Apps Script emails on excursions, faults, low battery and silent posts, judged against each post's own upload gap |
| ✅ Built | the hardware alert window — the MCP9808's own comparator tracks the band, ready for the ALERT wire |
| ✅ Built | the ALERT line as a wake source — `ext1` beside touch on `ext0`, the wake cause logged, `power nap` to meter and prove it |
| ✅ Built | the sleep mirror — alarm, upload deadline, record counter and supply flag kept across deep sleep, deadlines on the wall clock |
| ✅ Built | the captive portal, the always-on config form, mDNS, and the Network screen |
| ✅ Built | the Configure page — a QR that joins the setup AP, closing it on the way out |
| ✅ Built | the form trimmed to network + identity; thresholds in tenths, hold-to-repeat, debounced writes |
| ✅ Built | alerts — the state machine driven at the sample cadence, the alarm panel with Snooze / Off, the tone in bursts |
| ✅ Built | power — the idle ladder, alarm sleep-inhibit, battery sense and state-of-charge meter, the Power page, Turn Off |
| ✅ Built | `shot` + `tools/screenshot.py` — real screen captures over USB, for the docs |
| ✅ Built | the quiet wake — a timer wake records, judges the alarm and uploads on the dark cadence without lighting the screen, coming up fully only for an alarm or a charger |
| ✅ Built | the OTA layout — two app slots, a rollback bootloader, and log numbering that never falls below the upload mark |
| ✅ Verified | over-the-air updates — by hand from the console, then from a post's `fw_pin`, a downgrade through the Sheet, and a fleet rollout from `fw_target`, each new image kept only once it uploaded. A rejected hash and a deadline rollback are built but not yet exercised |
| ✅ Built | the nightly archive — readings past `archive_after_days` move to a spreadsheet per month, and every tab is trimmed to its own columns |
| ✅ Built | the band from the Sheet — `config` for the fleet, a `posts` row for one post, locked on the Temperature page while set |
| 🟨 Gated | sleeping on idle — the ladder's fourth rung, behind `kAutoSleepReady` until the quiet wake has run on the bench and the sleep current is metered |

Build order: display → sensor → touch → main screen → menu → alerts → cloud →
power. Each step ends at something flashable and observable. (Sensor jumped
ahead of touch — with the part wired up, confirming it reads correctly was worth
more than an input device nothing yet listens to.)

`recorder` was not in the original plan as its own step. It landed with the main
screen because a chart needs a source: nothing was writing a `TempRecord` on the
settings interval, so the graph had no history to draw and none would have
survived a reboot. Making it one component with one job also gave the alarm's
sampling cadence and M9's RTC-memory mirror an obvious place to attach.

### Open questions

- **Power model.** The plan is always-on first (dim → backlight off → deep sleep
  on long idle, with a timer wake so readings continue), then *measure* before
  deciding whether deeper duty-cycling is worth the complexity. The numbers that
  decide it: average current with the screen on, dimmed, backlight off, and
  deep-sleeping between one-minute readings.
- **Is IO4 actually spare?** MCLK does not work on it, so the codec derives its
  master clock from BCLK and `kI2sMclkRouted` is false, which leaves the pin
  undriven. Whether that is a missing trace or a wrong pin number is unresolved,
  and it matters only because a genuinely free GPIO is worth knowing about. Flipping
  `kI2sMclkRouted` and tapping Test settles it in one flash.
- **Can the amplifier be gated at all?** The enable on IO1 turned out to be active
  low, and during the wiring sweep *both* polarities produced sound, which suggests
  the amp may be hard-enabled and IO1 not in control of it. If so the FM8002E draws
  its idle current continuously and M9's power measurements have to include it —
  `board_pins.hpp` currently claims the line is held inactive between alarms, which
  may be wishful.
- **Time zone.** The displayed clock renders in a compile-time POSIX zone string in
  [`ui_chrome.cpp`](components/ui/ui_chrome.cpp) — currently espressopost's, since
  the two share a bench. It belongs in `settings`, sent on the upload reply the way the
  band is, from the Sheet's own time zone; until then relocating a post is a
  one-string change. Nothing stored is affected either way: every record carries
  UTC.
