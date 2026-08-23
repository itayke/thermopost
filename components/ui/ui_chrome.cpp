#include "ui_chrome.hpp"

#include <cstdlib>  // setenv
#include <ctime>

namespace thermopost::ui {
namespace {

// --- Wall clock -------------------------------------------------------------
// SNTP lands UTC in the system clock (there is no RTC chip on this board), and
// localtime_r needs a zone to render it. Matches espressopost's, since these
// devices share a bench; one string relocates the displayed clock.
constexpr const char* kPosixTz = "EST5EDT,M3.2.0,M11.1.0";

// Below this, the clock has never been set by SNTP and every record carries
// epoch_s = 0 (see storage.hpp). Roughly Nov 2023 — comfortably after any
// plausible build date and far above the 1970 an unsynced ESP32 reports.
constexpr std::time_t kEpochValidFrom = 1'700'000'000;

// --- The painted glyphs -----------------------------------------------------
// One box, two glyphs, so the pill's flex row measures the same width whichever
// one it holds and "Menu" and "Back" sit at the same x.
constexpr int32_t kGlyphW = 14;
constexpr int32_t kGlyphH = 16;

constexpr int32_t kBurgerBarH = 1;  // bar thickness; thinner than espressopost's 3
constexpr int32_t kBurgerGap  = 4;  // centre-to-centre spacing
constexpr int32_t kBurgerRows = 3;
static_assert((kBurgerRows - 1) * kBurgerGap + kBurgerBarH <= kGlyphH,
              "hamburger bars overflow their glyph box");

// Thicker than a burger bar: a one-pixel diagonal reads as an artefact, where
// three one-pixel horizontals still read as a stack.
constexpr int32_t kChevronStroke = 2;
// Half the stroke, so the drawn width stays inside the glyph box instead of being
// clipped by it.
constexpr int32_t kChevronInset = kChevronStroke / 2 + 1;
// Narrower than the burger — a chevron as wide as it is tall looks blunt.
constexpr int32_t kChevronW = 8;
// Both glyphs have to fit the one box, because that box is what the flex row
// measures: a glyph that needed a taller one would make the Back pill lay out
// differently from the Menu pill, and the two are supposed to be the same object in
// two moods.
static_assert(kChevronW <= kGlyphW && kChevronStroke < kGlyphH,
              "the chevron overflows the shared glyph box");

// A press darkens the control's own colour rather than swapping it for another
// hue: the feedback should read as "this one, pushed in", and a jump to a
// different colour reads as a change of meaning instead. Shallower than the 50%
// the outlined pills used — a whole capsule of fill changing tone is far louder
// than a 2 px stroke doing it, and going deeper starves the black label of the
// contrast it is knocked out of.
constexpr lv_opa_t kPressedDarken = LV_OPA_30;

lv_color_t pressed_tint(lv_color_t base) {
  return lv_color_darken(base, kPressedDarken);
}

// LV_PART_* and LV_STATE_* are separate enumerations, and C++20 deprecated OR-ing
// two different ones. LVGL's selector parameter is a plain uint32 anyway, so the
// combination is named once here instead of being spelled out — and cast — at every
// call site.
constexpr lv_style_selector_t kSelPressed =
    static_cast<lv_style_selector_t>(LV_PART_MAIN) |
    static_cast<lv_style_selector_t>(LV_STATE_PRESSED);

// Painted at draw time from the button's live state, so this needs no bookkeeping
// and cannot end up stuck lit after a press that was cancelled by a drag.
void draw_nav_glyph(lv_event_t* e) {
  auto* pill = static_cast<NavPill*>(lv_event_get_user_data(e));
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr || pill == nullptr) return;
  auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const int32_t cx = coords.x1 + lv_area_get_width(&coords) / 2;
  const int32_t cy = coords.y1 + lv_area_get_height(&coords) / 2;
  // Constant across states, like the word beside it: the capsule's fill is what a
  // press darkens, and a glyph that dimmed along with it would read as two
  // separate things reacting.
  const lv_color_t tint = kColorPillText;

