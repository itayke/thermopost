#include "ui.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "alert_logic.hpp"
#include "cloud.hpp"
#include "display.hpp"
#include "esp_log.h"
#include "recorder.hpp"
#include "sensor.hpp"
#include "settings.hpp"
#include "ui_chrome.hpp"
#include "ui_menu.hpp"
#include "ui_nav.hpp"
#include "ui_settings.hpp"
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

// Short on purpose. "clock not set" is the fuller phrase, and it is what the menu
// says because it has the width — but at Montserrat 14 it reaches under a centred
// 48 px readout, and a status line that collides with the number beside it is worse
// than a terser one.
constexpr const char* kNoClock = "no clock";

// --- Top band: clock under the nav pill (left) -------------------------------
// The pill itself is shared chrome now (ui_chrome.hpp) — every screen puts one in
// this corner, and a second copy of its geometry here is how the two would drift.
//
// Derived from the pill's own origin and height, never from kTopPad again: the
// clock has to follow the pill when the pill moves, and recomputing the same sum
// is exactly how the two came apart the first time.
constexpr int32_t kClockGap = 3;
constexpr int32_t kClockY = kNavPillY + kNavPillH + kClockGap;
// Wide enough for kNoClock and no wider, so a long value can never grow into the
// readout's space.
constexpr int32_t kClockW = 64;
static_assert(kClockY + kFont14H <= kTopPad + kReadoutH,
              "the clock row overflows the top band");

// --- Top band: threshold chips (right) --------------------------------------
// The arrow sits at a fixed offset from the right edge and the number is centred
// in the band beside it. Formatting both into one right-aligned label — the
// obvious way — makes the arrow slide horizontally as the number's width changes,
// so the two chips visibly fail to line up with each other.
constexpr int32_t kChipNumW   = 46;  // fits "-40.0" and "257.0"
constexpr int32_t kChipArrowW = 14;
constexpr int32_t kChipArrowX = kScreenW - kEdgePad - kChipNumW - kChipArrowW;
constexpr int32_t kChipNumX   = kChipArrowX + kChipArrowW;
constexpr int32_t kChipY      = kTopPad + kTopRowNudge;
// Leading between the two chip rows — a line height alone sets them touching.
constexpr int32_t kChipLeading = 2;
constexpr int32_t kChipSpacing = kFont14H + kChipLeading;
static_assert(kChipArrowX > kScreenW / 2, "threshold chips reach past the centre");

// The readout centres in the gap between the two side columns rather than on the
// screen. Nine pixels off true centre, which is invisible — but it means even the
// widest value the sensor can produce (-40.0 °F) cannot reach under the menu pill
// or the chips, which a screen-centred label would once the pill grew to hold a
// word as well as a glyph.
constexpr int32_t kReadoutCx = (kEdgePad + kNavPillW + kChipArrowX) / 2;
constexpr int32_t kReadoutDx = kReadoutCx - kScreenW / 2;

// The unit rides after the digits at the small text size, in a flex row so the pair
// stays centred as a group — centring the number alone and hanging the unit off its
// right would shift the whole thing visibly off-centre by half the unit's width.
// The degree glyph carries its own left sidebearing, so the gap is nearly nothing.
constexpr int32_t kReadoutUnitGap = 2;

// --- Status strip: name on the first row, counters on the second -------------
constexpr int32_t kStatusW = kScreenW - 2 * kEdgePad;
constexpr int32_t kStatusRow2Y = kStatusY + kFont14H;

// Shown in place of the high-water mark while no Sheet endpoint is configured.
// Deliberately not "0" — zero uploaded is a claim that an uploader is running and
// has sent nothing, which reads as a fault. The truth is that there is nowhere to
// send to, and a dash says that without inviting a bug report.
constexpr const char* kNotUploaded = "--";

// Ten digits of uint32_t plus the terminator. The counter is an absolute record
// index and only ever grows, so it is worth sizing for the whole range rather
// than for what a bench session reaches.
constexpr size_t kUploadedTextMax = 11;

