#pragma once

#include "board_pins.hpp"
#include "lvgl.h"

// Shared palette and the layout frame every screen sits inside. Private to the
// `ui` component on purpose — nothing outside it should be positioning widgets,
// and it depends on board_pins.hpp, which is main/'s.
namespace thermopost::ui {

// Palette entries read like a CSS hex literal instead of three comma-separated
// bytes, so a swatch pastes in unchanged. espressopost used a macro for this;
// LV_COLOR_MAKE turns out to expand to a plain brace-init list `{b, g, r}`, not a
// compound literal, so a constexpr function does the job without the macro.
constexpr lv_color_t color(uint32_t rgb) {
  return LV_COLOR_MAKE(static_cast<uint8_t>((rgb >> 16) & 0xFF),
                       static_cast<uint8_t>((rgb >> 8) & 0xFF),
                       static_cast<uint8_t>(rgb & 0xFF));
}

// --- Palette ---------------------------------------------------------------
// Near-black rather than the pure #000 espressopost used: that panel was AMOLED,
// where black costs no power and burn-in is real. This is a backlit IPS, so #000
// buys nothing and shows the backlight's own unevenness — a few counts up hides
// it without looking grey.
constexpr lv_color_t kColorBg      = color(0x090B0D);
constexpr lv_color_t kColorPanel   = color(0x14171A);
constexpr lv_color_t kColorGrid    = color(0x24282C);
constexpr lv_color_t kColorMuted   = color(0x5F6368);
constexpr lv_color_t kColorSubText = color(0x9AA0A6);
constexpr lv_color_t kColorText    = color(0xE8EAED);

// Reading inside the band.
constexpr lv_color_t kColorNormal = color(0x36C5D9);
// The min/max envelope either side of the mean trace. Dim enough to read as
// context rather than as three competing lines — the eye should follow the mean
// and notice the envelope, not have to separate them.
constexpr lv_color_t kColorEnvelope = color(0x1B6673);
// High is warm and low is cold on purpose: the colour says which way the reading
// went before the number is read, which is what a screen glanced at across a room
// actually communicates.
constexpr lv_color_t kColorHigh  = color(0xFF6B4A);
constexpr lv_color_t kColorLow   = color(0x5AA9FF);
constexpr lv_color_t kColorFault = color(0xFFB020);
constexpr lv_color_t kColorOk    = color(0x3FD07F);

// The menu pill's amber, carried over from espressopost's presets screen so the
// two devices' menu affordance reads the same.
constexpr lv_color_t kColorMenu = color(0xF37B02);

// The stepper discs' amber, which happens to be the same one today. Its own name
// rather than a reuse of kColorMenu so restyling the editors' controls doesn't drag
// the navigation chrome along with it — the two are unrelated decisions.
constexpr lv_color_t kColorStepper = kColorMenu;

// --- Typography -------------------------------------------------------------
// Screens name a role, never a face. Three roles cover the whole UI: body for
// captions, chips, footers and the status strip; title for headings and every
// button label; readout for the one big number on the main screen. Changing what
// the UI is set in is then these three lines plus the heights below, rather than
// three dozen call sites that each happened to pick a size.
//
// LVGL's built-in Montserrat is what ships with the library, at whichever sizes
// sdkconfig.defaults enables. A different typeface has to be converted to a C
// array and compiled in — see the note there — but nothing outside this block
// needs to know that happened.
constexpr const lv_font_t* kFontBody    = &lv_font_montserrat_14;
constexpr const lv_font_t* kFontTitle   = &lv_font_montserrat_24;
constexpr const lv_font_t* kFontReadout = &lv_font_montserrat_42;

// --- Layout: 320x240 landscape ---------------------------------------------
// Taken from the display's own resolution rather than restated, so a rotation
// change cannot leave the layout silently disagreeing with the panel.
constexpr int32_t kScreenW = board::kLcdHRes;
constexpr int32_t kScreenH = board::kLcdVRes;
constexpr int32_t kEdgePad = 8;

// A row's height is its font's line_height, NOT its point size — Montserrat 14
// occupies 16 px and Montserrat 42 occupies 46. Budgeting by point size overflows
// the 240 px column by just enough that the time captions land on top of the
// status strip, which looks like a stray widget rather than an arithmetic error.
//
// These have to be literals because the layout below is constexpr and a font's
// line_height is only known at link time, so they are the one place the fonts
// above are restated. ui::init() checks them against the linked faces at boot and
// warns on a mismatch — which is what a font swap that forgot this looks like.
constexpr int32_t kFontBodyH    = 16;
constexpr int32_t kFontTitleH   = 27;
constexpr int32_t kFontReadoutH = 46;

// Rows, top to bottom: readout, chart, time captions, status strip. Everything is
// a fixed band except the chart, which absorbs the remainder — so rounding can
// never open a seam at the bottom edge.
// The top band holds three columns: the menu glyph with the clock beneath it on
// the left, the readout centred, and the threshold chips right. The readout is the
// tallest of the three, so it sets the band's height — and the chart, which takes
// whatever is left, grows and shrinks with it.
constexpr int32_t kTopPad    = 4;  // the big readout wants to sit high
constexpr int32_t kChartGap  = 6;
constexpr int32_t kReadoutY  = kTopPad;
constexpr int32_t kReadoutH  = kFontReadoutH;
constexpr int32_t kAxisH     = kFontBodyH;
// Two rows: the post's name, then the record counters. One row would be tidier,
// but the three counters run to about 250 px and a 24-character name needs most of
// the remaining width — they do not fit on a line together, and truncating either
// one costs more than the 16 px the chart gives up.
constexpr int32_t kStatusRows = 2;
constexpr int32_t kStatusH    = kStatusRows * kFontBodyH;

constexpr int32_t kChartX = kEdgePad;
constexpr int32_t kChartY = kReadoutY + kReadoutH + kChartGap;
constexpr int32_t kChartW = kScreenW - 2 * kEdgePad;
constexpr int32_t kChartH = kScreenH - kChartY - kAxisH - kStatusH - kEdgePad;
constexpr int32_t kAxisY   = kChartY + kChartH;
constexpr int32_t kStatusY = kAxisY + kAxisH;

// The chart's 1 px border insets its content area, and both the series and any
// child overlay are positioned inside it. Threshold lines have to use these, not
// the object's full size, or they sit up to two pixels off — which reads as a
// calibration fault rather than a layout one.
constexpr int32_t kChartBorderPx = 1;
constexpr int32_t kPlotW = kChartW - 2 * kChartBorderPx;
constexpr int32_t kPlotH = kChartH - 2 * kChartBorderPx;

static_assert(kChartH > 0, "layout bands do not fit the panel height");
static_assert(kStatusY + kStatusH + kEdgePad <= kScreenH, "status strip overflows");

// --- Shared chrome ----------------------------------------------------------
// Every tappable thing in this UI is a solid capsule: the control's own colour as
// the fill, its label knocked out of it. An outline was quieter beside the graph,
// but it also read as a decorated label rather than a target — a filled shape is
// unmistakably a button from across the room, which is the distance this panel is
// looked at from.
//
// The knocked-out label. Pure black rather than kColorBg: against a mid-bright
// fill the label wants every count of contrast the panel can give it, and it is
// not sitting on the background anyway.
constexpr lv_color_t kColorPillText = color(0x000000);

// One height for every text button — the menu's entries, the settings toggles, the
// test pill — so a screen of controls reads as one system instead of several sizes
// that happen to coexist. Width stays the caller's: a label decides how wide a
// capsule has to be, and nothing decides how tall.
//
// Two deliberate exceptions, both because their size is not a button-system
// decision:
//   * the stepper's discs, whose size is a diameter set by the glyph they hold —
//     tying it to this would make a font change move the discs;
//   * the nav pill, at kNavPillH below. At this height the clock beneath it
//     overflows the top band, which ui_main.cpp asserts; its 25 px belongs to the
//     main screen's layout, not to this.
constexpr int32_t kPillH = 35;

// The smallest a control may be to a finger. The drawn capsule is allowed to be
// smaller — lv_obj_set_ext_click_area grows the hit area without touching the
// chrome — but nothing should ask for more precision than this. A fingertip covers
// roughly this much, and the contact patch is not where the user thinks they are
// pointing (see the offset in touch.cpp).
constexpr int32_t kMinTouchPx = 48;

// Slop every control gets past its drawn edge, whatever its size. A capsule that
// already clears kMinTouchPx does not *need* one, but a tap that lands two pixels
// outside a button still meant that button — the drawn shape is where the control
// looks like it is, not where the finger has to be. This is the number to raise if
// the panel feels fussy.
//
// It is not free, and the cost is a layout constraint rather than a visual one:
// adjacent controls must sit more than twice this apart or their hit areas meet in
// the gap between them, which then belongs to whichever was built last. Every place
// that puts two controls side by side asserts it — see ui_menu's gutters and
// ui_settings' columns. Raising this without raising those is a compile error, by
// design: the symptom otherwise is a tap near a seam opening the wrong page, which
// reads as a flaky touch panel rather than as arithmetic.
constexpr int32_t kExtClickPad = 10;

// How far a control's hit area reaches past its capsule: the pad above, or further
// if the control is small enough to still be under kMinTouchPx at that pad. Driven
// by the smaller dimension, because ext_click_area is a single value applied to all
// four sides — so a wide, short pill is grown by its height.
constexpr int32_t ext_click_for(int32_t w, int32_t h) {
  const int32_t smaller = w < h ? w : h;
  const int32_t to_min = smaller >= kMinTouchPx ? 0 : (kMinTouchPx - smaller + 1) / 2;
  return to_min > kExtClickPad ? to_min : kExtClickPad;
}

// The navigation pill lives in the top-left corner of *every* screen: a burger and
// "Menu" on the main screen, a chevron and "Back" on the rest. Same corner, same
// size, so the way out never has to be looked for.
constexpr int32_t kNavPillW = 78;
constexpr int32_t kNavPillH = 25;
// The pill sits slightly below the band's top edge so it reads as optically level
// with the readout's digits, whose glyphs start below their own line box. Shared by
// the pill and the main screen's threshold chips so the two columns can't drift.
constexpr int32_t kTopRowNudge = 2;
constexpr int32_t kNavPillY = kTopPad + kTopRowNudge;
constexpr int32_t kNavRowGap = 6;  // between the pill's glyph and its label

// A screen's heading centres in the band to the right of the nav pill, not on the
// screen — a title long enough to be worth ellipsising would otherwise run under
// the pill, where the truncation happens off-centre and looks like a bug.
constexpr int32_t kHeadingY  = kTopPad;
constexpr int32_t kHeadingW  = kScreenW - 2 * (kEdgePad + kNavPillW);
constexpr int32_t kHeadingCx = kEdgePad + kNavPillW + kHeadingW / 2;
constexpr int32_t kHeadingDx = kHeadingCx - kScreenW / 2;
static_assert(kHeadingCx - kHeadingW / 2 >= kEdgePad + kNavPillW,
              "a centred heading would overlap the nav pill");

// One line of small text along the bottom of every screen but the main one. Here
// rather than in each page's own file because three of them had derived the same
// expression independently, and a fourth would have made it a convention nobody
// had written down.
constexpr int32_t kFooterY = kScreenH - kEdgePad - kFontBodyH;

}  // namespace thermopost::ui