  // LVGL multiplies LV_STYLE_OPA up the parent chain for the widgets it draws
  // itself, but a hand-rolled draw call is outside that — so a screen cross-fade
  // would take the capsule and the word away and leave the glyph hanging at full
  // strength. Read the inherited value and apply it here.
  const lv_opa_t opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
  if (opa <= LV_OPA_MIN) return;

  if (pill->kind == NavGlyph::Burger) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = tint;
    d.bg_opa = opa;
    d.radius = kBurgerBarH / 2;

    const int32_t half_w = kGlyphW / 2;
    const int32_t half_h = kBurgerBarH / 2;
    for (int32_t row = -(kBurgerRows / 2); row <= kBurgerRows / 2; ++row) {
      const int32_t yc = cy + row * kBurgerGap;
      lv_area_t a = {cx - half_w, yc - half_h, cx + half_w, yc + half_h};
      lv_draw_rect(layer, &d, &a);
    }
    return;
  }

  // Two segments meeting at the tip, both rounded there so the join is a point
  // rather than a notch. Drawn as two p1/p2 calls rather than one points array
  // because that is the path every rendering backend takes.
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = tint;
  d.width = kChevronStroke;
  d.opa = opa;
  d.round_start = 1;
  d.round_end = 1;

  const int32_t x_back = cx + kChevronW / 2;
  const int32_t x_tip  = cx - kChevronW / 2;
  const int32_t y_top  = cy - (kGlyphH / 2 - kChevronInset);
  const int32_t y_bot  = cy + (kGlyphH / 2 - kChevronInset);

  d.p1 = {static_cast<lv_value_precise_t>(x_back), static_cast<lv_value_precise_t>(y_top)};
  d.p2 = {static_cast<lv_value_precise_t>(x_tip),  static_cast<lv_value_precise_t>(cy)};
  lv_draw_line(layer, &d);
  d.p1 = {static_cast<lv_value_precise_t>(x_tip),  static_cast<lv_value_precise_t>(cy)};
  d.p2 = {static_cast<lv_value_precise_t>(x_back), static_cast<lv_value_precise_t>(y_bot)};
  lv_draw_line(layer, &d);
}

}  // namespace

bool format_clock(char* out, size_t n, const char* fmt) {
  if (out == nullptr || n == 0) return false;
  out[0] = '\0';

  // Once, lazily: the zone only matters when something asks for the time, and
  // doing it here keeps ui::init() from having to know the clock exists.
  static bool tz_set = false;
  if (!tz_set) {
    setenv("TZ", kPosixTz, 1);
    tzset();
    tz_set = true;
  }

  const std::time_t now = std::time(nullptr);
  if (now < kEpochValidFrom) return false;
  std::tm tm = {};
  localtime_r(&now, &tm);
  return std::strftime(out, n, fmt, &tm) > 0;
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color,
                     const char* initial) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
  lv_label_set_text(l, initial);
  return l;
}

lv_obj_t* make_boxed_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color,
                           int32_t w, lv_text_align_t align) {
  lv_obj_t* l = make_label(parent, font, color, "");
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_align(l, align, LV_PART_MAIN);
  return l;
}

lv_obj_t* build_group(lv_obj_t* scr) {
  lv_obj_t* g = lv_obj_create(scr);
  lv_obj_set_size(g, kScreenW, kScreenH);
  lv_obj_set_pos(g, 0, 0);
  lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(g, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g, 0, LV_PART_MAIN);
  lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
  return g;
}

