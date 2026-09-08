#include "ui_settings.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "audio.hpp"
#include "power.hpp"
#include "settings.hpp"
#include "ui_chrome.hpp"
#include "ui_nav.hpp"
#include "ui_stepper.hpp"
#include "ui_theme.hpp"

namespace thermopost::ui::settings_screen {
namespace {

constexpr size_t kTextMax = 64;
// Anything format_span() writes, and no more. Formatting a kTextMax buffer into
// another kTextMax buffer is a truncation GCC refuses outright, and the honest fix
// is the smaller source rather than a bigger target.
//
// Sized from the compiler's worst case rather than ours: for a uint32_t of seconds
// the widest "%luh %lum" is "1193046h 59m", twelve characters. The Recording footer
// formats *two* of these into one kTextMax line, which is what forced this bound to
// be a real one — at 32 the pair no longer provably fits.
constexpr size_t kSpanTextMax = 16;

// --- Shared page geometry ---------------------------------------------------
// Two columns, two rows. A cell is a caption with its control underneath, and the
// cell is as wide as the widest control it can hold — the stepper, click area
// included — so a caption can never be wider than the thing it labels.
constexpr StepperCfg kStepperCfg = {kFontTitle, 50, 34, /*suffix_below=*/false};

// Same geometry, unit on its own line. The discs leave 2*50 - 34 = 66 px between
// them; "21.5" fits that at kFontTitle and "21.5°C" does not, and the discs cannot
// move apart without breaking the two-column assert below.
constexpr StepperCfg kTempStepperCfg = {kFontTitle, 50, 34, /*suffix_below=*/true};
static_assert(stepper_width(kTempStepperCfg) == stepper_width(kStepperCfg) &&
                  stepper_height(kTempStepperCfg) == stepper_height(kStepperCfg),
              "the temperature stepper must stay the size the page geometry assumes");

constexpr int32_t kCaptionGap = 2;
constexpr int32_t kRow1Y = 48;
constexpr int32_t kRow2Y = 128;
constexpr int32_t kColLeftCx  = kScreenW / 4;
constexpr int32_t kColRightCx = 3 * kScreenW / 4;
constexpr int32_t kColMidCx   = kScreenW / 2;

constexpr int32_t kToggleW = 120;
constexpr int32_t kToggleH = kPillH;

// A row can hold a toggle beside a stepper, and the two are not the same height —
// the stepper's container carries its discs' click margin as well as the discs. So a
// cell's control sits in a band as tall as the tallest of them, centred, and the two
// share a centreline instead of each centring on its own height. Without this the
// Sound page's toggle rides six pixels above the volume discs, which reads as a
// misalignment rather than as two differently sized controls.
constexpr int32_t kControlBandH = stepper_height(kStepperCfg);
constexpr int32_t kPillBandDy = (kControlBandH - kPillH) / 2;
static_assert(kPillH <= kControlBandH,
              "a text pill taller than the band would centre outside its own cell");

// Checked here rather than discovered on the panel: a stepper that overhangs the
// screen edge loses the half of its click area that hangs off, which reads as a
// disc that only works on one side.
constexpr int32_t kCellW = stepper_width(kStepperCfg);
static_assert(kColLeftCx - kCellW / 2 >= 0, "the left column overhangs the screen");
static_assert(kColRightCx + kCellW / 2 <= kScreenW, "the right column overhangs the screen");
static_assert(kRow2Y + kFontBodyH + kCaptionGap + stepper_height(kStepperCfg) <= kFooterY,
              "the second control row runs into the footer");

// Three pages put a control in each column of row 1, so the two must not meet.
// stepper_width already includes the discs' click margin, which makes this the hit
// test and not just the visual one: at kExtClickPad = 10 the clearance between the
// left cell's (+) and the right cell's (-) is six pixels, and raising the pad by four
// is what turns a tap between them into a value changing on the wrong stepper.
static_assert(kColRightCx - kColLeftCx >= kCellW,
              "the two column cells' hit areas overlap between them");

// A toggle is the other thing a column holds, and it is sized independently — so the
// column check above only covers it while it stays the narrower of the two.
constexpr int32_t kToggleExtClick = ext_click_for(kToggleW, kToggleH);
static_assert(kToggleW + 2 * kToggleExtClick <= kCellW,
              "a toggle now reaches wider than a stepper cell, so the column assert "
              "above no longer speaks for it");

// --- Threshold coupling -----------------------------------------------------
// Minimum gap between the two thresholds, in tenths of a display degree. A whole
// degree, not a tenth: the pair must not cross, and a band 0.1 wide is one nothing
// could sit inside — every reading would be an alarm in one direction or the other.
constexpr int16_t kBandMinTenths = 10;

// How long the page waits after the last tap before writing to NVS. A held disc
// steps ten times a second, and persisting each one would be ten flash writes a
// second for as long as a finger is down. Long enough to swallow a hold, short
// enough that letting go feels like it committed.
constexpr uint32_t kPersistDelayMs = 400;

// --- Interval table ---------------------------------------------------------
// The stepper's value is an index into this. Ten taps covers ten seconds to an
// hour; any fixed step over that range is either hundreds of taps or so coarse that
// the fast end is unreachable.
constexpr uint16_t kIntervalChoicesS[] = {10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
constexpr int16_t kIntervalChoiceCount =
    static_cast<int16_t>(sizeof(kIntervalChoicesS) / sizeof(kIntervalChoicesS[0]));
static_assert(kIntervalChoicesS[0] >= settings::kIntervalMinS,
              "the fastest offered interval is below what settings accepts");
static_assert(kIntervalChoicesS[kIntervalChoiceCount - 1] <= settings::kIntervalMaxS,
              "the slowest offered interval is above what settings accepts");

// --- Upload cadence table ---------------------------------------------------
// Minutes between radio sessions; the first entry is "every record", which is what
// settings stores as zero. Same index-into-a-table trick as the interval above, and
// for the same reason: the useful range spans two orders of magnitude.
//
// The top of the table is a day rather than the week the range would allow. A post
// that has not spoken since yesterday is one someone should look at, and offering a
// cadence that makes silence normal would hide exactly that.
constexpr uint16_t kUploadChoicesMin[] = {0, 5, 15, 30, 60, 180, 360, 720, 1440};
constexpr int16_t kUploadChoiceCount =
    static_cast<int16_t>(sizeof(kUploadChoicesMin) / sizeof(kUploadChoicesMin[0]));
static_assert(kUploadChoicesMin[kUploadChoiceCount - 1] <= settings::kUploadEveryMaxMin,
              "the slowest offered upload cadence is above what settings accepts");

// A default the stepper cannot land on would show one value while NVS held another,
// until the first tap quietly resolved the disagreement.
constexpr bool upload_choice_exists(uint16_t v) {
  for (int16_t i = 0; i < kUploadChoiceCount; ++i) {
    if (kUploadChoicesMin[i] == v) return true;
  }
  return false;
}
static_assert(upload_choice_exists(settings::kUploadEveryDefaultMin),
              "the lit upload default is not one of the offered cadences");
static_assert(upload_choice_exists(settings::kUploadDarkDefaultMin),
              "the dark upload default is not one of the offered cadences");

constexpr uint16_t kGraphPointsStep = 30;
constexpr uint8_t  kVolumeStepPct   = 5;

// The battery readout is the one value on any page that moves without being
// touched, so the Power page repolls it while it is open.
constexpr uint32_t kBatteryPollMs = 2000;

constexpr uint32_t kSecPerMin  = 60;
constexpr uint32_t kSecPerHour = 3600;

// Centre a widget of known width on `cx`, top edge at `y`. The width is passed in
// rather than read back with lv_obj_get_width: coords are only reliable after a
// layout pass, and every control here has a width its caller already knows.
void place(lv_obj_t* obj, int32_t cx, int32_t w, int32_t y) {
  lv_obj_set_pos(obj, cx - w / 2, y);
}

lv_obj_t* build_caption(lv_obj_t* group, int32_t cx, int32_t y, const char* text) {
  lv_obj_t* l = make_boxed_label(group, kFontBody, kColorSubText, kCellW,
                                 LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(l, text);
  lv_obj_set_pos(l, cx - kCellW / 2, y);
  return l;
}

// A stepper cell: caption, then the control under it. Returns the control's
// container so the caller can add it to the fade set.
lv_obj_t* build_stepper_cell(lv_obj_t* group, int32_t cx, int32_t y, const char* caption,
                             StepperState* st, lv_obj_t** out_caption,
                             const StepperCfg& cfg = kStepperCfg) {
  *out_caption = build_caption(group, cx, y, caption);
  lv_obj_t* row = build_stepper(group, st, cfg);
  place(row, cx, stepper_width(cfg), y + kFontBodyH + kCaptionGap);
  return row;
}

// A toggle cell: same caption, and a capsule whose label *is* the current value.
// A switch would need a legend to say which end is which; a capsule reading "°F"
// says it outright, and the tap target is the whole word.
lv_obj_t* build_toggle_cell(lv_obj_t* group, int32_t cx, int32_t y, const char* caption,
                            lv_event_cb_t on_tap, lv_obj_t** out_caption) {
  *out_caption = build_caption(group, cx, y, caption);
  lv_obj_t* pill = build_pill(group, "", kToggleW, kToggleH, kColorText,
                              kFontTitle, on_tap, nullptr);
  place(pill, cx, kToggleW, y + kFontBodyH + kCaptionGap + kPillBandDy);
  return pill;
}

// Nearest offered value to what is stored, so a setting written by an older
// firmware — or by anything that does not know this table — lands on a real stepper
// position instead of snapping to the bottom of it.
int16_t nearest_index(const uint16_t* table, int16_t count, uint16_t want) {
  int16_t best = 0;
  int32_t best_err = INT32_MAX;
  for (int16_t i = 0; i < count; ++i) {
    const int32_t err = std::abs(static_cast<int32_t>(table[i]) - want);
    if (err < best_err) {
      best_err = err;
      best = i;
    }
  }
  return best;
}

void format_span(char* out, size_t n, uint32_t seconds) {
  if (seconds >= kSecPerHour && seconds % kSecPerHour == 0) {
    std::snprintf(out, n, "%luh", static_cast<unsigned long>(seconds / kSecPerHour));
  } else if (seconds >= kSecPerHour) {
    std::snprintf(out, n, "%luh %lum", static_cast<unsigned long>(seconds / kSecPerHour),
                  static_cast<unsigned long>((seconds % kSecPerHour) / kSecPerMin));
  } else if (seconds >= kSecPerMin) {
    std::snprintf(out, n, "%lum", static_cast<unsigned long>(seconds / kSecPerMin));
  } else {
    std::snprintf(out, n, "%lus", static_cast<unsigned long>(seconds));
  }
}

// ===========================================================================
// Temperature — the alarm band, its recovery margin, and the unit all three are
// read in.
//
// The unit used to be a page of its own, and moving it here is not just tidying:
// it is the only control on the device whose effect is entirely on the three
// numbers beside it. On a page by itself a tap changed a suffix and nothing
// visibly followed; here the same tap re-renders the band it governs, which is
// what the setting actually does. It also puts the choice next to the values at
// the moment someone is deciding what to type into them.
// ===========================================================================
namespace temperature {

PageChrome    s_page = {};
StepperState  s_min = {};
StepperState  s_max = {};
StepperState  s_hyst = {};
lv_obj_t*     s_unit = nullptr;

lv_timer_t* s_persist = nullptr;

// The main screen's readout toggles the unit too. That shortcut is invisible until
// someone is told, and the moment they have just used the slow path is exactly when
// it is worth saying — so the footer says it for a beat after a toggle and then goes
// back to the stored values, which is what the line is for the rest of the time.
constexpr const char* kTapHint = "tapping the reading does this too";
constexpr uint32_t kTapHintMs = 2500;
lv_timer_t* s_hint = nullptr;

// While the page is open the three steppers are the working copy, and NVS is
// written from them once the taps stop. Everything below therefore derives from
// the steppers rather than from settings::get() — reading back a value that has
// not been written yet is how a coupled bound ends up one tap behind.
int16_t min_c100()  { return settings::c100_from_tenths(s_min.value); }
int16_t max_c100()  { return settings::c100_from_tenths(s_max.value); }
int16_t hyst_c100() { return settings::c100_from_span_tenths(s_hyst.value); }

// Largest hysteresis, in display tenths, whose *stored* value still fits under
// alert::sanitize()'s half-band cap. Rounded down and then verified, because
// rounding the cap to the nearest tenth can land a tenth above it — and a stepper
// whose top value gets silently clamped on its way to NVS is precisely the failure
// this coupling exists to prevent.
int16_t hysteresis_ceiling_tenths(int16_t lo_c100, int16_t hi_c100) {
  const int16_t cap = static_cast<int16_t>((hi_c100 - lo_c100) / 2);
  int16_t tenths = settings::display_span_tenths(cap);
  while (tenths > 0 && settings::c100_from_span_tenths(tenths) > cap) --tenths;
  return tenths;
}

// One tenth of a display degree, rendered. Written by hand rather than through a
// float: the value IS exact tenths, and -0.5 must not come out as "0.-5" — C
// division truncates toward zero, so anything between -9 and 0 has a whole part of
// zero and loses the sign with it.
void format_tenths(char* out, size_t n, int16_t v) {
  const int whole = v / 10;
  const int frac = (v < 0 ? -v : v) % 10;
  const char* sign = (v < 0 && whole == 0) ? "-" : "";
  std::snprintf(out, n, "%s%d.%d", sign, whole, frac);
}

void write_through() {
  settings::set_band(min_c100(), max_c100());
  settings::set_hysteresis_c100(hyst_c100());
}

void on_persist_due(lv_timer_t*) {
  s_persist = nullptr;  // one-shot: LVGL deletes it after this returns
  write_through();
}

void schedule_persist() {
  if (s_persist != nullptr) {
    lv_timer_reset(s_persist);
    return;
  }
  s_persist = lv_timer_create(on_persist_due, kPersistDelayMs, nullptr);
  lv_timer_set_repeat_count(s_persist, 1);
}

// Write now and cancel the pending timer. For leaving the page inside the delay,
// which would otherwise drop the last edit on the floor.
void flush_persist() {
  if (s_persist == nullptr) return;
  lv_timer_delete(s_persist);
  s_persist = nullptr;
  write_through();
}

// The stored Celsius hundredths, always, whatever the steppers are showing. In °C
// that is the same number with one more decimal; in °F it is the payoff — a band
// set in Fahrenheit is not a round number of Celsius, and this is where that shows
// rather than looking like drift somewhere else.
void refresh_footer() {
  // Any hint still showing is stale the moment a value moves, and cancelling it here
  // rather than letting it expire keeps the timer from overwriting a fresher line.
  if (s_hint != nullptr) {
    lv_timer_delete(s_hint);
    s_hint = nullptr;
  }
  char text[kTextMax];
  std::snprintf(text, sizeof(text), "stored %.2f to %.2f C, clears by %.2f",
                static_cast<double>(settings::c100_to_c(min_c100())),
                static_cast<double>(settings::c100_to_c(max_c100())),
                static_cast<double>(settings::c100_to_c(hyst_c100())));
  lv_label_set_text(s_page.footer, text);
}

void on_hint_done(lv_timer_t*) {
  s_hint = nullptr;  // one-shot: LVGL deletes it after this returns
  refresh_footer();
}

// Say something for a beat, then fall back to the stored line.
void show_hint(const char* text) {
  refresh_footer();  // cancels any hint already running
  lv_label_set_text(s_page.footer, text);
  s_hint = lv_timer_create(on_hint_done, kTapHintMs, nullptr);
  lv_timer_set_repeat_count(s_hint, 1);
}

// Re-derive every bound from the steppers. Called on show and after each tap, so
// the coupling holds no matter which of the three moved.
void rebound() {
  s_min.min = settings::display_tenths(settings::kTempFloorC100);
  s_min.max = static_cast<int16_t>(s_max.value - kBandMinTenths);
  s_max.min = static_cast<int16_t>(s_min.value + kBandMinTenths);
  s_max.max = settings::display_tenths(settings::kTempCeilC100);
  s_hyst.min = 0;
  s_hyst.max = hysteresis_ceiling_tenths(min_c100(), max_c100());

  // Only the hysteresis is clamped. A narrowed band can leave it above its own new
  // ceiling; sanitize() would cap it for the alarm logic anyway, so pulling the
  // stepper down now means the screen and NVS agree rather than the screen showing
  // a number the state machine has quietly stopped using.
  //
  // The two thresholds are deliberately not clamped to match each other. Their
  // bounds are derived from each other's *current* value, so neither can be out of
  // range in the first place — and pulling one to satisfy the other would move a
  // threshold nobody touched.
  if (stepper_clamp(&s_hyst)) schedule_persist();
}

// One handler for both thresholds: the deferred write takes the pair from the
// steppers, so there is no longer a version of this that touches only one of them.
//
// The precision that makes that safe is the point of tenths. Under whole degrees a
// stored 20.5 displayed as 21, and writing the display back while the user was
// only touching the maximum moved the minimum half a degree. A tenth of a display
// degree is at most 0.06 °C — below the MCP9808's own resolution — so the round
// trip is no longer something a threshold can be moved by.
void on_band_change() {
  rebound();
  stepper_refresh(&s_hyst);
  refresh_footer();
  schedule_persist();
}

void on_hyst_change() {
  refresh_footer();
  schedule_persist();
}

void load() {
  // Anything still pending belongs to the visit that scheduled it, and this reload
  // is about to overwrite the steppers it would have been written from.
  flush_persist();

  const settings::Settings s = settings::get();
  const char* suffix = settings::unit_suffix();
  s_min.suffix = suffix;
  s_max.suffix = suffix;
  // No suffix on a span: "0.5 °C" would be right, but a unit under a margin reads
  // as a temperature rather than a distance, and the caption already says which it
  // is. The empty label still holds the number on its siblings' baseline.
  s_hyst.suffix = "";

  s_min.value  = settings::display_tenths(s.min_c100);
  s_max.value  = settings::display_tenths(s.max_c100);
  s_hyst.value = settings::display_span_tenths(s.hysteresis_c100);

  lv_label_set_text(pill_label(s_unit), suffix);

  rebound();
  stepper_refresh(&s_min);
  stepper_refresh(&s_max);
  stepper_refresh(&s_hyst);
  refresh_footer();
}

// Switching the unit re-reads all three steppers, because tenths of a degree mean
// different amounts in each and the stored hundredths are what survives.
//
// The flush has to come *first*, and that ordering is the whole subtlety here. The
// steppers hold tenths in whatever unit was on screen when they were painted, and
// write_through() reads them back through c100_from_tenths(), which asks settings
// what the current unit is. Setting the unit before the pending write lands would
// reinterpret 21.5 °C as 21.5 °F and store a band nobody asked for — a silent
// 11-degree move, on a debounce window nobody can see.
void on_unit_toggle(lv_event_t*) {
  flush_persist();
  const settings::TempUnit next = settings::get().unit == settings::TempUnit::Celsius
                                      ? settings::TempUnit::Fahrenheit
                                      : settings::TempUnit::Celsius;
  settings::set_unit(next);
  load();
  show_hint(kTapHint);
}

// Leaving inside the debounce window would otherwise drop the last edit.
void on_back(lv_event_t*) {
  flush_persist();
  nav::go(nav::Screen::Menu);
}

void build(lv_obj_t* scr) {
  build_page_chrome(scr, "Temperature", &s_page, on_back);

  // A tenth per tap, a tenth per repeat, and ten times that once a hold has run
  // past kHoldFastAfter — see ui_stepper.cpp. Taps alone would be 1650 of them
  // across the sensor's range.
  for (StepperState* st : {&s_min, &s_max, &s_hyst}) {
    st->step = 1;
    st->hold_step = 1;
    st->format = format_tenths;
  }
  s_min.on_change = on_band_change;
  s_max.on_change = on_band_change;
  s_hyst.on_change = on_hyst_change;

  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_page.back.btn;
  fade[n++] = s_page.heading;
  fade[n++] = s_page.footer;
  lv_obj_t* cap = nullptr;
  fade[n++] = build_stepper_cell(s_page.group, kColLeftCx, kRow1Y, "Alarm below",
                                 &s_min, &cap, kTempStepperCfg);
  fade[n++] = cap;
  fade[n++] = build_stepper_cell(s_page.group, kColRightCx, kRow1Y, "Alarm above",
                                 &s_max, &cap, kTempStepperCfg);
  fade[n++] = cap;
  // Row 2 is a pair now rather than one centred cell: the margin keeps the left
  // column under the minimum it belongs to, and the unit takes the right. Both are
  // geometry the shared asserts above already cover — a toggle beside a stepper is
  // what the Sound page does, and kPillBandDy is what puts them on one centreline.
  fade[n++] = build_stepper_cell(s_page.group, kColLeftCx, kRow2Y, "Recovery margin",
                                 &s_hyst, &cap, kTempStepperCfg);
  fade[n++] = cap;
  s_unit = build_toggle_cell(s_page.group, kColRightCx, kRow2Y, "Unit", on_unit_toggle,
                             &cap);
  fade[n++] = s_unit;
  fade[n++] = cap;

  load();
  nav::register_screen(nav::Screen::Temperature, s_page.group, fade, n, load);
}

}  // namespace temperature

// ===========================================================================
// Recording — how often a record is written, and how many are plotted.
// ===========================================================================
namespace recording {

// Wider than kTextMax: this footer formats three spans into one line, and the
// compiler bounds each at kSpanTextMax whatever the values can really be.
constexpr size_t kFooterTextMax = 96;

PageChrome   s_page = {};
StepperState s_interval = {};
StepperState s_points = {};
StepperState s_upload = {};
StepperState s_upload_dark = {};

void format_interval(char* out, size_t n, int16_t index) {
  const int16_t i = static_cast<int16_t>(
      std::clamp<int16_t>(index, 0, kIntervalChoiceCount - 1));
  format_span(out, n, kIntervalChoicesS[i]);
}

// "rec" rather than "record": the stepper's value sits between the two discs, and
// the longer word runs into them.
void format_upload(char* out, size_t n, int16_t index) {
  const int16_t i = static_cast<int16_t>(
      std::clamp<int16_t>(index, 0, kUploadChoiceCount - 1));
  const uint16_t minutes = kUploadChoicesMin[i];
  if (minutes == 0) {
    std::snprintf(out, n, "rec");
    return;
  }
  format_span(out, n, static_cast<uint32_t>(minutes) * kSecPerMin);
}

// The lag one cadence allows, as the footer says it: zero means the Sheet is never
// waiting on anything.
void format_lag(char* out, size_t n, uint16_t minutes) {
  if (minutes == 0) {
    std::snprintf(out, n, "live");
    return;
  }
  format_span(out, n, static_cast<uint32_t>(minutes) * kSecPerMin);
}

// Every number on this page is a span someone is choosing between, so the footer
// states each as the thing it actually costs: how much history the graph holds, and
// how stale the Sheet is allowed to get on each side of the screen going dark.
void refresh_footer() {
  const settings::Settings s = settings::get();
  char span[kSpanTextMax];
  format_span(span, sizeof(span),
              static_cast<uint32_t>(s.graph_points) * s.interval_s);
  char text[kFooterTextMax];
  if (s.upload_every_min == 0 && s.upload_dark_min == 0) {
    std::snprintf(text, sizeof(text), "the graph spans %s", span);
  } else {
    char lit[kSpanTextMax];
    char dark[kSpanTextMax];
    format_lag(lit, sizeof(lit), s.upload_every_min);
    format_lag(dark, sizeof(dark), s.upload_dark_min);
    // ASCII separators: LVGL's built-in Montserrat carries 0x20-0x7F plus a handful
    // of symbols, and a middle dot is not among them — it would draw as a blank box.
    std::snprintf(text, sizeof(text), "graph %s, Sheet %s, %s when dark", span, lit, dark);
  }
  lv_label_set_text(s_page.footer, text);
}

void on_interval_change() {
  settings::set_interval_s(kIntervalChoicesS[s_interval.value]);
  refresh_footer();
}

void on_points_change() {
  settings::set_graph_points(static_cast<uint16_t>(s_points.value));
  refresh_footer();
}

void on_upload_change() {
  settings::set_upload_every_min(kUploadChoicesMin[s_upload.value]);
  refresh_footer();
}

void on_upload_dark_change() {
  settings::set_upload_dark_min(kUploadChoicesMin[s_upload_dark.value]);
  refresh_footer();
}

void load() {
  const settings::Settings s = settings::get();
  s_interval.value = nearest_index(kIntervalChoicesS, kIntervalChoiceCount, s.interval_s);
  s_points.value = static_cast<int16_t>(s.graph_points);
  s_upload.value =
      nearest_index(kUploadChoicesMin, kUploadChoiceCount, s.upload_every_min);
  s_upload_dark.value =
      nearest_index(kUploadChoicesMin, kUploadChoiceCount, s.upload_dark_min);
  stepper_refresh(&s_interval);
  stepper_refresh(&s_points);
  stepper_refresh(&s_upload);
  stepper_refresh(&s_upload_dark);
  refresh_footer();
}

void build(lv_obj_t* scr) {
  build_page_chrome(scr, "Recording", &s_page);

  s_interval.min = 0;
  s_interval.max = kIntervalChoiceCount - 1;
  s_interval.step = 1;
  s_interval.format = format_interval;
  s_interval.on_change = on_interval_change;

  s_points.min = static_cast<int16_t>(settings::kGraphPointsMin);
  s_points.max = static_cast<int16_t>(settings::kGraphPointsMax);
  s_points.step = kGraphPointsStep;
  s_points.suffix = "";
  s_points.on_change = on_points_change;

  s_upload.min = 0;
  s_upload.max = kUploadChoiceCount - 1;
  s_upload.step = 1;
  s_upload.format = format_upload;
  s_upload.on_change = on_upload_change;

  s_upload_dark.min = 0;
  s_upload_dark.max = kUploadChoiceCount - 1;
  s_upload_dark.step = 1;
  s_upload_dark.format = format_upload;
  s_upload_dark.on_change = on_upload_dark_change;

  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_page.back.btn;
  fade[n++] = s_page.heading;
  fade[n++] = s_page.footer;
  lv_obj_t* cap = nullptr;
  fade[n++] = build_stepper_cell(s_page.group, kColLeftCx, kRow1Y, "Every",
                                 &s_interval, &cap);
  fade[n++] = cap;
  fade[n++] = build_stepper_cell(s_page.group, kColRightCx, kRow1Y, "Points plotted",
                                 &s_points, &cap);
  fade[n++] = cap;
  fade[n++] = build_stepper_cell(s_page.group, kColLeftCx, kRow2Y, "Upload every",
                                 &s_upload, &cap);
  fade[n++] = cap;
  fade[n++] = build_stepper_cell(s_page.group, kColRightCx, kRow2Y, "When dark",
                                 &s_upload_dark, &cap);
  fade[n++] = cap;

  load();
  nav::register_screen(nav::Screen::Recording, s_page.group, fade, n, load);
}

}  // namespace recording

// ===========================================================================
// Sound — the master mute, the alarm volume, and a button to hear both.
// ===========================================================================
namespace sound {

PageChrome   s_page = {};
lv_obj_t*    s_toggle = nullptr;
lv_obj_t*    s_test = nullptr;
StepperState s_volume = {};

constexpr const char* kOn  = "On";
constexpr const char* kOff = "Off";
constexpr const char* kSilentWarning = "alarms will be silent";
constexpr const char* kAudibleFmt    = "tap Test for a %d-second alarm";
constexpr const char* kNoCodec       = "no codec found — alarms will be visual only";

// --- The test button --------------------------------------------------------
// Two seconds is long enough to hear the beep pattern repeat and judge whether the
// volume carries across the room, and short enough that nobody reaches for the
// power switch. It lives here rather than in `audio` because it is a property of
// this button, not of the tone — M7's real alarm runs until the reading recovers.
constexpr int      kTestSeconds = CONFIG_THERMOPOST_AUDIO_TEST_SECONDS;
constexpr uint32_t kTestToneMs = static_cast<uint32_t>(kTestSeconds) * 1000;
constexpr int32_t  kTestW = 160;
constexpr int32_t  kTestH = kToggleH;
// The tone runs on its own task, so the button cannot know it has finished; it
// asks, at a rate a finger cannot outrun.
constexpr uint32_t kTestPollMs = 200;
constexpr const char* kTestIdle = "Test alarm";
constexpr const char* kTestStop = "Stop";

void refresh_test_label() {
  lv_label_set_text(pill_label(s_test), audio::playing() ? kTestStop : kTestIdle);
}

// Greyed and unclickable rather than hidden, so the reason it cannot be used stays
// on screen next to it — the footer says which of the two reasons applies.
void set_test_enabled(bool on) {
  pill_set_color(s_test, on ? kColorText : kColorMuted);
  if (on) {
    lv_obj_add_flag(s_test, LV_OBJ_FLAG_CLICKABLE);
  } else {
    lv_obj_remove_flag(s_test, LV_OBJ_FLAG_CLICKABLE);
  }
}

void refresh_state() {
  const settings::Settings s = settings::get();
  lv_label_set_text(pill_label(s_toggle), s.sound_enabled ? kOn : kOff);
  // Amber when muted. A mute is a setting the user chose, not a fault — but it is
  // the setting most likely to be forgotten about, so it does not get to look
  // identical to the working state.
  pill_set_color(s_toggle, s.sound_enabled ? kColorText : kColorFault);

  // The test follows the mute rather than overriding it. It exists to prove what
  // the alarm will do, and while the alarm is muted the truthful demonstration is
  // silence — a button that beeps anyway would be testing something the device
  // will never actually do.
  const bool have_codec = audio::available();
  set_test_enabled(have_codec && s.sound_enabled);
  refresh_test_label();

  if (!have_codec) {
    lv_label_set_text(s_page.footer, kNoCodec);
  } else if (!s.sound_enabled) {
    lv_label_set_text(s_page.footer, kSilentWarning);
  } else {
    char note[kTextMax];
    std::snprintf(note, sizeof(note), kAudibleFmt, kTestSeconds);
    lv_label_set_text(s_page.footer, note);
  }
}

void on_toggle(lv_event_t*) {
  settings::set_sound_enabled(!settings::get().sound_enabled);
  refresh_state();
}

// A second tap stops the tone rather than queueing another two seconds, which is
// the one thing anyone wants from a button that has just started making a noise.
void on_test(lv_event_t*) {
  if (audio::playing()) {
    audio::stop();
  } else {
    audio::play(kTestToneMs, settings::get().volume_pct);
  }
  refresh_test_label();
}

// Only the label, and only while the page is up: the tone ends on its own task, so
// this is how "Stop" turns back into "Test alarm".
void tick(lv_timer_t*) {
  if (!nav::visible(nav::Screen::Sound)) return;
  refresh_test_label();
}

void on_volume_change() {
  settings::set_volume_pct(static_cast<uint8_t>(s_volume.value));
}

void load() {
  s_volume.value = static_cast<int16_t>(settings::get().volume_pct);
  stepper_refresh(&s_volume);
  refresh_state();
}

void build(lv_obj_t* scr) {
  build_page_chrome(scr, "Sound", &s_page);

  s_volume.min = 0;
  s_volume.max = static_cast<int16_t>(settings::kVolumeMaxPct);
  s_volume.step = kVolumeStepPct;
  s_volume.suffix = "%";
  s_volume.on_change = on_volume_change;

  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_page.back.btn;
  fade[n++] = s_page.heading;
  fade[n++] = s_page.footer;
  lv_obj_t* cap = nullptr;
  s_toggle = build_toggle_cell(s_page.group, kColLeftCx, kRow1Y, "Alarm sound",
                               on_toggle, &cap);
  fade[n++] = s_toggle;
  fade[n++] = cap;
  fade[n++] = build_stepper_cell(s_page.group, kColRightCx, kRow1Y, "Volume",
                                 &s_volume, &cap);
  fade[n++] = cap;

  s_test = build_pill(s_page.group, kTestIdle, kTestW, kTestH, kColorText,
                      kFontTitle, on_test, nullptr);
  place(s_test, kColMidCx, kTestW, kRow2Y + kFontBodyH + kCaptionGap + kPillBandDy);
  fade[n++] = s_test;

  load();
  lv_timer_create(tick, kTestPollMs, nullptr);
  nav::register_screen(nav::Screen::Sound, s_page.group, fade, n, load);
}

}  // namespace sound

}  // namespace


// ===========================================================================
// Power — what the post is allowed to do when nobody is looking at it.
//
// Three modes and one number, because the interesting question is not how long
// to wait but who decides whether waiting is allowed at all. The middle mode is
// the default and infers that from the pack voltage; the other two exist because
// the inference can be wrong and an operator standing in front of the post is a
// better judge than a threshold.
// ===========================================================================
namespace power_page {

PageChrome   s_page = {};
lv_obj_t*    s_mode = nullptr;
lv_obj_t*    s_off = nullptr;
lv_obj_t*    s_pct = nullptr;
lv_obj_t*    s_volts = nullptr;
StepperState s_sleep = {};

void load();

// Row 1 carries two pills, so the mode's label had to get shorter than the mode's
// name. The footer says what each one actually does, which is where the sentence
// belongs anyway — a capsule is a value, not an explanation.
constexpr int32_t kOffW  = 104;
constexpr int32_t kModeW = 150;
constexpr int32_t kRowH  = kToggleH;
// Derived exactly as every other pair on every other page: two hit areas that meet
// in the gap belong to whichever was built last, and here that would be Turn Off
// swallowing taps meant for the mode beside it.
constexpr int32_t kRow1ExtClick = ext_click_for(kOffW, kRowH);
constexpr int32_t kRow1Seam = 2;
constexpr int32_t kRow1Gap = 2 * kRow1ExtClick + kRow1Seam;
constexpr int32_t kRow1W = kOffW + kRow1Gap + kModeW;
constexpr int32_t kRow1X = (kScreenW - kRow1W) / 2;
static_assert(kRow1X >= kEdgePad, "the two row-one pills overhang the screen");
constexpr int32_t kOffX  = kRow1X;
constexpr int32_t kModeX = kRow1X + kOffW + kRow1Gap;
constexpr int32_t kRow1PillY = kRow1Y + kFontBodyH + kCaptionGap + kPillBandDy;

// A caption may be wider than the pill it labels, and here it has to be: at
// Montserrat 14 the hint is wider than a 104 px capsule, and a boxed label that
// overflows *wraps*, which puts a second line where the layout budgeted one and
// clips it. Each caption is allowed the pill's width plus its share of the gutter,
// leaving kRow1Seam between the two. They also ellipsise rather than wrap, so a
// longer wording later degrades to one shortened line instead of reintroducing
// this.
constexpr int32_t kRow1CapPad = (kRow1Gap - kRow1Seam) / 2;
constexpr int32_t kOffCapW  = kOffW + 2 * kRow1CapPad;
constexpr int32_t kModeCapW = kModeW + 2 * kRow1CapPad;
constexpr int32_t kOffCapX  = kOffX - kRow1CapPad;
constexpr int32_t kModeCapX = kModeX - kRow1CapPad;
static_assert(kOffCapX >= kEdgePad, "the Turn Off caption overhangs the screen edge");
static_assert(kOffCapX + kOffCapW + kRow1Seam <= kModeCapX,
              "the two row-one captions meet");

// The mode footers run to a second line, and the shared kFooterY budgets exactly
// one — a wrapped line there renders below the panel and is simply lost. This page
// has the room to start a line higher: row 2's controls end precisely where this
// begins.
constexpr int32_t kFooterLines = 2;
constexpr int32_t kPowerFooterY = kFooterY - kFontBodyH;
static_assert(kPowerFooterY >= kRow2Y + kFontBodyH + kCaptionGap + stepper_height(kStepperCfg),
              "the two-line footer overlaps the sleep stepper's click area");
static_assert(kPowerFooterY + kFooterLines * kFontBodyH <= kScreenH - kEdgePad,
              "the two-line footer overflows the panel");

constexpr const char* kModeAlwaysOn    = "Always on";
constexpr const char* kModeConnected   = "USB on";
constexpr const char* kModeAlwaysSleep = "Always sleep";
constexpr const char* kModeHint = "Screen mode";

// Two taps, because one stray tap would otherwise blank the device until a charger
// turns up. The same shape as the Sound page's Test button: the label is the state,
// so nothing has to be explained beside it.
constexpr const char* kOffIdle    = "Turn Off";
constexpr const char* kOffConfirm = "Sure?";
constexpr const char* kOffHint    = "Touch to wake";
constexpr uint32_t kOffConfirmMs = 4000;
lv_timer_t* s_off_timer = nullptr;

// Sensible sleep delays rather than a fixed step, the same trick the interval
// stepper uses: twelve taps covers ten seconds to an hour, and any fixed step
// over that range is either dozens of taps or too coarse at the fast end.
constexpr uint16_t kSleepChoicesS[] = {10, 15, 20, 30, 45, 60, 90, 120, 300, 600, 1800, 3600};
constexpr int16_t kSleepChoiceCount =
    static_cast<int16_t>(sizeof(kSleepChoicesS) / sizeof(kSleepChoicesS[0]));
static_assert(kSleepChoicesS[0] >= settings::kSleepAfterMinS,
              "the shortest offered sleep delay is below what settings accepts");
static_assert(kSleepChoicesS[kSleepChoiceCount - 1] <= settings::kSleepAfterMaxS,
              "the longest offered sleep delay is above what settings accepts");

constexpr const char* kNoBattery = "--";
constexpr const char* kNoPack    = "no pack";
constexpr const char* kFooterNeverSleeps  = "stays awake; the screen still dims and blanks";
constexpr const char* kFooterOnBattery    = "sleeps after %s on battery, never on USB";
constexpr const char* kFooterAlwaysSleeps = "sleeps after %s, on any supply";
// The one thing this page must not be coy about. Two of the three modes are mostly
// promises until the post can put itself to sleep, and a settings page that let
// them look finished would be the page lying about the device. Says "on its own"
// because Turn Off, one button down this same page, deep-sleeps for real.
constexpr const char* kFooterNotArmed = "won't sleep on its own yet — dims and blanks only";
constexpr const char* kFooterOffConfirm = "tap again to switch off; a touch wakes it";

int16_t sleep_index_for(uint16_t seconds) {
  for (int16_t i = 0; i < kSleepChoiceCount; ++i) {
    if (kSleepChoicesS[i] >= seconds) return i;
  }
  return kSleepChoiceCount - 1;
}

void format_sleep(char* out, size_t n, int16_t index) {
  if (index < 0 || index >= kSleepChoiceCount) return;
  format_span(out, n, kSleepChoicesS[index]);
}

const char* mode_label(settings::PowerMode m) {
  switch (m) {
    case settings::PowerMode::AlwaysOn:    return kModeAlwaysOn;
    case settings::PowerMode::AlwaysSleep: return kModeAlwaysSleep;
    default:                               return kModeConnected;
  }
}

void refresh_footer() {
  const settings::Settings s = settings::get();
  if (s.power_mode == settings::PowerMode::AlwaysOn) {
    lv_label_set_text(s_page.footer, kFooterNeverSleeps);
    return;
  }
  if (!power::auto_sleep_armed()) {
    lv_label_set_text(s_page.footer, kFooterNotArmed);
    return;
  }
  char span[kSpanTextMax];
  format_span(span, sizeof(span), s.sleep_after_s);
  char note[kTextMax];
  std::snprintf(note, sizeof(note),
                s.power_mode == settings::PowerMode::AlwaysSleep ? kFooterAlwaysSleeps
                                                                 : kFooterOnBattery,
                span);
  lv_label_set_text(s_page.footer, note);
}

// The number and the voltage it was derived from, one above the other. Both,
// deliberately: the percentage is an estimate off a published lithium curve and
// the volts are the measurement, and showing only the friendly one would hide
// which of the two can be trusted.
void refresh_battery() {
  const power::Battery b = power::battery();
  if (!b.present || !b.calibrated) {
    lv_label_set_text(s_pct, kNoBattery);
    lv_label_set_text(s_volts, kNoPack);
    lv_obj_set_style_text_color(s_pct, kColorMuted, LV_PART_MAIN);
    return;
  }
  char text[kTextMax];
  std::snprintf(text, sizeof(text), "%d%%%s", b.pct,
                power::on_external_power() ? " " LV_SYMBOL_CHARGE : "");
  lv_label_set_text(s_pct, text);
  lv_obj_set_style_text_color(s_pct, b.pct < 25 ? kColorFault : kColorText, LV_PART_MAIN);

  std::snprintf(text, sizeof(text), "%d.%02d V est.", b.pack_mv / 1000,
                (b.pack_mv % 1000) / 10);
  lv_label_set_text(s_volts, text);
}

void reset_off_button() {
  if (s_off_timer != nullptr) {
    lv_timer_delete(s_off_timer);
    s_off_timer = nullptr;
  }
  lv_label_set_text(pill_label(s_off), kOffIdle);
  pill_set_color(s_off, kColorText);
  refresh_footer();
}

void on_off_timeout(lv_timer_t*) {
  s_off_timer = nullptr;
  reset_off_button();
}

void on_off_tap(lv_event_t*) {
  if (s_off_timer != nullptr) {
    // Armed, so this is the second tap. Never returns.
    power::turn_off();
  }
  lv_label_set_text(pill_label(s_off), kOffConfirm);
  pill_set_color(s_off, kColorFault);
  lv_label_set_text(s_page.footer, kFooterOffConfirm);
  s_off_timer = lv_timer_create(on_off_timeout, kOffConfirmMs, nullptr);
  lv_timer_set_repeat_count(s_off_timer, 1);
}

void on_mode_tap(lv_event_t*) {
  const settings::PowerMode cur = settings::get().power_mode;
  const settings::PowerMode next =
      cur == settings::PowerMode::AlwaysSleep
          ? settings::PowerMode::AlwaysOn
          : static_cast<settings::PowerMode>(static_cast<uint8_t>(cur) + 1);
  settings::set_power_mode(next);
  // Straight back to the top of the ladder: a mode changed from a dimming screen
  // should not leave the screen dimming while the reader checks what it did.
  power::wake();
  load();
}

void on_sleep_change() {
  settings::set_sleep_after_s(kSleepChoicesS[s_sleep.value]);
  refresh_footer();
}

void tick(lv_timer_t*) {
  if (!nav::visible(nav::Screen::Power)) return;
  refresh_battery();
}

void load() {
  const settings::Settings s = settings::get();
  lv_label_set_text(pill_label(s_mode), mode_label(s.power_mode));
  s_sleep.value = sleep_index_for(s.sleep_after_s);
  stepper_refresh(&s_sleep);
  refresh_battery();
  // Also cancels a half-armed Turn Off, so leaving the page and coming back can
  // never land on a screen where one tap switches the post off.
  reset_off_button();
}

void build(lv_obj_t* scr) {
  build_page_chrome(scr, "Power", &s_page);

  // This page's footer says what a mode actually does, which is a sentence rather
  // than a phrase. Lifted a line so the wrap has somewhere to go.
  lv_obj_set_pos(s_page.footer, kEdgePad, kPowerFooterY);

  s_sleep.min = 0;
  s_sleep.max = kSleepChoiceCount - 1;
  s_sleep.step = 1;
  s_sleep.suffix = "";
  s_sleep.format = format_sleep;
  s_sleep.on_change = on_sleep_change;

  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_page.back.btn;
  fade[n++] = s_page.heading;
  fade[n++] = s_page.footer;

  // --- row 1: Turn Off, then the mode
  lv_obj_t* off_cap = make_boxed_label(s_page.group, kFontBody, kColorSubText, kOffCapW,
                                       LV_TEXT_ALIGN_CENTER);
  lv_label_set_long_mode(off_cap, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_text(off_cap, kOffHint);
  lv_obj_set_pos(off_cap, kOffCapX, kRow1Y);
  fade[n++] = off_cap;
  s_off = build_pill(s_page.group, kOffIdle, kOffW, kRowH, kColorText, kFontTitle,
                     on_off_tap, nullptr);
  lv_obj_set_pos(s_off, kOffX, kRow1PillY);
  fade[n++] = s_off;

  lv_obj_t* mode_cap = make_boxed_label(s_page.group, kFontBody, kColorSubText, kModeCapW,
                                        LV_TEXT_ALIGN_CENTER);
  lv_label_set_long_mode(mode_cap, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_text(mode_cap, kModeHint);
  lv_obj_set_pos(mode_cap, kModeCapX, kRow1Y);
  fade[n++] = mode_cap;
  s_mode = build_pill(s_page.group, mode_label(settings::kPowerModeDefault), kModeW,
                      kRowH, kColorText, kFontTitle, on_mode_tap, nullptr);
  lv_obj_set_pos(s_mode, kModeX, kRow1PillY);
  fade[n++] = s_mode;

  // --- row 2: the sleep delay, and a readout that is not a control
  lv_obj_t* cap = nullptr;
  fade[n++] = build_stepper_cell(s_page.group, kColLeftCx, kRow2Y, "Sleep after",
                                 &s_sleep, &cap);
  fade[n++] = cap;

  // The only cell on any page that cannot be tapped. It is here because this is the
  // page someone opens when they are thinking about the battery, and a number you
  // need a serial console for is a number nobody looks at.
  fade[n++] = build_caption(s_page.group, kColRightCx, kRow2Y, "Battery");
  const int32_t readout_y = kRow2Y + kFontBodyH + kCaptionGap;
  s_pct = make_boxed_label(s_page.group, kFontTitle, kColorText, kCellW,
                           LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_pct, kColRightCx - kCellW / 2, readout_y);
  fade[n++] = s_pct;
  s_volts = make_boxed_label(s_page.group, kFontBody, kColorMuted, kCellW,
                             LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_volts, kColRightCx - kCellW / 2, readout_y + kFontTitleH);
  fade[n++] = s_volts;
  static_assert(kRow2Y + kFontBodyH + kCaptionGap + kFontTitleH + kFontBodyH <= kFooterY,
                "the battery readout's second line runs into the footer");

  load();
  lv_timer_create(tick, kBatteryPollMs, nullptr);
  nav::register_screen(nav::Screen::Power, s_page.group, fade, n, load);
}

}  // namespace power_page

void build_all(lv_obj_t* scr) {
  temperature::build(scr);
  recording::build(scr);
  sound::build(scr);
  power_page::build(scr);
}

}  // namespace thermopost::ui::settings_screen
