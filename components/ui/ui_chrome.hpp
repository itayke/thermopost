#pragma once

#include "lvgl.h"
#include "ui_theme.hpp"

// The widgets every screen builds out of. Extracted from ui_main.cpp when the menu
// arrived and needed the same capsule and the same top-left corner — a second copy
// of the pill code is how two screens' chrome starts disagreeing.
namespace thermopost::ui {

// Local time per `fmt` (strftime). Returns false and writes an empty string if the
// system clock has never been set, leaving the wording to the caller — the main
// screen's column is too narrow for the phrase the menu has room for. A 1970
// timestamp would be worse than either: one is a fact, the other is a bug report.
//
// Renders in kPosixTz, which is a compile-time constant for now. It belongs in
// settings once the config form can carry it (M8); until then a relocation is a
// one-string change in ui_chrome.cpp.
bool format_clock(char* out, size_t n, const char* fmt);

// A plain label with its font and colour set. Returned unpositioned; the caller
// places it, because every screen has its own reason for where things go.
lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color,
                     const char* initial);

// A fixed-width label whose text is aligned inside that width, so content can
// change length without moving anything around it.
lv_obj_t* make_boxed_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color,
                           int32_t w, lv_text_align_t align);

// A full-screen transparent container holding one screen's widgets, so the nav
// engine can hide a whole screen with one flag. Not scrollable and not clickable:
// it must not eat a tap meant for whatever is on top of it, and a stray drag must
// not scroll a layout that is positioned absolutely.
lv_obj_t* build_group(lv_obj_t* scr);

// An outlined capsule with a centred label — the shape of every button here.
// `text` may be nullptr for a caller that wants to fill the capsule itself.
// `on_tap` fires on LV_EVENT_CLICKED with `user_data`.
//
// The press tint is a pressed-state local style rather than an event handler, so
// LVGL applies and reverts it. Doing it by hand is a trap: LV_EVENT_RELEASED is
// sent *before* the indev clears LV_STATE_PRESSED, so a handler that reads the
// state leaves the pill stuck lit.
lv_obj_t* build_pill(lv_obj_t* parent, const char* text, int32_t w, int32_t h,
                     lv_color_t color, const lv_font_t* font, lv_event_cb_t on_tap,
                     void* user_data);

// The label inside a pill built above, so a caller can repaint a toggle's text
// without tracking two handles. Only meaningful for a pill built *with* text.
lv_obj_t* pill_label(lv_obj_t* pill);

// The top-left navigation pill. Both glyphs are painted rather than taken from the
// font: a painted glyph tints and scales with the layout instead of being pinned to
// whatever the font designer chose, and LVGL's built-in Montserrat has no
// left-chevron at all.
enum class NavGlyph : uint8_t { Burger, Chevron };

// Caller-owned so several can coexist — one per screen — without a global. The
// painter finds its instance through the glyph widget's user_data.
struct NavPill {
  lv_obj_t* btn;
  lv_obj_t* glyph;  // transparent widget with the painter attached
  NavGlyph  kind;
};

// Builds into `parent` at the shared top-left position. `on_tap` fires on
// LV_EVENT_CLICKED.
void build_nav_pill(lv_obj_t* parent, NavPill* pill, NavGlyph glyph,
                    const char* text, lv_event_cb_t on_tap);

}  // namespace thermopost::ui
