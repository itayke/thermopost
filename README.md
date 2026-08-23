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
idf.py build
idf.py -p /dev/cu.usbmodem<N> flash monitor   # Ctrl+] to exit
```

Editor red squigglies on `esp_err_t` and friends before the first build are
expected — clangd has no context until the build writes
`build/compile_commands.json`. The xtensa-only flag warnings (`-mlongcalls`, …)
are cosmetic and never go away.

## Usage

Flashing brings the board up through `display → nvs → storage → settings → i2c →
sensor → touch → audio → recorder → ui → shell → cloud` and draws the main screen:

```
 (☰ Menu)      72.4°F            ↑ 86.0
 14:32                           ↓ 59.0
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
it changes colour with the band — white inside it, warm above the maximum, cold
below the minimum, amber when the sensor isn't answering. The colour says which
way the temperature went before the digits are legible, which is what a screen
read across a room actually conveys.

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

### The menu and the settings screens

Tapping *Menu* cross-fades to the hub. Its heading is **the post's name**, not the
word "MENU" — in a rack of identical boxes, the useful thing to say at the top of a
settings screen is which box this is, and the chevron in the corner already says
what the screen is for. Under it sit four pages and a line carrying the full date,
the time and the total record count.

```
 (‹ Back)          North Tank
              rename from the setup page
   (   Thresholds   ) (    Recording   )

   (      Sound     ) (     Display    )
          Tue 28 Jul   14:32   4213 records
```

| page | controls | footer says |
|---|---|---|
| Thresholds | alarm below, alarm above, recovery margin | the exact stored hundredths |
| Recording | interval, points plotted | the time span the graph covers |
| Sound | alarm sound on/off, volume, **test alarm** | whether alarms will be audible |
| Display | temperature unit | that tapping the reading does this too |

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

Two consequences of stepping in whole degrees, both deliberate:

- The **stored value stays hundredths of a °C**. Each tap converts the displayed
  integer back, which keeps the standing promise that °F exists only at the point of
  display. A band set in °F therefore isn't a round number of °C — correct behaviour,
  not drift, and the footer prints the stored value so it is visible rather than
  mysterious.
- A stepper writes **only its own field**, taking its sibling from what is stored. A
  stored 20.5 °C minimum displays as 21, and writing that back while the user was
  only touching the maximum would move a threshold they never touched.

The interval stepper walks a table — 10 s, 15 s, 30 s, 1 m, 2 m, 5 m, 10 m, 15 m,
30 m, 1 h — instead of counting seconds. Ten taps covers the whole range; any fixed
step over 10…3600 s is either hundreds of taps or too coarse to reach the fast end.

The Sound page also carries a **Test alarm** button: two seconds of the real tone at
the real volume, so the speaker can be judged without waiting for a threshold to be
crossed. It is the same code path M7's alarm will use, which is the point — a test
that exercises a second implementation proves nothing about the first. A second tap
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

Each page re-reads its values at the midpoint, while it is still hidden. That is what
keeps a page opened while the web form writes the same setting from showing — and then
clobbering — a stale number. Both paths go through the same
`settings::set_*` setters, so NVS stays the single source of truth.

### The post's name

Thermo Posts are meant to run in a group, one per tank, so each carries a name
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

**What is different here is the lifetime.** espressopost's HTTP server existed only
while the setup AP was up: the first `GOT_IP` tore down the AP, the DNS responder
and the server together, because provisioning was all the form ever did. Here the
same form is also where thresholds, the interval and the post's name are edited, so
that model would mean rejoining a setup AP to adjust a number. The two halves are
therefore split:

| | lifetime |
|---|---|
| the HTTP server and the form | the device's whole life, on every interface |
| the SoftAP and the DNS hijack | until credentials land, then gone |

One server serves both, because it binds `INADDR_ANY` — the AP is a *way to reach*
the form, not a second copy of it. After provisioning the post is reachable at
`http://<name>.local/` over the LAN, and the Network screen shows the raw IP
beside the `.local` name because some networks drop multicast and a post that could
only offer a name it can't resolve has no address to give at all.

The form carries every setting, each **pre-filled from the current value**, so it
doubles as a way to read a post's configuration. The token is the one exception and
is never echoed back. Two deliberate choices in it:

- **Temperatures are always in °C**, whatever the screen is set to show. The stored
  value is hundredths of a °C; a form that rendered °F would have to interpret its
  own numbers in whichever unit was selected *when the page was drawn*, and a unit
  change submitted alongside a threshold change would be genuinely ambiguous. The
  unit select on the form sets what the panel displays and nothing else.
