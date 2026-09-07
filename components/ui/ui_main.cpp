#include "ui.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "alert.hpp"
#include "alert_logic.hpp"
#include "cloud.hpp"
#include "power.hpp"
#include "display.hpp"
#include "esp_log.h"
#include "recorder.hpp"
#include "sensor.hpp"
#include "settings.hpp"
#include "ui_chrome.hpp"
#include "ui_configure.hpp"
#include "ui_network.hpp"
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

// The battery meter and the charge bolt now share this row, so the clock's slot is
// narrower than the phrase "no clock" needs. Shaped like a time instead, which
// reads as "not set yet" in the same way the readout's "--.-" does, and costs no
// width at all. The menu screen still says it in words, where there is room.
constexpr const char* kNoClock = "--:--";

// --- Top band: clock under the nav pill (left) -------------------------------
// The pill itself is shared chrome now (ui_chrome.hpp) — every screen puts one in
// this corner, and a second copy of its geometry here is how the two would drift.
//
// Derived from the pill's own origin and height, never from kTopPad again: the
// clock has to follow the pill when the pill moves, and recomputing the same sum
// is exactly how the two came apart the first time.
constexpr int32_t kClockGap = 3;
constexpr int32_t kClockRowY = kNavPillBottom + kClockGap;

// --- Top band: the battery meter, left of the clock --------------------------
// A drawn meter rather than LVGL's battery glyphs, which come in five fixed steps
// — a bar that moves continuously says "draining" where a five-state icon says
// nothing for an hour and then jumps. Drawn from plain objects rather than a paint
// callback: a body, a terminal nub, and a fill whose width is the charge.
//
// No number beside it. A percentage on a lithium pack is an estimate off a
// published curve (see power.cpp), and printing an estimate to the digit invites
// it to be read as a measurement. The bar carries exactly the precision the
// estimate has; the Power page prints the number and the volts it came from.
constexpr int32_t kBatBodyW = 18;
constexpr int32_t kBatBodyH = 11;
constexpr int32_t kBatBorder = 1;
constexpr int32_t kBatNubW = 2;
constexpr int32_t kBatNubH = 5;
constexpr int32_t kBatInnerPad = 1;
constexpr int32_t kBatFillMaxW = kBatBodyW - 2 * (kBatBorder + kBatInnerPad);
constexpr int32_t kBatW = kBatBodyW + kBatNubW;

// The bolt keeps its slot whether or not it is showing, so the clock does not
// shuffle sideways every time a charger is plugged in.
constexpr int32_t kBatGap  = 3;
constexpr int32_t kBoltW   = 9;

// The bolt reads a shade heavy beside the meter at the body size. Scaled at draw
// time rather than set in a smaller face: Montserrat 12 is not compiled, and
// adding a whole font for one glyph would cost flash and put a fourth entry in a
// theme that deliberately names three roles. LVGL scales in 1/256ths, and the pivot
// has to be the box centre — it defaults to the top-left corner, which would shrink
// the glyph away from the clock instead of in place.
constexpr int32_t kScaleUnity   = 256;
constexpr int32_t kBoltScalePct = 85;
constexpr int32_t kBoltScale    = kScaleUnity * kBoltScalePct / 100;
constexpr int32_t kBatX    = kEdgePad;
constexpr int32_t kBoltX   = kBatX + kBatW + kBatGap;
constexpr int32_t kClockX  = kBoltX + kBoltW + kBatGap;
constexpr int32_t kClockW  = kEdgePad + kNavPillW - kClockX;
static_assert(kClockW >= 40, "the clock's slot is too narrow for HH:MM");

// Centred on the text line beside it rather than sharing its top edge: the meter is
// shorter than a line of Montserrat 14, and hanging both from the same y puts the
// bar visibly above the digits.
constexpr int32_t kBatY  = kClockRowY + (kFontBodyH - kBatBodyH) / 2;
constexpr int32_t kBatNubY = kClockRowY + (kFontBodyH - kBatNubH) / 2;

