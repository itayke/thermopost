#include "ui.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include "alert_logic.hpp"
#include "display.hpp"
#include "esp_log.h"
#include "recorder.hpp"
#include "sensor.hpp"
#include "settings.hpp"
#include "ui_theme.hpp"

namespace thermopost::ui {
namespace {

constexpr const char* kTag = "ui";

// Poll faster than the sensor samples so the readout is never more than a
// fraction of a period behind the value it is showing. The chart is guarded
// separately by recorder::generation() — this cadence is for the number, not the
// graph.
constexpr uint32_t kRefreshMs = 250;

constexpr size_t kTextMax = 64;
// Sized to its longest content rather than reusing kTextMax: formatting a 64-byte
// buffer into another 64-byte buffer is a truncation GCC is right to refuse, and
// the honest fix is the smaller buffer, not a bigger target.
constexpr size_t kClockTextMax = 16;
constexpr const char* kNoValue = "--.-";

// Short on purpose. "clock not set" is the accurate phrase and it is what the menu
// will say, but at Montserrat 14 it is wide enough to reach under a centred 48 px
// readout — and a status line that collides with the number it sits beside is
// worse than a terser one.
constexpr const char* kNoClock = "no clock";

// --- Top band: menu pill + clock (left) -------------------------------------
// An outlined capsule holding a hamburger glyph and the word "Menu", ported from
// espressopost's menu pill so the affordance reads identically on both devices.
// Scaled down for a 320 px screen: its label is Montserrat 14 rather than 24.
constexpr int32_t kMenuBtnW = 78;
constexpr int32_t kMenuBtnH = 25;
constexpr int32_t kMenuBtnY = kTopPad + 2;
constexpr int32_t kMenuBtnStroke = 2;
constexpr int32_t kMenuBtnExtClick = 10;  // hit area grows; the outline does not
constexpr int32_t kMenuRowGap = 6;        // between the glyph and the label

// Hand-drawn rather than LV_SYMBOL_BARS, which the built-in font does carry.
// espressopost's reason holds here: a painted glyph tints and scales exactly with
// the layout instead of being pinned to whatever the font designer chose, and it
// matches the house style of its other custom glyphs.
constexpr int32_t kBurgerW    = 14;  // bar length
constexpr int32_t kBurgerH    = 16;  // glyph box
constexpr int32_t kBurgerBarH = 1;   // bar thickness; thinner than espressopost's 3
constexpr int32_t kBurgerGap  = 4;   // centre-to-centre spacing
constexpr int32_t kBurgerRows = 3;
static_assert((kBurgerRows - 1) * kBurgerGap + kBurgerBarH <= kBurgerH,
              "hamburger bars overflow their glyph box");

constexpr int32_t kClockY = kMenuBtnY + kMenuBtnH + 3;
// Wide enough for kNoClock and no wider, so a long value can never grow into the
// readout's space.
constexpr int32_t kClockW = 64;

// --- Top band: threshold chips (right) --------------------------------------
// The arrow sits at a fixed offset from the right edge and the number is centred
// in the band beside it. Formatting both into one right-aligned label — the
// obvious way — makes the arrow slide horizontally as the number's width changes,
// so the two chips visibly fail to line up with each other.
constexpr int32_t kChipNumW   = 46;  // fits "-40.0" and "257.0"
constexpr int32_t kChipArrowW = 14;
constexpr int32_t kChipArrowX = kScreenW - kEdgePad - kChipNumW - kChipArrowW;
constexpr int32_t kChipNumX   = kChipArrowX + kChipArrowW;
constexpr int32_t kChipY      = kTopPad + 2;
constexpr int32_t kChipSpacing = kFont14H + 2;
static_assert(kChipArrowX > kScreenW / 2, "threshold chips reach past the centre");

// The readout centres in the gap between the two side columns rather than on the
// screen. Nine pixels off true centre, which is invisible — but it means even the
// widest value the sensor can produce (-40.0 °F) cannot reach under the menu pill
// or the chips, which a screen-centred label would once the pill grew to hold a
// word as well as a glyph.
constexpr int32_t kReadoutCx = (kEdgePad + kMenuBtnW + kChipArrowX) / 2;
constexpr int32_t kReadoutDx = kReadoutCx - kScreenW / 2;

// --- Status strip: name on the first row, counters on the second -------------
constexpr int32_t kStatusW = kScreenW - 2 * kEdgePad;
constexpr int32_t kStatusRow2Y = kStatusY + kFont14H;

// Cloud sync lands in M8; until then there is no high-water mark to report and
// this stands in for it. Deliberately not "0" — zero uploaded is a claim that the
// uploader exists and has sent nothing, which would be a bug report rather than
// the truth, which is that there is no uploader yet.
constexpr const char* kNotUploaded = "--";

// --- Chart ------------------------------------------------------------------
constexpr uint32_t kChartHDiv = 4;
constexpr uint32_t kChartVDiv = 6;
constexpr int32_t  kTraceWidth = 2;
constexpr int32_t  kThresholdWidth = 1;
constexpr int32_t  kThresholdDash = 4;
constexpr int32_t  kThresholdGap  = 4;

// The y range always covers the band as well as the data, so the threshold lines
// stay on screen even when the reading is nowhere near them.
constexpr int32_t kRangeMarginC100 = 50;   // 0.5 °C of headroom above and below
constexpr int32_t kRangeMinSpanC100 = 200; // 2 °C — a dead-flat trace shouldn't
                                           // fill the plot and read as noise

// Below this the system clock has never been set (see storage.hpp on epoch_s).
// Duplicated from recorder.cpp deliberately for now; when M8's cloud component
// lands it owns SNTP and both of these collapse into one predicate there.
constexpr std::time_t kEpochValidFrom = 1'700'000'000;

constexpr uint32_t kSecPerMin  = 60;
constexpr uint32_t kSecPerHour = 3600;

lv_obj_t* s_value_lbl   = nullptr;
lv_obj_t* s_max_num     = nullptr;
lv_obj_t* s_min_num     = nullptr;
lv_obj_t* s_chart       = nullptr;
lv_obj_t* s_max_line    = nullptr;
lv_obj_t* s_min_line    = nullptr;
lv_obj_t* s_axis_left   = nullptr;
lv_obj_t* s_axis_mid    = nullptr;
lv_obj_t* s_name_lbl    = nullptr;
lv_obj_t* s_counter_lbl = nullptr;
lv_obj_t* s_clock_lbl   = nullptr;
lv_obj_t* s_menu_btn    = nullptr;
lv_obj_t* s_menu_glyph  = nullptr;
lv_obj_t* s_menu_lbl    = nullptr;
lv_chart_series_t* s_series = nullptr;

// The painter reads this, so the glyph tints with the rest of the pill.
bool s_menu_pressed = false;

// Threshold lines are positioned by moving the object, not by rewriting its
// points — a horizontal line at local y=0 keeps lv_line's self-sizing out of it.
// Spans the plot area, not the object, so it stops at the chart's inner border.
lv_point_precise_t s_line_pts[2] = {{0, 0}, {kPlotW, 0}};

// Scratch for the chart rebuild. File scope rather than stack: 480 points is
// nearly 3 KB across both, and the LVGL task's stack has better uses. Only ever
// touched from refresh(), which runs on that one task.
int16_t s_hist[settings::kGraphPointsMax];
int32_t s_plot[settings::kGraphPointsMax];

// Redraw guards — the chart is rebuilt only when something it draws from moved.
// The unit is deliberately not among them: the chart plots hundredths of a degree
// and carries no numeric axis labels, so switching °C/°F changes nothing in it.
uint32_t s_seen_generation = UINT32_MAX;
uint16_t s_seen_points = 0;
int16_t  s_seen_min = 0;
int16_t  s_seen_max = 0;

bool s_inited = false;

void refresh(lv_timer_t*);

// --- helpers ---------------------------------------------------------------

lv_color_t reading_color(const sensor::Reading& r, const alert::Thresholds& t) {
  if (!r.valid) return kColorFault;
  if (r.temp_c100 > t.max_c100) return kColorHigh;
  if (r.temp_c100 < t.min_c100) return kColorLow;
  return kColorText;
}

// Map a c100 value onto a pixel row inside the plot, y growing downwards.
int32_t y_for(int32_t v, int32_t lo, int32_t hi) {
  if (hi <= lo) return kPlotH / 2;
  const int32_t clamped = std::clamp(v, lo, hi);
  return (kPlotH - 1) - ((clamped - lo) * (kPlotH - 1)) / (hi - lo);
}

// Compact "3h" / "45m" / "90s", because the captions share a 320 px line.
void format_span(char* out, size_t n, uint32_t seconds) {
  if (seconds >= kSecPerHour) {
    std::snprintf(out, n, "-%luh", static_cast<unsigned long>(seconds / kSecPerHour));
  } else if (seconds >= kSecPerMin) {
    std::snprintf(out, n, "-%lum", static_cast<unsigned long>(seconds / kSecPerMin));
  } else {
    std::snprintf(out, n, "-%lus", static_cast<unsigned long>(seconds));
  }
}

// Local time, or an honest admission. Showing a 1970 timestamp would be worse
// than saying the clock isn't set — one is a bug report, the other is a fact.
void format_clock(char* out, size_t n) {
  const std::time_t now = std::time(nullptr);
  if (now < kEpochValidFrom) {
    std::snprintf(out, n, "%s", kNoClock);
    return;
  }
  std::tm tm = {};
  localtime_r(&now, &tm);
  std::snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// --- chart -----------------------------------------------------------------

void rebuild_chart(const settings::Settings& s, const alert::Thresholds& t) {
  const size_t points = s.graph_points;
  const size_t n = recorder::history(s_hist, points);

  // Range from the samples and the band together. Starting from the band means a
  // log with no valid readings still produces a sane axis.
  int32_t lo = std::min(t.min_c100, t.max_c100);
  int32_t hi = std::max(t.min_c100, t.max_c100);
  for (size_t i = 0; i < n; ++i) {
    if (s_hist[i] == recorder::kInvalidTempC100) continue;
    lo = std::min<int32_t>(lo, s_hist[i]);
    hi = std::max<int32_t>(hi, s_hist[i]);
  }
  if (hi - lo < kRangeMinSpanC100) {
    const int32_t mid = (lo + hi) / 2;
    lo = mid - kRangeMinSpanC100 / 2;
    hi = mid + kRangeMinSpanC100 / 2;
  }
  lo -= kRangeMarginC100;
  hi += kRangeMarginC100;

  // Newest sample at the right edge: pad the left with gaps until the ring has
  // filled, rather than stretching a short history across the whole plot and
  // implying a window that hasn't elapsed yet.
  const size_t pad = points - n;
  for (size_t i = 0; i < pad; ++i) s_plot[i] = LV_CHART_POINT_NONE;
  for (size_t i = 0; i < n; ++i) {
    s_plot[pad + i] = s_hist[i] == recorder::kInvalidTempC100
                          ? LV_CHART_POINT_NONE
                          : static_cast<int32_t>(s_hist[i]);
  }

  lv_chart_set_point_count(s_chart, static_cast<uint32_t>(points));
  lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
  lv_chart_set_series_values(s_chart, s_series, s_plot, points);

  lv_obj_set_pos(s_max_line, 0, y_for(t.max_c100, lo, hi));
  lv_obj_set_pos(s_min_line, 0, y_for(t.min_c100, lo, hi));

  char text[kTextMax];
  const uint32_t window_s = static_cast<uint32_t>(points) * s.interval_s;
  format_span(text, sizeof(text), window_s);
  lv_label_set_text(s_axis_left, text);
  format_span(text, sizeof(text), window_s / 2);
  lv_label_set_text(s_axis_mid, text);
}

// --- refresh ---------------------------------------------------------------

// Runs in the LVGL task, which already holds the LVGL lock.
//
// Formatting goes through std::snprintf, never lv_label_set_text_fmt. LVGL's
// built-in printf compiles out `case 'f'` unless LV_USE_FLOAT is on, and the
// unsupported conversion then falls through WITHOUT consuming its double — every
// later argument is read from the wrong offset and a trailing %s dies in
// lv_strnlen. Silent, and it only fires on format strings carrying a float.
void refresh(lv_timer_t*) {
  const settings::Settings s = settings::get();
  const alert::Thresholds t = settings::thresholds();
  const sensor::Reading r = sensor::latest();
  char text[kTextMax];

  // --- the number
  if (r.valid) {
    std::snprintf(text, sizeof(text), "%.1f%s",
                  static_cast<double>(settings::display_value(r.temp_c100)),
                  settings::unit_suffix());
  } else {
    std::snprintf(text, sizeof(text), "%s", kNoValue);
  }
  lv_label_set_text(s_value_lbl, text);
  lv_obj_set_style_text_color(s_value_lbl, reading_color(r, t), LV_PART_MAIN);

  // --- threshold chips. Only the numbers change; the arrows are fixed.
  std::snprintf(text, sizeof(text), "%.1f",
                static_cast<double>(settings::display_value(t.max_c100)));
  lv_label_set_text(s_max_num, text);
  std::snprintf(text, sizeof(text), "%.1f",
                static_cast<double>(settings::display_value(t.min_c100)));
  lv_label_set_text(s_min_num, text);

  // --- clock, under the menu glyph
  char clock[kClockTextMax];
  format_clock(clock, sizeof(clock));
  lv_label_set_text(s_clock_lbl, clock);

  // --- status strip
  lv_label_set_text(s_name_lbl, s.name);

  // Three different questions, which is why one number was not enough:
  //   live     — appended since boot; frozen means logging has stopped
  //   total    — ever written, the absolute index cloud sync counts against
  //   uploaded — how far the Sheet has caught up (M8)
  std::snprintf(text, sizeof(text), "#recs live %lu  total %lu  uploaded %s",
                static_cast<unsigned long>(recorder::written()),
                static_cast<unsigned long>(recorder::total()), kNotUploaded);
  lv_label_set_text(s_counter_lbl, text);

  // --- the graph, only when something it draws from has actually changed
  const uint32_t gen = recorder::generation();
  if (gen != s_seen_generation || s.graph_points != s_seen_points ||
      t.min_c100 != s_seen_min || t.max_c100 != s_seen_max) {
    s_seen_generation = gen;
    s_seen_points = s.graph_points;
    s_seen_min = t.min_c100;
    s_seen_max = t.max_c100;
    rebuild_chart(s, t);
  }
}

// --- events ----------------------------------------------------------------

// Tapping the readout swaps °C/°F. Display-only by construction: every stored
// value is hundredths of a degree Celsius and settings::display_value() is the
// only thing that converts, so the thresholds and the graph follow along without
// any of them being rewritten. Persisted, so the choice survives a reboot.
void value_clicked(lv_event_t*) {
  const settings::Settings s = settings::get();
  const settings::TempUnit next = s.unit == settings::TempUnit::Celsius
                                      ? settings::TempUnit::Fahrenheit
                                      : settings::TempUnit::Celsius;
  settings::set_unit(next);
  // Repaint now instead of waiting up to kRefreshMs — a tap that takes a quarter
  // of a second to answer feels like it was missed.
  refresh(nullptr);
  ESP_LOGI(kTag, "unit -> %s", settings::unit_suffix());
}

void draw_burger(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;
  lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const int32_t cx = coords.x1 + lv_area_get_width(&coords) / 2;
  const int32_t cy = coords.y1 + lv_area_get_height(&coords) / 2;

  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = s_menu_pressed ? kColorOk : kColorMenu;
  d.bg_opa = LV_OPA_COVER;
  d.radius = kBurgerBarH / 2;

  const int32_t half_w = kBurgerW / 2;
  const int32_t half_h = kBurgerBarH / 2;
  for (int32_t row = -(kBurgerRows / 2); row <= kBurgerRows / 2; ++row) {
    const int32_t yc = cy + row * kBurgerGap;
    lv_area_t a = {cx - half_w, yc - half_h, cx + half_w, yc + half_h};
    lv_draw_rect(layer, &d, &a);
  }
}

// Outline, label and painted glyph tint together, so the whole pill responds
// rather than one part of it.
void set_menu_tint(lv_color_t c) {
  lv_obj_set_style_border_color(s_menu_btn, c, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_menu_lbl, c, LV_PART_MAIN);
  lv_obj_invalidate(s_menu_glyph);
}

void menu_pressed(lv_event_t*) {
  s_menu_pressed = true;
  set_menu_tint(kColorOk);
  // M6 replaces this with the fade into the menu hub. Until then the tap is
  // acknowledged visually as well as on serial, so the pill doubles as the touch
  // check that the M2 probe screen used to be.
  ESP_LOGI(kTag, "MENU tapped (no menu yet — M6)");
}

void menu_released(lv_event_t*) {
  s_menu_pressed = false;
  set_menu_tint(kColorMenu);
}

// --- construction ----------------------------------------------------------

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color,
                     const char* initial) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
  lv_label_set_text(l, initial);
  return l;
}

// A fixed-width, centre-aligned label — the shape both threshold numbers and the
// counter need so their content can change width without moving anything else.
lv_obj_t* make_boxed_label(lv_obj_t* parent, lv_color_t color, int32_t w,
                           lv_text_align_t align) {
  lv_obj_t* l = make_label(parent, &lv_font_montserrat_14, color, "");
  lv_obj_set_width(l, w);
  lv_obj_set_style_text_align(l, align, LV_PART_MAIN);
  return l;
}

void build_chart(lv_obj_t* scr) {
  s_chart = lv_chart_create(scr);
  lv_obj_set_size(s_chart, kChartW, kChartH);
  lv_obj_set_pos(s_chart, kChartX, kChartY);
  lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(s_chart, kChartHDiv, kChartVDiv);

  // Zero padding so the object's area IS the plot area. y_for() and the threshold
  // lines both assume that; a default pad would shift every line by a few pixels
  // and the error would look like a calibration problem rather than a layout one.
  lv_obj_set_style_pad_all(s_chart, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_chart, kColorPanel, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_chart, kColorGrid, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_chart, kChartBorderPx, LV_PART_MAIN);
  lv_obj_set_style_line_color(s_chart, kColorGrid, LV_PART_ITEMS);
  // No dots on the trace: at a couple of pixels per sample they merge into a
  // thicker, blurrier line rather than reading as data points.
  lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);
  lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);

  s_series = lv_chart_add_series(s_chart, kColorNormal, LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_set_style_line_width(s_chart, kTraceWidth, LV_PART_ITEMS);

  for (lv_obj_t** slot : {&s_max_line, &s_min_line}) {
    *slot = lv_line_create(s_chart);
    lv_line_set_points(*slot, s_line_pts, 2);
    lv_obj_set_style_line_width(*slot, kThresholdWidth, LV_PART_MAIN);
    lv_obj_set_style_line_dash_width(*slot, kThresholdDash, LV_PART_MAIN);
    lv_obj_set_style_line_dash_gap(*slot, kThresholdGap, LV_PART_MAIN);
  }
  lv_obj_set_style_line_color(s_max_line, kColorHigh, LV_PART_MAIN);
  lv_obj_set_style_line_color(s_min_line, kColorLow, LV_PART_MAIN);
}

