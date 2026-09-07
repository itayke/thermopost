#pragma once

#include <cstdint>

#include "lvgl.h"

// Screen switchboard and the cross-fade between screens, ported from
// espressopost's section-swap engine (ui_report.cpp's animate_section_swap and
// friends). Two phases: fade the outgoing screen's widgets out, swap the group
// flags in one frame at the midpoint, fade the incoming screen's widgets in.
//
// Two things carried over verbatim because both were learned the hard way there:
//
//   * Each widget gets its own opacity animation instead of one animation on the
//     group. Fading a container makes LVGL redraw the container's whole rectangle
//     every frame, including the empty space between the widgets; fading the widgets
//     confines each frame's redraw to the pixels that are actually changing.
//   * A no-op "driver" animation owns the phase timing, so a phase ends after
//     exactly one fade duration regardless of how many widgets took part — and
//     still ends when a screen's fade set happens to be empty.
//
// Where this departs from espressopost: the per-screen bookkeeping is a table
// rather than a switch statement repeated in four functions. Adding a screen there
// meant remembering five places; here it is one register_screen() call.
namespace thermopost::ui::nav {

enum class Screen : uint8_t {
  Main = 0,
  Menu,
  Temperature,
  Recording,
  Sound,
  Power,
  Network,
  Configure,
  kCount,
};

// Maximum widgets one screen can hand over to fade. Sized for the busiest screen
// (Main, at fifteen) with room to add; register_screen() logs and truncates rather
// than overflowing, so exceeding it degrades to a partial fade instead of memory
// corruption.
constexpr int kMaxFadeWidgets = 24;

// Create the input blocker and take ownership of `scr`. Call once, before any
// register_screen().
void init(lv_obj_t* scr);

// Register a screen's group container and the widgets that fade with it. `on_show`
// runs at the swap midpoint, while the screen is still hidden — the place to
// re-read settings, because a screen left open while the web form writes the same
// value would otherwise show a stale number.
//
// `fade` is copied, so a caller may pass a local array.
void register_screen(Screen s, lv_obj_t* group, lv_obj_t* const* fade, int n,
                     void (*on_show)());

// Show `Screen::Main` and hide everything else, with no animation. Call once after
// every screen is registered.
void start();

// Cross-fade to `s`. A no-op if it is already current or if a swap is in flight —
// taps are swallowed for the swap's duration, but a programmatic call could still
// arrive and re-entering the engine mid-swap would leave a screen half-faded.
void go(Screen s);

Screen current();

// Is this screen the one on display? Cheap, and the guard a screen's own refresh
// timer uses so an off-screen page doesn't poll the sensor or reformat labels
// forty times a second for nobody.
bool visible(Screen s);

}  // namespace thermopost::ui::nav