// --- Chart ------------------------------------------------------------------
constexpr uint32_t kChartHDiv = 4;
constexpr uint32_t kChartVDiv = 6;
// Zero, so the object's area is exactly the plot area — see build_chart().
constexpr int32_t  kChartPad = 0;
constexpr int32_t  kTraceWidth = 2;
// The envelope is thinner than the mean trace so the mean stays the primary line.
// LVGL styles line width per chart, not per series, so both come from LV_PART_ITEMS
// and the envelope cannot actually be given its own width — this exists to be the
// one place that changes if a later LVGL grows per-series styling.
constexpr int32_t  kEnvelopeWidth = kTraceWidth;
constexpr int32_t  kThresholdWidth = 1;
constexpr int32_t  kThresholdDash = 4;
constexpr int32_t  kThresholdGap  = 4;
constexpr int32_t  kGridWidth = 1;
// Zero hides the per-sample dots. At a couple of pixels per point they merge into a
// thicker, blurrier line rather than reading as data.
constexpr int32_t  kPointDiam = 0;

// The y range always covers the band as well as the data, so the threshold lines
// stay on screen even when the reading is nowhere near them.
constexpr int32_t kRangeMarginC100 = 50;   // 0.5 °C of headroom above and below
constexpr int32_t kRangeMinSpanC100 = 200; // 2 °C — a dead-flat trace shouldn't
                                           // fill the plot and read as noise

// Just the hour and minute: the column under the pill is 64 px wide, and the date
// is on the menu screen, which has the room for it.
constexpr const char* kClockFmt = "%H:%M";

constexpr uint32_t kSecPerMin  = 60;
constexpr uint32_t kSecPerHour = 3600;

lv_obj_t* s_readout_row = nullptr;
lv_obj_t* s_value_lbl   = nullptr;
lv_obj_t* s_unit_lbl    = nullptr;
lv_obj_t* s_max_arrow   = nullptr;
lv_obj_t* s_max_num     = nullptr;
lv_obj_t* s_min_arrow   = nullptr;
lv_obj_t* s_min_num     = nullptr;
lv_obj_t* s_chart       = nullptr;
lv_obj_t* s_max_line    = nullptr;
lv_obj_t* s_min_line    = nullptr;
lv_obj_t* s_axis_left   = nullptr;
lv_obj_t* s_axis_mid    = nullptr;
lv_obj_t* s_axis_right  = nullptr;
lv_obj_t* s_name_lbl    = nullptr;
lv_obj_t* s_counter_lbl = nullptr;
lv_obj_t* s_clock_lbl   = nullptr;
lv_obj_t* s_group       = nullptr;
NavPill   s_nav         = {};
// Mean plus the min/max envelope. Three series rather than one because a record is
// an aggregate of sixty readings — plotting only the mean would smooth away the
// short excursions the device exists to catch.
lv_chart_series_t* s_mean_series = nullptr;
lv_chart_series_t* s_min_series = nullptr;
lv_chart_series_t* s_max_series = nullptr;

// Threshold lines are positioned by moving the object, not by rewriting its
// points — a horizontal line at local y=0 keeps lv_line's self-sizing out of it.
// Spans the plot area, not the object, so it stops at the chart's inner border.
lv_point_precise_t s_line_pts[2] = {{0, 0}, {kPlotW, 0}};

// Scratch for the chart rebuild. File scope rather than stack: 480 points across
// four arrays is nearly 9 KB, and the LVGL task's stack has better uses. Only ever
// touched from rebuild_chart(), which runs on that one task.
recorder::Sample s_hist[settings::kGraphPointsMax];
int32_t s_plot_mean[settings::kGraphPointsMax];
int32_t s_plot_min[settings::kGraphPointsMax];
int32_t s_plot_max[settings::kGraphPointsMax];

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

// --- chart -----------------------------------------------------------------