// The outlined capsule: hamburger glyph + "Menu" in a centred flex row, exactly
// the shape espressopost uses. The glyph is its own transparent widget with a
// painter attached, so the row's flex layout measures it like any other child.
void build_menu_pill(lv_obj_t* scr) {
  s_menu_btn = lv_button_create(scr);
  lv_obj_set_size(s_menu_btn, kMenuBtnW, kMenuBtnH);
  lv_obj_set_style_radius(s_menu_btn, kMenuBtnH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_menu_btn, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_menu_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_menu_btn, kColorMenu, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_menu_btn, kMenuBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_menu_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_ext_click_area(s_menu_btn, kMenuBtnExtClick);
  lv_obj_set_pos(s_menu_btn, kEdgePad, kMenuBtnY);
  lv_obj_add_event_cb(s_menu_btn, menu_pressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s_menu_btn, menu_released, LV_EVENT_RELEASED, nullptr);

  lv_obj_t* row = lv_obj_create(s_menu_btn);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(row, kMenuRowGap, LV_PART_MAIN);
  lv_obj_set_layout(row, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  s_menu_glyph = lv_obj_create(row);
  lv_obj_set_size(s_menu_glyph, kBurgerW, kBurgerH);
  lv_obj_set_style_bg_opa(s_menu_glyph, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_menu_glyph, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_menu_glyph, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_menu_glyph, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(s_menu_glyph, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s_menu_glyph, draw_burger, LV_EVENT_DRAW_MAIN, nullptr);

  s_menu_lbl = make_label(row, &lv_font_montserrat_14, kColorMenu, "Menu");

  lv_obj_center(row);
}

void build_top_band(lv_obj_t* scr) {
  build_menu_pill(scr);

  // --- clock, directly under the pill
  s_clock_lbl = make_boxed_label(scr, kColorMuted, kClockW, LV_TEXT_ALIGN_LEFT);
  lv_label_set_long_mode(s_clock_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_clock_lbl, kEdgePad, kClockY);

  // --- the readout, centred in the gap between the side columns
  s_value_lbl = make_label(scr, &lv_font_montserrat_48, kColorText, kNoValue);
  lv_obj_align(s_value_lbl, LV_ALIGN_TOP_MID, kReadoutDx, kReadoutY);
  lv_obj_add_flag(s_value_lbl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_value_lbl, value_clicked, LV_EVENT_CLICKED, nullptr);

  // --- threshold chips, right. Arrows fixed, numbers centred beside them.
  lv_obj_t* max_arrow =
      make_label(scr, &lv_font_montserrat_14, kColorHigh, LV_SYMBOL_UP);
  lv_obj_set_pos(max_arrow, kChipArrowX, kChipY);
  s_max_num = make_boxed_label(scr, kColorHigh, kChipNumW, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_max_num, kChipNumX, kChipY);

  lv_obj_t* min_arrow =
      make_label(scr, &lv_font_montserrat_14, kColorLow, LV_SYMBOL_DOWN);
  lv_obj_set_pos(min_arrow, kChipArrowX, kChipY + kChipSpacing);
  s_min_num = make_boxed_label(scr, kColorLow, kChipNumW, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_min_num, kChipNumX, kChipY + kChipSpacing);
}

void build_screen() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, kColorBg, LV_PART_MAIN);
  lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  build_top_band(scr);
  build_chart(scr);

  // --- time captions
  s_axis_left = make_label(scr, &lv_font_montserrat_14, kColorMuted, "");
  lv_obj_set_pos(s_axis_left, kEdgePad, kAxisY);
  s_axis_mid = make_label(scr, &lv_font_montserrat_14, kColorMuted, "");
  lv_obj_align(s_axis_mid, LV_ALIGN_TOP_MID, 0, kAxisY);
  lv_obj_t* axis_right = make_label(scr, &lv_font_montserrat_14, kColorMuted, "now");
  lv_obj_align(axis_right, LV_ALIGN_TOP_RIGHT, -kEdgePad, kAxisY);

  // --- status strip, two rows. Both ellipsise rather than wrap: each row is one
  // line high and a wrapped line would grow downwards off the panel.
  s_name_lbl = make_label(scr, &lv_font_montserrat_14, kColorSubText, "");
  lv_obj_set_width(s_name_lbl, kStatusW);
  lv_label_set_long_mode(s_name_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_name_lbl, kEdgePad, kStatusY);

  s_counter_lbl = make_label(scr, &lv_font_montserrat_14, kColorMuted, "");
  lv_obj_set_width(s_counter_lbl, kStatusW);
  lv_label_set_long_mode(s_counter_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_counter_lbl, kEdgePad, kStatusRow2Y);
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;
  if (display::lvgl_display() == nullptr) {
    ESP_LOGE(kTag, "display is not up — call display::init() first");
    return ESP_ERR_INVALID_STATE;
  }
  if (!display::lock()) {
    ESP_LOGE(kTag, "could not take the LVGL lock");
    return ESP_ERR_TIMEOUT;
  }

  // The row budget is built from these at compile time, so a font swap or an LVGL
  // update that changes a line height would silently crowd the rows rather than
  // fail. Cheap to check, and the symptom (captions touching the status strip) is
  // otherwise easy to mistake for a stray widget.
  if (lv_font_montserrat_14.line_height != kFont14H ||
      lv_font_montserrat_48.line_height != kFont48H) {
    ESP_LOGW(kTag, "font line heights are %d/%d, layout budgeted %ld/%ld — rows may crowd",
             lv_font_montserrat_14.line_height, lv_font_montserrat_48.line_height,
             static_cast<long>(kFont14H), static_cast<long>(kFont48H));
  }

  build_screen();
  refresh(nullptr);  // paint real values before the first frame, not placeholders
  lv_timer_create(refresh, kRefreshMs, nullptr);

  display::unlock();
  s_inited = true;
  ESP_LOGI(kTag, "main screen up (%ldx%ld, chart %ldx%ld at y=%ld)",
           static_cast<long>(kScreenW), static_cast<long>(kScreenH),
           static_cast<long>(kChartW), static_cast<long>(kChartH),
           static_cast<long>(kChartY));
  return ESP_OK;
}

}  // namespace thermopost::ui
