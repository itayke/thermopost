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
constexpr lv_color_t kColorMenu = color(0xC88036);

// --- Layout: 320x240 landscape ---------------------------------------------
// Taken from the display's own resolution rather than restated, so a rotation
// change cannot leave the layout silently disagreeing with the panel.
constexpr int32_t kScreenW = board::kLcdHRes;
constexpr int32_t kScreenH = board::kLcdVRes;
constexpr int32_t kEdgePad = 8;

// A row's height is its font's line_height, NOT its point size — Montserrat 14
// occupies 16 px and Montserrat 48 occupies 52. Budgeting by point size overflows
// the 240 px column by just enough that the time captions land on top of the
// status strip, which looks like a stray widget rather than an arithmetic error.
// ui::init() checks these against the linked fonts at boot.
constexpr int32_t kFont14H = 16;
constexpr int32_t kFont48H = 52;

// Rows, top to bottom: readout, chart, time captions, status strip. Everything is
// a fixed band except the chart, which absorbs the remainder — so rounding can
// never open a seam at the bottom edge.
// The top band holds three columns: the menu glyph with the clock beneath it on
// the left, the readout centred, and the threshold chips right. The 48 px readout
// is the tallest of the three, so it sets the band's height.
constexpr int32_t kTopPad    = 4;  // the big readout wants to sit high
constexpr int32_t kChartGap  = 6;
constexpr int32_t kReadoutY  = kTopPad;
constexpr int32_t kReadoutH  = kFont48H;
constexpr int32_t kAxisH     = kFont14H;
// Two rows: the post's name, then the record counters. One row would be tidier,
// but the three counters run to about 250 px and a 24-character name needs most of
// the remaining width — they do not fit on a line together, and truncating either
// one costs more than the 16 px the chart gives up.
constexpr int32_t kStatusRows = 2;
constexpr int32_t kStatusH    = kStatusRows * kFont14H;

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

}  // namespace thermopost::ui
