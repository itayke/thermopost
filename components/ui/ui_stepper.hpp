#pragma once

#include <cstdint>

#include "lvgl.h"
#include "ui_theme.hpp"  // ext_click_for, for the constexpr sizes below

// Reusable value stepper — a [ (-) value (+) ] row, ported from espressopost's
// ui_stepper.cpp. It earns its place on a 2.8" panel because it is unmissable,
// needs no drag precision, and has no hit target smaller than a fingertip.
//
// The state lives in the caller so several instances coexist; the tap handlers
// find theirs through the widgets' user_data rather than a global.
//
// Five deliberate departures from the espressopost original, each forced by the
// data rather than by taste:
//
//   * `int16_t value`, not uint8_t — the range is -40…125 °C, which is signed and
//     wider than a byte once it is shown in °F.
//   * `const char* suffix`, not `char unit` — "°C" is three bytes, not a char.
//   * no `touched` / "--" gate — a shot's brew time was genuinely unset until it
//     was entered, but a threshold always has a value, so the gate could only add
//     a state that cannot happen.
//   * an explicit `step` — the interval spans 10…3600 s, and one second per tap
//     is not an interface.
//   * `min`/`max` are re-derived by the owner, not fixed at build time — the
//     minimum's ceiling is a function of the maximum, and it moves when that does.
//
// That last one is the interesting one, and it is why the bounds live in the state
// rather than in the config: the threshold pair is coupled, so `(+)` on the minimum
// simply stops below the maximum instead of letting the two cross and having
// alert::sanitize() quietly rewrite the input afterwards. A control that accepts a
// tap and then changes the number to something else is worse than one that doesn't
// move.
namespace thermopost::ui {

struct StepperState {
  int16_t     value;    // always within [min, max]; the discs cannot leave it
  int16_t     min;
  int16_t     max;
  int16_t     step;     // per tap
  const char* suffix;   // printed after the value ("°C", "s", "%", "" for none)
  lv_obj_t*   value_lbl;
  void (*on_change)();  // fired after each tap, after `value` has settled

  // Per repeat while a disc is held down. Zero — the default — means taps only.
  //
  // Opt-in rather than automatic because most steppers here do not want it: the
  // interval is ten table entries, the volume twenty taps end to end, and a
  // repeat on either would overshoot faster than a finger can lift. It exists for
  // the thresholds, where a tenth of a degree per tap is the right resolution and
  // the wrong way to cross a range 1650 of them wide.
  int16_t     hold_step;

  // Set by build_stepper when the config asks for the unit on its own line;
  // null otherwise, in which case the suffix is appended to the value instead.
  lv_obj_t*   suffix_lbl;

  // Optional. When set, it renders the readout instead of "<value><suffix>", and
  // `value` is free to be an index rather than the quantity itself. That is what
  // makes a 10…3600 s interval usable: a table of ten sensible periods is ten taps
  // end to end, where any fixed step over that range is either hundreds of taps or
  // too coarse to be useful near the bottom.
  void (*format)(char* out, size_t n, int16_t value);

  // The discs, kept by build_stepper so stepper_set_locked() can reach them.
  lv_obj_t*   minus;
  lv_obj_t*   plus;
};

// Visual config — the thresholds page wants a compact pair side by side, so the
// discs and the value font are per-instance rather than fixed.
struct StepperCfg {
  const lv_font_t* value_font;
  int32_t          btn_dx;    // disc centre offset either side of the row centre
  int32_t          btn_diam;

  // Put the unit on a second line under the number instead of after it.
  //
  // Forced by arithmetic, not taste: the discs leave 2·btn_dx − btn_diam of clear
  // space between them, which on the thresholds page is 66 px, and "21.5°C" at
  // Montserrat 24 wants about 78. The number alone fits; the pair does not. Moving
  // the discs apart is not the alternative — that widens stepper_width past what
  // the two-column layout asserts it can hold.
  bool             suffix_below;
};

// Overall size of the container build_stepper() returns, so a caller can lay a page
// out without having built the steppers yet. constexpr so a page can also
// static_assert that its columns fit the screen rather than discovering it on the
// panel, where the symptom is a disc that only responds on one side.
//
// Wider than the discs themselves: the container has to contain their extended click
// areas as well, or the parent clips taps in exactly the margin that exists to catch
// them.
constexpr int32_t stepper_ext_click(const StepperCfg& cfg) {
  return ext_click_for(cfg.btn_diam, cfg.btn_diam);
}
constexpr int32_t stepper_width(const StepperCfg& cfg) {
  return 2 * cfg.btn_dx + cfg.btn_diam + 2 * stepper_ext_click(cfg);
}
constexpr int32_t stepper_height(const StepperCfg& cfg) {
  return cfg.btn_diam + 2 * stepper_ext_click(cfg);
}

// Build the row into a transparent container under `parent`; returns the container
// so the caller can position it. Wires the (-)/(+) handlers to `st` and paints the
// value once.
lv_obj_t* build_stepper(lv_obj_t* parent, StepperState* st, const StepperCfg& cfg);

// Repaint the value readout from `st`. Call after changing `value` or `suffix`
// from outside — reloading a page from NVS, or a sibling stepper's on_change
// having clamped this one.
void stepper_refresh(StepperState* st);

// Clamp `value` into the current [min, max] and repaint. Returns true if the value
// actually moved, so a caller can persist only when something changed. This is what
// a coupled sibling calls after narrowing this stepper's bounds.
bool stepper_clamp(StepperState* st);

// Show the value as set elsewhere: the discs go muted and ignore taps, and the value
// stays readable. For a setting the Sheet controls.
void stepper_set_locked(StepperState* st, bool locked);

}  // namespace thermopost::ui