// Below this the bar goes amber. A quarter left on a lithium pack is not a quarter
// of the runtime left — the curve falls away underneath it — so the colour change
// belongs here rather than at some lower number that would arrive too late.
constexpr int kBatteryLowPct = 25;

// Exactly level at Montserrat 42: the clock's last row ends on the readout band's
// own bottom edge, so the left column has no slack left and a smaller readout font
// fails here rather than pushing the clock into the chart.
constexpr int32_t kClockY = kClockRowY;
// Exactly level at Montserrat 42: the clock's last row ends on the readout band's
// own bottom edge, so the left column has no slack left and a smaller readout font
// fails here rather than pushing the clock into the chart.
static_assert(kClockRowY + kFontBodyH <= kTopPad + kReadoutH,
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
constexpr int32_t kChipSpacing = kFontBodyH + kChipLeading;
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
constexpr int32_t kStatusRow2Y = kStatusY + kFontBodyH;


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

// --- Alarm overlay ----------------------------------------------------------
// A panel that floats over the top of the chart, immediately under the readout:
// the number and the reason it is the wrong number read as one block, and the
// graph stays visible underneath rather than being replaced by the alarm. It is
// deliberately not full-screen — a post that hides its own temperature the moment
// the temperature matters is the wrong instinct, however loud the alarm.
//
// It cannot reach the readout above it either, which is what keeps the
// tap-to-swap-units target clear of the Snooze button by the width of the chart's
// top border plus the inset.
constexpr int32_t kAlarmInset = 12;  // in from the chart's own edges, all round
constexpr int32_t kAlarmX = kChartX + kAlarmInset;
constexpr int32_t kAlarmY = kChartY + kAlarmInset;
constexpr int32_t kAlarmW = kChartW - 2 * kAlarmInset;
constexpr int32_t kAlarmPad = 10;
constexpr int32_t kAlarmRowGap = 8;
constexpr int32_t kAlarmBorderPx = 2;
constexpr int32_t kAlarmRadius = 8;
// Children are positioned inside the content area, which the border insets — the
// same relationship the chart's kPlotW/kPlotH already spell out. So the panel's
// own height carries the border twice and nothing below has to subtract it.
constexpr int32_t kAlarmContentW = kAlarmW - 2 * kAlarmBorderPx;
constexpr int32_t kAlarmH = 2 * kAlarmBorderPx + kAlarmPad + kFontTitleH +
                            kAlarmRowGap + kPillH + kAlarmPad;
static_assert(kAlarmY + kAlarmH <= kChartY + kChartH,
              "the alarm overlay hangs off the bottom of the chart");

// Two buttons side by side, gutter derived exactly as the menu's grid derives its
// own and for the same reason: adjacent hit areas that meet in the gap belong to
// whichever control was built last, and here that would be Snooze swallowing taps
// meant for Off. kAlarmSeam is the dead strip left deliberately between them.
constexpr int32_t kAlarmBtnExtClick = ext_click_for(kPillH, kPillH);
constexpr int32_t kAlarmSeam = 2;
constexpr int32_t kAlarmBtnGap = 2 * kAlarmBtnExtClick + kAlarmSeam;
constexpr int32_t kAlarmBtnW = (kAlarmContentW - 2 * kAlarmPad - kAlarmBtnGap) / 2;
static_assert(kAlarmBtnW > kPillH,
              "a button taller than it is wide takes its hit margin from the width, "
              "which kAlarmBtnExtClick above assumes it never does");
constexpr int32_t kAlarmBtnY = kAlarmPad + kFontTitleH + kAlarmRowGap;

// Said the way someone across the room reads it, not the way the state machine
// spells it. Fault gets a line of its own rather than being folded into one of
// the other two: "no reading" is a different problem from a wrong reading, and
// the fix is at the other end of the flying lead.
constexpr const char* kAlarmHigh  = "TOO HOT";
constexpr const char* kAlarmLow   = "TOO COLD";
constexpr const char* kAlarmFault = "NO READING";
constexpr const char* kAlarmOff   = "Off";
// Labelled with the number it acts on, from alert::snooze_minutes() — a button
// that hides how long it lasts has to be learned once and remembered after.
constexpr const char* kAlarmSnoozeFmt = "Snooze %lum";

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
lv_obj_t* s_bat_body    = nullptr;
lv_obj_t* s_bat_nub     = nullptr;
lv_obj_t* s_bat_fill    = nullptr;
lv_obj_t* s_bat_slash   = nullptr;
lv_obj_t* s_bolt_lbl    = nullptr;
lv_obj_t* s_alarm_panel = nullptr;
lv_obj_t* s_alarm_title = nullptr;
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

// Corner to corner across the meter's body — the "no pack fitted" mark. A slash
// says absent where a dim, empty meter only says flat, and the two are worth
// telling apart at a glance on a device that runs happily on USB alone.
lv_point_precise_t s_slash_pts[2] = {{0, kBatBodyH - 1}, {kBatBodyW - 1, 0}};

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

// The one colour the whole screen's alarm state is painted in — the readout, the
// overlay's border, its title.
//
// Taken from the state machine rather than recomputed from the reading and the
// band, which is what this used to do. The difference is hysteresis: a bare
// compare re-decides on every sample, so a temperature sitting on the limit
// flickers the readout between two colours several times a minute while the alarm
// itself, which has a recovery margin, is not changing its mind at all.
lv_color_t state_color(alert::State s) {
  switch (s) {
    case alert::State::High:  return kColorHigh;
    case alert::State::Low:   return kColorLow;
    case alert::State::Fault: return kColorFault;
    default:                  return kColorText;
  }
}

const char* alarm_title(alert::State s) {
  switch (s) {
    case alert::State::Low:   return kAlarmLow;
    case alert::State::Fault: return kAlarmFault;
    default:                  return kAlarmHigh;
  }
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

  // --- the alarm, which colours the number as well as raising the panel
  const alert::Status alarm = alert::status();
  const lv_color_t tint = state_color(alarm.state);
  lv_obj_set_style_text_color(s_value_lbl, tint, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_unit_lbl, tint, LV_PART_MAIN);

  // The panel is up exactly while the alarm is live and unsilenced, so Snooze and
  // Off close it by the same predicate that quiets the speaker. The tint is not
  // part of that: a silenced alarm is still an alarm, and the number goes on
  // saying so after the panel has been dismissed.
  if (alert::should_alert(alarm)) {
    lv_label_set_text(s_alarm_title, alarm_title(alarm.state));
    lv_obj_set_style_text_color(s_alarm_title, tint, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_alarm_panel, tint, LV_PART_MAIN);
    lv_obj_remove_flag(s_alarm_panel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_alarm_panel, LV_OBJ_FLAG_HIDDEN);
  }

  // --- threshold chips. Only the numbers change; the arrows are fixed.
  std::snprintf(text, sizeof(text), "%.1f",
                static_cast<double>(settings::display_value(t.max_c100)));
  lv_label_set_text(s_max_num, text);
  std::snprintf(text, sizeof(text), "%.1f",
                static_cast<double>(settings::display_value(t.min_c100)));
  lv_label_set_text(s_min_num, text);

  // --- the battery meter. Absent and empty are different things, and the slash is
  // what separates them: a post running on USB with no pack fitted is a normal way
  // to run one, and an empty meter would read as a fault.
  const power::Battery bat = power::battery();
  const bool have_pack = bat.present && bat.calibrated;
  if (have_pack) {
    lv_obj_add_flag(s_bat_slash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_bat_fill, LV_OBJ_FLAG_HIDDEN);
    const int32_t w = kBatFillMaxW * std::clamp(bat.pct, 0, 100) / 100;
    // Never zero while a pack is answering: a bar that disappears entirely looks
    // like the meter stopped working rather than like the pack is nearly flat.
    lv_obj_set_width(s_bat_fill, std::max<int32_t>(w, 1));
    lv_obj_set_style_bg_color(s_bat_fill,
                              bat.pct < kBatteryLowPct ? kColorFault : kColorOk,
                              LV_PART_MAIN);
  } else {
    lv_obj_remove_flag(s_bat_slash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bat_fill, LV_OBJ_FLAG_HIDDEN);
  }

  if (power::on_external_power()) {
    lv_obj_remove_flag(s_bolt_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_bolt_lbl, LV_OBJ_FLAG_HIDDEN);
  }

  // --- clock, beside the meter
  char clock[kClockTextMax];
  if (!format_clock(clock, sizeof(clock), kClockFmt)) {
    std::snprintf(clock, sizeof(clock), "%s", kNoClock);
  }
  lv_label_set_text(s_clock_lbl, clock);

  // --- status strip
  lv_label_set_text(s_name_lbl, s.name);

  // Blank rather than a placeholder when there is no pack: a post on USB with no
  // battery fitted is a normal way to run one, and a permanent "--" in the corner
  // would read as a fault that needs looking into.

  // Three different questions, which is why one number was not enough:
  //   live     — appended since boot; frozen means logging has stopped
  //   total    — ever written, the absolute index cloud sync counts against
  //   uploaded — how far the Sheet has caught up
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

// Both repaint immediately rather than waiting out kRefreshMs. The tone stops
// inside alert's own call, so a panel that lingered a quarter of a second after
// the speaker went quiet would read as a button that half-worked.
void alarm_snooze_clicked(lv_event_t*) {
  alert::snooze();
  refresh(nullptr);
  ESP_LOGI(kTag, "alarm snoozed for %lu min",
           static_cast<unsigned long>(alert::snooze_minutes()));
}

void alarm_off_clicked(lv_event_t*) {
  alert::silence();
  refresh(nullptr);
  ESP_LOGI(kTag, "alarm silenced until the reading changes");
}

// --- construction ----------------------------------------------------------

// Every boxed label on this screen is Montserrat 14 — the clock and the two
// threshold numbers — so the font is bound once here rather than at each call.
lv_obj_t* make_chip_label(lv_obj_t* parent, lv_color_t color, int32_t w,
                          lv_text_align_t align) {
  return make_boxed_label(parent, kFontBody, color, w, align);
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

// A bare rectangle with no padding, border or scrolling — the building block the
// meter is assembled from. lv_obj's defaults are all wrong for a shape this small.
lv_obj_t* make_block(lv_obj_t* parent, int32_t w, int32_t h, lv_color_t color) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

void build_battery_meter(lv_obj_t* scr) {
  s_bat_body = make_block(scr, kBatBodyW, kBatBodyH, kColorBg);
  lv_obj_set_style_border_width(s_bat_body, kBatBorder, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_bat_body, kColorMuted, LV_PART_MAIN);
  lv_obj_set_pos(s_bat_body, kBatX, kBatY);

  s_bat_nub = make_block(scr, kBatNubW, kBatNubH, kColorMuted);
  lv_obj_set_pos(s_bat_nub, kBatX + kBatBodyW, kBatNubY);

  // A child of the body, so the border insets it and the fill can never paint over
  // the outline it is supposed to sit inside.
  s_bat_fill = make_block(s_bat_body, kBatFillMaxW, kBatBodyH - 2 * (kBatBorder + kBatInnerPad),
                          kColorOk);
  lv_obj_set_pos(s_bat_fill, kBatInnerPad, kBatInnerPad);

  s_bat_slash = lv_line_create(scr);
  lv_line_set_points(s_bat_slash, s_slash_pts, 2);
  lv_obj_set_style_line_width(s_bat_slash, kBatBorder, LV_PART_MAIN);
  lv_obj_set_style_line_color(s_bat_slash, kColorMuted, LV_PART_MAIN);
  lv_obj_set_pos(s_bat_slash, kBatX, kBatY);

  s_bolt_lbl = make_boxed_label(scr, kFontBody, kColorOk, kBoltW, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(s_bolt_lbl, LV_SYMBOL_CHARGE);
  lv_obj_set_style_transform_pivot_x(s_bolt_lbl, kBoltW / 2, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(s_bolt_lbl, kFontBodyH / 2, LV_PART_MAIN);
  lv_obj_set_style_transform_scale(s_bolt_lbl, kBoltScale, LV_PART_MAIN);
  lv_obj_set_pos(s_bolt_lbl, kBoltX, kClockRowY);
}

void build_top_band(lv_obj_t* scr) {
  build_nav_pill(scr, &s_nav, NavGlyph::Burger, "Menu", menu_clicked);

  // --- the battery meter, the charge bolt and the clock, in that order
  build_battery_meter(scr);
  s_clock_lbl = make_chip_label(scr, kColorMuted, kClockW, LV_TEXT_ALIGN_LEFT);
  lv_label_set_long_mode(s_clock_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_clock_lbl, kClockX, kClockRowY);

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

  s_value_lbl = make_label(s_readout_row, kFontReadout, kColorText, kNoValue);
  s_unit_lbl = make_label(s_readout_row, kFontBody, kColorText, "");

  // Sit the unit on the digits' baseline, computed from the two fonts rather than
  // guessed: a font's baseline is line_height - base_line below its box top, so the
  // unit's box needs padding equal to the difference. Derived, so swapping either
  // font keeps them aligned instead of quietly drifting apart.
  const int32_t digits_baseline =
      kFontReadout->line_height - kFontReadout->base_line;
  const int32_t unit_baseline =
      kFontBody->line_height - kFontBody->base_line;
  lv_obj_set_style_pad_top(s_unit_lbl, digits_baseline - unit_baseline, LV_PART_MAIN);

  lv_obj_align(s_readout_row, LV_ALIGN_TOP_MID, kReadoutDx, kReadoutY);

  // --- threshold chips, right. Arrows fixed, numbers centred beside them.
  s_max_arrow = make_label(scr, kFontBody, kColorHigh, LV_SYMBOL_UP);
  lv_obj_set_pos(s_max_arrow, kChipArrowX, kChipY);
  s_max_num = make_chip_label(scr, kColorHigh, kChipNumW, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_max_num, kChipNumX, kChipY);

  s_min_arrow = make_label(scr, kFontBody, kColorLow, LV_SYMBOL_DOWN);
  lv_obj_set_pos(s_min_arrow, kChipArrowX, kChipY + kChipSpacing);
  s_min_num = make_chip_label(scr, kColorLow, kChipNumW, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_min_num, kChipNumX, kChipY + kChipSpacing);
}

// Built after the chart so it is the later sibling and therefore draws on top —
// LVGL's z-order among siblings is creation order, and an overlay created first
// would be a panel hidden behind the graph it is meant to cover.
//
// Comes up hidden. refresh() is the only thing that shows it, so there is exactly
// one place that decides whether an alarm is on screen.
void build_alarm_panel(lv_obj_t* parent) {
  s_alarm_panel = lv_obj_create(parent);
  lv_obj_set_size(s_alarm_panel, kAlarmW, kAlarmH);
  lv_obj_set_pos(s_alarm_panel, kAlarmX, kAlarmY);
  lv_obj_set_style_bg_color(s_alarm_panel, kColorPanel, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_alarm_panel, LV_OPA_COVER, LV_PART_MAIN);
  // The border is the state's colour and is set in refresh(); only its width and
  // the corner are fixed here.
  lv_obj_set_style_border_width(s_alarm_panel, kAlarmBorderPx, LV_PART_MAIN);
  lv_obj_set_style_radius(s_alarm_panel, kAlarmRadius, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_alarm_panel, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_alarm_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_alarm_panel, LV_OBJ_FLAG_HIDDEN);

  s_alarm_title = make_boxed_label(s_alarm_panel, kFontTitle, kColorText,
                                   kAlarmContentW, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_alarm_title, 0, kAlarmPad);

  // Snooze on the left, Off on the right: left to right is least committal to
  // most, and the one that ends the alarm for good should not be the one a thumb
  // reaches first.
  char snooze[kTextMax];
  std::snprintf(snooze, sizeof(snooze), kAlarmSnoozeFmt,
                static_cast<unsigned long>(alert::snooze_minutes()));
  lv_obj_t* snooze_btn =
      build_pill(s_alarm_panel, snooze, kAlarmBtnW, kPillH, kColorMenu, kFontTitle,
                 alarm_snooze_clicked, nullptr);
  lv_obj_set_pos(snooze_btn, kAlarmPad, kAlarmBtnY);

  lv_obj_t* off_btn =
      build_pill(s_alarm_panel, kAlarmOff, kAlarmBtnW, kPillH, kColorText, kFontTitle,
                 alarm_off_clicked, nullptr);
  lv_obj_set_pos(off_btn, kAlarmPad + kAlarmBtnW + kAlarmBtnGap, kAlarmBtnY);
}

void build_screen(lv_obj_t* scr) {
  s_group = build_group(scr);

  build_top_band(s_group);
  build_chart(s_group);
  build_alarm_panel(s_group);

  // --- time captions
  s_axis_left = make_label(s_group, kFontBody, kColorMuted, "");
  lv_obj_set_pos(s_axis_left, kEdgePad, kAxisY);
  s_axis_mid = make_label(s_group, kFontBody, kColorMuted, "");
  lv_obj_align(s_axis_mid, LV_ALIGN_TOP_MID, 0, kAxisY);
  s_axis_right = make_label(s_group, kFontBody, kColorMuted, "now");
  lv_obj_align(s_axis_right, LV_ALIGN_TOP_RIGHT, -kEdgePad, kAxisY);

  // --- status strip, two rows. Both ellipsise rather than wrap: each row is one
  // line high and a wrapped line would grow downwards off the panel.
  s_name_lbl = make_label(s_group, kFontBody, kColorSubText, "");
  lv_obj_set_width(s_name_lbl, kStatusW);
  lv_label_set_long_mode(s_name_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_name_lbl, kEdgePad, kStatusY);

  s_counter_lbl = make_label(s_group, kFontBody, kColorMuted, "");
  lv_obj_set_width(s_counter_lbl, kStatusW);
  lv_label_set_long_mode(s_counter_lbl, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_counter_lbl, kEdgePad, kStatusRow2Y);

  // The chart's threshold lines are its children, and LV_STYLE_OPA applies to an
  // object together with everything inside it, so the chart fades as one piece.
  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_nav.btn;
  fade[n++] = s_clock_lbl;
  fade[n++] = s_bat_body;
  fade[n++] = s_bat_nub;
  fade[n++] = s_bat_slash;
  fade[n++] = s_bolt_lbl;
  fade[n++] = s_readout_row;
  fade[n++] = s_max_arrow;
  fade[n++] = s_max_num;
  fade[n++] = s_min_arrow;
  fade[n++] = s_min_num;
  fade[n++] = s_chart;
  // The panel alone: LV_STYLE_OPA applies to an object together with its children,
  // so its title and both buttons fade with it. Fading a hidden object is a no-op
  // that costs one animation, which is cheaper than teaching the nav engine about
  // widgets that come and go.
  fade[n++] = s_alarm_panel;
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
  if (kFontBody->line_height != kFontBodyH ||
      kFontTitle->line_height != kFontTitleH ||
      kFontReadout->line_height != kFontReadoutH) {
    ESP_LOGW(kTag, "font line heights are %d/%d/%d, layout budgeted %ld/%ld/%ld — rows may crowd",
             kFontBody->line_height, kFontTitle->line_height,
             kFontReadout->line_height, static_cast<long>(kFontBodyH),
             static_cast<long>(kFontTitleH), static_cast<long>(kFontReadoutH));
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
  network_screen::build(scr);
  configure_screen::build(scr);
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