lv_obj_t* build_pill(lv_obj_t* parent, const char* text, int32_t w, int32_t h,
                     lv_color_t color, const lv_font_t* font, lv_event_cb_t on_tap,
                     void* user_data) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  // Half the height: a capsule, whatever the caller's aspect ratio. A square gets a
  // circle out of the same line, which is what the stepper's discs are.
  lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  lv_obj_set_ext_click_area(b, ext_click_for(w, h));
  pill_set_color(b, color);

  // One text colour, no pressed variant — the fill carries the press. text_color is
  // inheritable, so a label parented *directly* on the capsule picks it up without
  // needing its own — but only directly: inheritance stops at the first ancestor
  // that has the property, and the default theme puts a text_color on every plain
  // container, so a wrapper in between silently swallows this. That is why
  // build_nav_pill lays its contents out on the button itself.
  lv_obj_set_style_text_color(b, kColorPillText, LV_PART_MAIN);

  // The default theme grows a pressed button by a few pixels and lays a black
  // recolour over it. Both are overridden here rather than switched off in
  // menuconfig, so the intent lives next to the styling it belongs to: a press
  // should darken the control, not resize it — a capsule that changes size nudges
  // everything the eye was using to aim.
  lv_obj_set_style_transform_width(b, 0, kSelPressed);
  lv_obj_set_style_transform_height(b, 0, kSelPressed);
  // Dropped as well, because a recolour applies to what LVGL draws but not to the
  // hand-painted nav glyph — leaving it on would darken the fill and the word while
  // the burger stayed as it was.
  lv_obj_set_style_recolor_opa(b, LV_OPA_TRANSP, kSelPressed);

  if (on_tap != nullptr) lv_obj_add_event_cb(b, on_tap, LV_EVENT_CLICKED, user_data);

  if (text != nullptr) {
    lv_obj_t* lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
  }
  return b;
}

lv_obj_t* pill_label(lv_obj_t* pill) { return lv_obj_get_child(pill, 0); }

void pill_set_color(lv_obj_t* pill, lv_color_t color) {
  lv_obj_set_style_bg_color(pill, color, LV_PART_MAIN);
  lv_obj_set_style_bg_color(pill, pressed_tint(color), kSelPressed);
}

void build_nav_pill(lv_obj_t* parent, NavPill* pill, NavGlyph glyph, const char* text,
                    lv_event_cb_t on_tap) {
  *pill = {};
  pill->kind = glyph;
  // Built without a label: the glyph and the word are laid out as a centred flex row
  // *on the capsule itself*, which is what makes the pair optically centred rather
  // than the word alone with a glyph hanging off it.
  //
  // On the button and not in a wrapper container, deliberately. A wrapper is the
  // tidier structure and it costs the label its colour: text_color is inheritable,
  // inheritance stops at the first ancestor that sets it, and the default theme
  // gives every plain container one — so the word came out in the theme's colour
  // rather than the capsule's.
  pill->btn = build_pill(parent, nullptr, kNavPillW, kNavPillH, kColorMenu,
                         &lv_font_montserrat_14, on_tap, nullptr);
  lv_obj_set_pos(pill->btn, kEdgePad, kNavPillY);
  lv_obj_set_style_pad_all(pill->btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(pill->btn, kNavRowGap, LV_PART_MAIN);
  lv_obj_set_layout(pill->btn, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(pill->btn, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pill->btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(pill->btn, LV_OBJ_FLAG_SCROLLABLE);

  // A transparent widget of the glyph's exact size, so the flex row measures it
  // like any other child even though nothing but a painter fills it.
  pill->glyph = lv_obj_create(pill->btn);
  lv_obj_set_size(pill->glyph, kGlyphW, kGlyphH);
  lv_obj_set_style_bg_opa(pill->glyph, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(pill->glyph, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(pill->glyph, 0, LV_PART_MAIN);
  lv_obj_remove_flag(pill->glyph, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(pill->glyph, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(pill->glyph, draw_nav_glyph, LV_EVENT_DRAW_MAIN, pill);

  // Inherits its colour, and the pressed variant, from the capsule it sits on.
  lv_obj_t* lbl = lv_label_create(pill->btn);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(lbl, text);
}

}  // namespace thermopost::ui
