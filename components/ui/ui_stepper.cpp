#include "ui_stepper.hpp"

#include <algorithm>
#include <cstdio>

#include "ui_chrome.hpp"
#include "ui_theme.hpp"

namespace thermopost::ui {
namespace {

// Widest thing the value label ever holds: "-40.0" plus "°C" plus the terminator,
// with room to spare for an interval in seconds.
constexpr size_t kValueTextMax = 12;

// --- Hold to repeat ---------------------------------------------------------
// LVGL repeats a long press every LV_INDEV_DEF_LONG_PRESS_REP_TIME (100 ms), so a
// held disc moves ten steps a second. On the thresholds that is 1 °C/s, which is
// right for a nudge and far too slow to cross a 165 °C range — hence the second
// gear. Two seconds of holding is well past "I meant one tap".
constexpr int     kHoldFastAfter  = 20;
constexpr int16_t kHoldFastFactor = 10;

// The panel is a single-touch indev, so exactly one disc can be held at a time and
// this does not have to live per-stepper. Reset on press rather than on release,
// because LVGL sends CLICKED *after* RELEASED and the click handler reads it to
// decide whether the release ends a hold or is a tap of its own.
int s_held_repeats = 0;

// A stepper whose bounds are coupled to a sibling's can be handed an inverted pair
// for one frame — a band collapsed to a single degree puts the minimum's ceiling one
// below its own floor. std::clamp is undefined behaviour when the high bound is
// below the low one, so the range is squared up here rather than trusted. The value
// then pins to `min`, which is the sane reading of an empty range.
int16_t clamp_to_range(const StepperState* st, int32_t v) {
  const int32_t hi = std::max<int32_t>(st->min, st->max);
  return static_cast<int16_t>(std::clamp<int32_t>(v, st->min, hi));
}

// Step towards `dir` (±1), landing exactly on the bound rather than stopping one
// step short of it. Without the clamp, an interval at 3590 with a step of 20 could
// never reach its own 3600 maximum — the tap would be rejected as out of range and
// the control would look broken at exactly the value a user is most likely to want.
void step_by(StepperState* st, int dir, bool hold) {
  int32_t amount = hold ? st->hold_step : st->step;
  if (hold && s_held_repeats > kHoldFastAfter) amount *= kHoldFastFactor;
  const int32_t next = static_cast<int32_t>(st->value) + static_cast<int32_t>(dir) * amount;
  const int16_t clamped = clamp_to_range(st, next);
  if (clamped == st->value) return;  // already against the bound; nothing to persist
  st->value = clamped;
  stepper_refresh(st);
  if (st->on_change != nullptr) st->on_change();
}

// A tap. Suppressed when the press that is ending was a hold: LVGL sends CLICKED
// on release whether or not it already sent repeats, and without this every hold
// would move one step further than the finger asked for.
void stepper_click(lv_event_t* e, int dir) {
  if (s_held_repeats > 0) return;
  step_by(static_cast<StepperState*>(lv_event_get_user_data(e)), dir, /*hold=*/false);
}

void stepper_repeat(lv_event_t* e, int dir) {
  auto* st = static_cast<StepperState*>(lv_event_get_user_data(e));
  if (st->hold_step == 0) return;  // this stepper is taps only
  ++s_held_repeats;
  step_by(st, dir, /*hold=*/true);
}

void stepper_minus(lv_event_t* e) { stepper_click(e, -1); }
void stepper_plus(lv_event_t* e)  { stepper_click(e, +1); }
void stepper_minus_hold(lv_event_t* e) { stepper_repeat(e, -1); }
void stepper_plus_hold(lv_event_t* e)  { stepper_repeat(e, +1); }
void stepper_pressed(lv_event_t*) { s_held_repeats = 0; }

// One filled disc with a centred glyph. Same capsule as every other button here —
// a circle is just a capsule whose radius is half its side.
lv_obj_t* build_disc(lv_obj_t* parent, const char* glyph, int32_t diam,
                     lv_event_cb_t cb, lv_event_cb_t hold_cb, StepperState* st) {
  lv_obj_t* d = build_pill(parent, glyph, diam, diam, kColorStepper,
                           kFontTitle, cb, st);
  lv_obj_add_event_cb(d, hold_cb, LV_EVENT_LONG_PRESSED_REPEAT, st);
  lv_obj_add_event_cb(d, stepper_pressed, LV_EVENT_PRESSED, st);
  return d;
}

}  // namespace

lv_obj_t* build_stepper(lv_obj_t* parent, StepperState* st, const StepperCfg& cfg) {
  // Transparent container, sized by stepper_width/height to include each disc's
  // extended click area — a parent cut to the discs would clip taps in exactly
  // the margin that exists to catch them.
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, stepper_width(cfg), stepper_height(cfg));
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

  // Not tappable, unlike espressopost's: there a tap on "--" committed the seeded
  // value, and with no untouched state there is nothing for a tap to commit.
  st->value_lbl = make_label(row, cfg.value_font, kColorText, "");
  st->suffix_lbl = nullptr;

  if (cfg.suffix_below) {
    // The two labels are centred as one stack rather than each on its own, so the
    // number sits where a single line would have and the unit hangs below it —
    // otherwise the value rides high and stops sharing a centreline with the discs.
    st->suffix_lbl = make_label(row, kFontBody, kColorSubText, "");
    const int32_t value_h = lv_font_get_line_height(cfg.value_font);
    const int32_t suffix_h = lv_font_get_line_height(kFontBody);
    const int32_t stack_h = value_h + suffix_h;
    lv_obj_align(st->value_lbl, LV_ALIGN_CENTER, 0, value_h / 2 - stack_h / 2);
    lv_obj_align(st->suffix_lbl, LV_ALIGN_CENTER, 0, stack_h / 2 - suffix_h / 2);
  } else {
    lv_obj_align(st->value_lbl, LV_ALIGN_CENTER, 0, 0);
  }

  lv_obj_t* minus = build_disc(row, LV_SYMBOL_MINUS, cfg.btn_diam, stepper_minus,
                               stepper_minus_hold, st);
  lv_obj_align(minus, LV_ALIGN_CENTER, -cfg.btn_dx, 0);
  lv_obj_t* plus = build_disc(row, LV_SYMBOL_PLUS, cfg.btn_diam, stepper_plus,
                              stepper_plus_hold, st);
  lv_obj_align(plus, LV_ALIGN_CENTER, +cfg.btn_dx, 0);

  stepper_refresh(st);
  return row;
}

void stepper_refresh(StepperState* st) {
  if (st == nullptr || st->value_lbl == nullptr) return;
  char buf[kValueTextMax];
  if (st->format != nullptr) {
    st->format(buf, sizeof(buf), st->value);
  } else if (st->suffix_lbl != nullptr) {
    // The unit has a label of its own, so the value label holds the number alone.
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(st->value));
  } else {
    std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(st->value),
                  st->suffix != nullptr ? st->suffix : "");
  }
  lv_label_set_text(st->value_lbl, buf);
  if (st->suffix_lbl != nullptr) {
    lv_label_set_text(st->suffix_lbl, st->suffix != nullptr ? st->suffix : "");
  }
}

bool stepper_clamp(StepperState* st) {
  if (st == nullptr) return false;
  const int16_t clamped = clamp_to_range(st, st->value);
  if (clamped == st->value) return false;
  st->value = clamped;
  stepper_refresh(st);
  return true;
}

}  // namespace thermopost::ui