- **Re-submitting the network you are already on does nothing.** The field comes back
  prefilled, and re-applying credentials drops the link — so nudging a threshold from
  a browser would otherwise cost a reconnect every time.

Both write paths — form and menu — go through the same `settings::set_*` setters, so
NVS stays the single source of truth. One loose end accepted rather than solved: a
menu page left open while the form writes the same setting shows a stale value until
it is re-entered. Screens re-read on show, which covers the realistic case; a change
bus would be over-engineering for a single-user device.

Worth being clear-eyed about the tradeoff: an always-listening HTTP server that
writes settings, on plain HTTP, with no auth. On a home or brewery LAN that is the
same exposure the setup AP already accepts, and the blast radius is a temperature
threshold. It should not grow anything reachable from outside the LAN without
revisiting that.

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
├── partitions.csv          nvs + factory (4 MB) + littlefs (~12 MB)
├── sdkconfig.defaults      PSRAM, flash, partition table, FreeRTOS tick, console, LVGL
├── main/                   boot sequencer, board_pins, opt-in bring-up probe
├── components/
│   ├── board_i2c/          the one shared I²C bus (touch + codec + sensor)
│   ├── cloud/              Wi-Fi + SNTP clock + the durable upload queue
│   ├── audio/              ES8311 + I²S alarm tone (the only thing that makes noise)
│   ├── display/            ILI9341 over SPI + LVGL + LEDC backlight
│   ├── sensor/             MCP9808 driver + 1 Hz sampling task
│   ├── touch/              FT6336 capacitive touch as an LVGL indev
│   ├── recorder/           the one log writer + the graph's RAM ring
│   ├── ui/                 theme, shared chrome, screen fade engine, the screens
│   ├── storage/            LittleFS append-log + rotation with stable absolute indices
│   ├── settings/           NVS-backed name / interval / band / units / sound
│   ├── shell/              the USB serial REPL commands are registered into
│   └── alert/              alarm decision logic (IDF-free, host-tested)
├── tools/                  the Apps Script sink for the Sheet
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

**Bring-up passed. Drivers next.**

| | |
|---|---|
| ✅ Built | Repo skeleton, bring-up probe, `storage`, `settings`, `alert` decision logic, host tests |
| ✅ Verified | The gate: N16R8 (16 MB flash / 8 MB PSRAM), touch `0x38`, ES8311 `0x18`, MCP9808 `0x1A` reading room temperature |
| ✅ Verified | `display` — ILI9341 + LVGL drawing correctly at 320×240, backlight under LEDC PWM |
| ✅ Verified | `board_i2c` + `sensor` — MCP9808 found by manufacturer ID, sampled at 1 Hz, shown on screen |
| ✅ Built | `touch` — FT6336 via the FT5x06 driver, registered as an LVGL pointer indev |
| ✅ Built | `recorder` + `ui` — one record per interval, graph backfilled from the log on boot, main screen with readout / chart / status strip |
| ✅ Built | menu hub + four settings pages, ported stepper with coupled bounds, cross-fade between screens |
| ✅ Verified | `audio` — ES8311 over I²S, beeping alarm tone out of the speaker, driven from the Sound page's test button |
| ✅ Built | `cloud` + `shell` — Wi-Fi with unbounded backoff, SNTP clock, `cloud` serial command |
| ✅ Built | the upload queue — batched POSTs to Apps Script, NVS high-water mark, host-tested JSON |
| ✅ Built | the captive portal, the always-on config form, mDNS, and the Network screen |
| ⬜️ Not started | alerts (the state machine's wiring and visuals), power |

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
- **Default alarm band.** Currently **20–25 °C**, deliberately tight around room
  temperature so both directions are reachable on a desk — cup the sensor to breach
  the maximum, a cold drink or canned air to breach the minimum. That makes the
  alarm path, the record flags and the graph's envelope testable in seconds instead
  of needing a tank. It follows that a device sitting on a desk will alarm, which is
  the intent of these numbers rather than a bug; 15–30 °C is the sane generic band
  to go back to once the alarms are proven.
- **Time zone.** The displayed clock renders in a compile-time POSIX zone string in
  [`ui_chrome.cpp`](components/ui/ui_chrome.cpp) — currently espressopost's, since
  the two share a bench. It belongs in `settings` and on the config form, which now
  exists and could carry it; until then relocating a post is a one-string change. Nothing stored is
  affected either way: every record carries UTC.
