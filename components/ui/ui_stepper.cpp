#include "ui_stepper.hpp"

#include <algorithm>
#include <cstdio>

#include "ui_chrome.hpp"
#include "ui_theme.hpp"

namespace thermopost::ui {
namespace {

// Widest thing the value label ever holds: "-40" plus "°C" plus the terminator,
// with room to spare for an interval in seconds.
constexpr size_t kValueTextMax = 12;

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
void step_by(StepperState* st, int dir) {
  const int32_t next = static_cast<int32_t>(st->value) +
                       static_cast<int32_t>(dir) * static_cast<int32_t>(st->step);
  const int16_t clamped = clamp_to_range(st, next);
  if (clamped == st->value) return;  // already against the bound; nothing to persist
  st->value = clamped;
  stepper_refresh(st);
  if (st->on_change != nullptr) st->on_change();
}

void stepper_minus(lv_event_t* e) {
  step_by(static_cast<StepperState*>(lv_event_get_user_data(e)), -1);
}

void stepper_plus(lv_event_t* e) {
  step_by(static_cast<StepperState*>(lv_event_get_user_data(e)), +1);
}

// One filled disc with a centred glyph. Same capsule as every other button here —
// a circle is just a capsule whose radius is half its side.
lv_obj_t* build_disc(lv_obj_t* parent, const char* glyph, int32_t diam,
                     lv_event_cb_t cb, StepperState* st) {
  return build_pill(parent, glyph, diam, diam, kColorStepper,
                    kFontTitle, cb, st);
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
  lv_obj_align(st->value_lbl, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* minus = build_disc(row, LV_SYMBOL_MINUS, cfg.btn_diam, stepper_minus, st);
  lv_obj_align(minus, LV_ALIGN_CENTER, -cfg.btn_dx, 0);
  lv_obj_t* plus = build_disc(row, LV_SYMBOL_PLUS, cfg.btn_diam, stepper_plus, st);
  lv_obj_align(plus, LV_ALIGN_CENTER, +cfg.btn_dx, 0);

  stepper_refresh(st);
  return row;
}

void stepper_refresh(StepperState* st) {
  if (st == nullptr || st->value_lbl == nullptr) return;
  char buf[kValueTextMax];
  if (st->format != nullptr) {
    st->format(buf, sizeof(buf), st->value);
  } else {
    std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(st->value),
                  st->suffix != nullptr ? st->suffix : "");
  }
  lv_label_set_text(st->value_lbl, buf);
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