void rebuild_chart(const settings::Settings& s, const alert::Thresholds& t) {
  const size_t points = s.graph_points;
  const size_t n = recorder::history(s_hist, points);

  // Range from the extremes and the band together — the envelope, not just the
  // mean, or a spike would be drawn clipped against the frame. Starting from the
  // band means a log with no valid readings still produces a sane axis.
  int32_t lo = std::min(t.min_c100, t.max_c100);
  int32_t hi = std::max(t.min_c100, t.max_c100);
  for (size_t i = 0; i < n; ++i) {
    if (s_hist[i].mean_c100 == recorder::kInvalidTempC100) continue;
    lo = std::min<int32_t>(lo, s_hist[i].min_c100);
    hi = std::max<int32_t>(hi, s_hist[i].max_c100);
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
  for (size_t i = 0; i < pad; ++i) {
    s_plot_mean[i] = LV_CHART_POINT_NONE;
    s_plot_min[i]  = LV_CHART_POINT_NONE;
    s_plot_max[i]  = LV_CHART_POINT_NONE;
  }
  for (size_t i = 0; i < n; ++i) {
    const recorder::Sample& h = s_hist[i];
    const bool gap = h.mean_c100 == recorder::kInvalidTempC100;
    s_plot_mean[pad + i] = gap ? LV_CHART_POINT_NONE : h.mean_c100;
    s_plot_min[pad + i]  = gap ? LV_CHART_POINT_NONE : h.min_c100;
    s_plot_max[pad + i]  = gap ? LV_CHART_POINT_NONE : h.max_c100;
  }

  lv_chart_set_point_count(s_chart, static_cast<uint32_t>(points));
  lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
  // Exactly `points` values per series, which is not merely tidy:
  // lv_chart_set_series_values is a loop over lv_chart_set_next_value, and that
  // writes at the series' circular start_point and advances it. Writing the point
  // count leaves start_point back where it began; any other count silently rotates
  // the whole trace instead of erroring.
  lv_chart_set_series_values(s_chart, s_min_series,  s_plot_min,  points);
  lv_chart_set_series_values(s_chart, s_max_series,  s_plot_max,  points);
  lv_chart_set_series_values(s_chart, s_mean_series, s_plot_mean, points);

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
void refresh(lv_timer_t* timer) {
  // Skipped while a menu page is up. Not an optimisation for its own sake: the
  // chart rebuild walks 480 samples across four arrays, and doing that four times a
  // second for a screen nobody can see is work taken from the LVGL task that is
  // animating the transition. nav::register_screen's on_show repaints on the way
  // back in, so nothing is stale when it returns.
  //
  // The nullptr call is the direct one — from init() and from that on_show hook —
  // and it always paints.
  if (timer != nullptr && !nav::visible(nav::Screen::Main)) return;

  const settings::Settings s = settings::get();
  const alert::Thresholds t = settings::thresholds();
  const sensor::Reading r = sensor::latest();
  char text[kTextMax];

  // --- the number, with the unit trailing it at the small size
  if (r.valid) {
    std::snprintf(text, sizeof(text), "%.1f",
                  static_cast<double>(settings::display_value(r.temp_c100)));
  } else {
    std::snprintf(text, sizeof(text), "%s", kNoValue);
  }
  lv_label_set_text(s_value_lbl, text);
  // The unit shows even with no reading: it is a property of the display, not of
  // the value, and hiding it would make the tap-to-toggle look like it did nothing
  // when the sensor is unplugged.
  lv_label_set_text(s_unit_lbl, settings::unit_suffix());

  const lv_color_t tint = reading_color(r, t);
  lv_obj_set_style_text_color(s_value_lbl, tint, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_unit_lbl, tint, LV_PART_MAIN);

  // --- threshold chips. Only the numbers change; the arrows are fixed.
  std::snprintf(text, sizeof(text), "%.1f",
                static_cast<double>(settings::display_value(t.max_c100)));
  lv_label_set_text(s_max_num, text);
  std::snprintf(text, sizeof(text), "%.1f",
                static_cast<double>(settings::display_value(t.min_c100)));
  lv_label_set_text(s_min_num, text);

  // --- clock, under the menu glyph
  char clock[kClockTextMax];
  if (!format_clock(clock, sizeof(clock), kClockFmt)) {
    std::snprintf(clock, sizeof(clock), "%s", kNoClock);
  }
  lv_label_set_text(s_clock_lbl, clock);

  // --- status strip
  lv_label_set_text(s_name_lbl, s.name);

  // Three different questions, which is why one number was not enough:
  //   live     — appended since boot; frozen means logging has stopped
  //   total    — ever written, the absolute index cloud sync counts against
  //   uploaded — how far the Sheet has caught up (M8)
  const cloud::Status net = cloud::status();
  char uploaded[kUploadedTextMax];
  if (net.endpoint_set) {
    std::snprintf(uploaded, sizeof(uploaded), "%lu",
                  static_cast<unsigned long>(net.uploaded));
  } else {
    std::snprintf(uploaded, sizeof(uploaded), "%s", kNotUploaded);
  }
  std::snprintf(text, sizeof(text), "#recs live %lu  total %lu  uploaded %s",
                static_cast<unsigned long>(recorder::written()),
                static_cast<unsigned long>(recorder::total()), uploaded);
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

void menu_clicked(lv_event_t*) { nav::go(nav::Screen::Menu); }

// --- construction ----------------------------------------------------------

// Every boxed label on this screen is Montserrat 14 — the clock and the two
// threshold numbers — so the font is bound once here rather than at each call.
lv_obj_t* make_chip_label(lv_obj_t* parent, lv_color_t color, int32_t w,
                          lv_text_align_t align) {
  return make_boxed_label(parent, &lv_font_montserrat_14, color, w, align);
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
  lv_obj_set_style_pad_all(s_chart, kChartPad, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_chart, kColorPanel, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_chart, kColorGrid, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_chart, kChartBorderPx, LV_PART_MAIN);

  // Division lines are drawn from LV_PART_MAIN, not LV_PART_ITEMS — draw_div_lines()
  // inits its descriptor from MAIN. Styling them on ITEMS (the obvious guess, since
  // that is where the trace lives) is silently ineffective, and because MAIN's
  // default line_width is 0 the grid then does not draw at all.
  lv_obj_set_style_line_color(s_chart, kColorGrid, LV_PART_MAIN);
  lv_obj_set_style_line_width(s_chart, kGridWidth, LV_PART_MAIN);

  lv_obj_set_style_size(s_chart, kPointDiam, kPointDiam, LV_PART_INDICATOR);
  lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);

  // A trace takes its colour from its series, not from a style: draw_series_line()
  // overwrites the descriptor's colour with ser->color. Width, by contrast, is read
  // from LV_PART_ITEMS and is therefore shared by every series — so the two are
  // configured in different places, and the envelope cannot be drawn thinner than
  // the mean however much it would like to be.
  //
  // Added envelope-first so the mean is registered last and draws on top of it.
  s_min_series  = lv_chart_add_series(s_chart, kColorEnvelope, LV_CHART_AXIS_PRIMARY_Y);
  s_max_series  = lv_chart_add_series(s_chart, kColorEnvelope, LV_CHART_AXIS_PRIMARY_Y);
  s_mean_series = lv_chart_add_series(s_chart, kColorNormal, LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_set_style_line_width(s_chart, kEnvelopeWidth, LV_PART_ITEMS);

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

void build_top_band(lv_obj_t* scr) {
  build_nav_pill(scr, &s_nav, NavGlyph::Burger, "Menu", menu_clicked);

  // --- clock, directly under the pill
  s_clock_lbl = make_chip_label(scr, kColorMuted, kClockW, LV_TEXT_ALIGN_LEFT);
  lv_label_set_long_mode(s_clock_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_clock_lbl, kEdgePad, kClockY);

  // --- the readout, centred as a group in the gap between the side columns
  s_readout_row = lv_obj_create(scr);
  lv_obj_set_size(s_readout_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(s_readout_row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_readout_row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_readout_row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(s_readout_row, kReadoutUnitGap, LV_PART_MAIN);
  lv_obj_set_layout(s_readout_row, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_readout_row, LV_FLEX_FLOW_ROW);
  // Cross-axis START, i.e. both boxes hang from the same top edge. Deliberately not
  // END: bottom-aligning two different sizes aligns their descender lines, not
  // their baselines, which drops the unit visibly below the digits it belongs to.
  lv_obj_set_flex_align(s_readout_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(s_readout_row, LV_OBJ_FLAG_SCROLLABLE);
  // The whole group is the tap target, not just the digits — the labels are not
  // clickable, so their events bubble here.
  lv_obj_add_flag(s_readout_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_readout_row, value_clicked, LV_EVENT_CLICKED, nullptr);

  s_value_lbl = make_label(s_readout_row, &lv_font_montserrat_48, kColorText, kNoValue);
  s_unit_lbl = make_label(s_readout_row, &lv_font_montserrat_14, kColorText, "");

  // Sit the unit on the digits' baseline, computed from the two fonts rather than
  // guessed: a font's baseline is line_height - base_line below its box top, so the
  // unit's box needs padding equal to the difference. Derived, so swapping either
  // font keeps them aligned instead of quietly drifting apart.
  const int32_t digits_baseline =
      lv_font_montserrat_48.line_height - lv_font_montserrat_48.base_line;
  const int32_t unit_baseline =
      lv_font_montserrat_14.line_height - lv_font_montserrat_14.base_line;
  lv_obj_set_style_pad_top(s_unit_lbl, digits_baseline - unit_baseline, LV_PART_MAIN);

  lv_obj_align(s_readout_row, LV_ALIGN_TOP_MID, kReadoutDx, kReadoutY);

  // --- threshold chips, right. Arrows fixed, numbers centred beside them.
  s_max_arrow = make_label(scr, &lv_font_montserrat_14, kColorHigh, LV_SYMBOL_UP);
  lv_obj_set_pos(s_max_arrow, kChipArrowX, kChipY);
  s_max_num = make_chip_label(scr, kColorHigh, kChipNumW, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_max_num, kChipNumX, kChipY);

  s_min_arrow = make_label(scr, &lv_font_montserrat_14, kColorLow, LV_SYMBOL_DOWN);
  lv_obj_set_pos(s_min_arrow, kChipArrowX, kChipY + kChipSpacing);
  s_min_num = make_chip_label(scr, kColorLow, kChipNumW, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_min_num, kChipNumX, kChipY + kChipSpacing);
}

void build_screen(lv_obj_t* scr) {
  s_group = build_group(scr);

  build_top_band(s_group);
  build_chart(s_group);

  // --- time captions
  s_axis_left = make_label(s_group, &lv_font_montserrat_14, kColorMuted, "");
  lv_obj_set_pos(s_axis_left, kEdgePad, kAxisY);
  s_axis_mid = make_label(s_group, &lv_font_montserrat_14, kColorMuted, "");
  lv_obj_align(s_axis_mid, LV_ALIGN_TOP_MID, 0, kAxisY);
  s_axis_right = make_label(s_group, &lv_font_montserrat_14, kColorMuted, "now");
  lv_obj_align(s_axis_right, LV_ALIGN_TOP_RIGHT, -kEdgePad, kAxisY);

  // --- status strip, two rows. Both ellipsise rather than wrap: each row is one
  // line high and a wrapped line would grow downwards off the panel.
  s_name_lbl = make_label(s_group, &lv_font_montserrat_14, kColorSubText, "");
  lv_obj_set_width(s_name_lbl, kStatusW);
  lv_label_set_long_mode(s_name_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_name_lbl, kEdgePad, kStatusY);

  s_counter_lbl = make_label(s_group, &lv_font_montserrat_14, kColorMuted, "");
  lv_obj_set_width(s_counter_lbl, kStatusW);
  lv_label_set_long_mode(s_counter_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_counter_lbl, kEdgePad, kStatusRow2Y);

  // The chart's threshold lines are its children, and LV_STYLE_OPA applies to an
  // object together with everything inside it, so the chart fades as one piece.
  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_nav.btn;
  fade[n++] = s_clock_lbl;
  fade[n++] = s_readout_row;
  fade[n++] = s_max_arrow;
  fade[n++] = s_max_num;
  fade[n++] = s_min_arrow;
  fade[n++] = s_min_num;
  fade[n++] = s_chart;
  fade[n++] = s_axis_left;
  fade[n++] = s_axis_mid;
  fade[n++] = s_axis_right;
  fade[n++] = s_name_lbl;
  fade[n++] = s_counter_lbl;
  // Repaint on the way back in rather than waiting up to kRefreshMs: arriving from
  // an editor that just changed a threshold, the old number would otherwise be
  // visible for a quarter of a second on the screen the edit was made for.
  nav::register_screen(nav::Screen::Main, s_group, fade, n, []() { refresh(nullptr); });
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
      lv_font_montserrat_24.line_height != kFont24H ||
      lv_font_montserrat_48.line_height != kFont48H) {
    ESP_LOGW(kTag, "font line heights are %d/%d/%d, layout budgeted %ld/%ld/%ld — rows may crowd",
             lv_font_montserrat_14.line_height, lv_font_montserrat_24.line_height,
             lv_font_montserrat_48.line_height, static_cast<long>(kFont14H),
             static_cast<long>(kFont24H), static_cast<long>(kFont48H));
  }

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, kColorBg, LV_PART_MAIN);
  lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Every screen is built up front and then hidden, rather than built on demand:
  // constructing a page inside a transition is what makes the transition stutter,
  // and the whole set is a few kilobytes of widgets against 8 MB of PSRAM.
  nav::init(scr);
  build_screen(scr);
  menu_screen::build(scr);
  settings_screen::build_all(scr);
  nav::start();

  refresh(nullptr);  // paint real values before the first frame, not placeholders
  lv_timer_create(refresh, kRefreshMs, nullptr);

  display::unlock();
  s_inited = true;
  ESP_LOGI(kTag, "screens up (%ldx%ld, chart %ldx%ld at y=%ld)",
           static_cast<long>(kScreenW), static_cast<long>(kScreenH),
           static_cast<long>(kChartW), static_cast<long>(kChartH),
           static_cast<long>(kChartY));
  return ESP_OK;
}

}  // namespace thermopost::ui
