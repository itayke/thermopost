#include "ui_settings.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "audio.hpp"
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
// is the smaller source rather than a bigger target. Wide enough for the "%luh %lum"
// worst case the compiler has to assume, not just the ~5 characters it can produce.
constexpr size_t kSpanTextMax = 32;

// --- Shared page geometry ---------------------------------------------------
// Two columns, two rows. A cell is a caption with its control underneath, and the
// cell is as wide as the widest control it can hold — the stepper, click area
// included — so a caption can never be wider than the thing it labels.
constexpr StepperCfg kStepperCfg = {&lv_font_montserrat_24, 50, 34};

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

constexpr int32_t kFooterY = kScreenH - kEdgePad - kFont14H;

// Checked here rather than discovered on the panel: a stepper that overhangs the
// screen edge loses the half of its click area that hangs off, which reads as a
// disc that only works on one side.
constexpr int32_t kCellW = stepper_width(kStepperCfg);
static_assert(kColLeftCx - kCellW / 2 >= 0, "the left column overhangs the screen");
static_assert(kColRightCx + kCellW / 2 <= kScreenW, "the right column overhangs the screen");
static_assert(kRow2Y + kFont14H + kCaptionGap + stepper_height(kStepperCfg) <= kFooterY,
              "the second control row runs into the footer");

// --- Threshold coupling -----------------------------------------------------
// Minimum gap between the two thresholds, in whole display degrees. One is enough:
// the point is only that the pair cannot cross or collapse to a zero-width band,
// which would leave alert::sanitize() with nothing to work with.
constexpr int16_t kBandMinDeg = 1;

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

constexpr uint16_t kGraphPointsStep = 30;
constexpr uint8_t  kVolumeStepPct   = 5;

constexpr uint32_t kSecPerMin  = 60;
constexpr uint32_t kSecPerHour = 3600;

// --- Shared chrome ----------------------------------------------------------
struct PageChrome {
  lv_obj_t* group;
  NavPill   back;
  lv_obj_t* heading;
  lv_obj_t* footer;
};

void on_back(lv_event_t*) { nav::go(nav::Screen::Menu); }

// Fills the caller's PageChrome in place rather than returning one. It has to: the
// nav pill's painter keeps a pointer to the NavPill it was built against, so a
// chrome built into a local and then copied out would leave that painter reading a
// destroyed stack frame on the first redraw.
void build_chrome(lv_obj_t* scr, const char* title, PageChrome* c) {
  c->group = build_group(scr);
  build_nav_pill(c->group, &c->back, NavGlyph::Chevron, "Back", on_back);

  c->heading = make_boxed_label(c->group, &lv_font_montserrat_24, kColorText, kHeadingW,
                                LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(c->heading, title);
  lv_obj_set_pos(c->heading, kEdgePad + kNavPillW, kHeadingY);

  c->footer = make_boxed_label(c->group, &lv_font_montserrat_14, kColorMuted,
                               kScreenW - 2 * kEdgePad, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(c->footer, kEdgePad, kFooterY);
}

// Centre a widget of known width on `cx`, top edge at `y`. The width is passed in
// rather than read back with lv_obj_get_width: coords are only reliable after a
// layout pass, and every control here has a width its caller already knows.
void place(lv_obj_t* obj, int32_t cx, int32_t w, int32_t y) {
  lv_obj_set_pos(obj, cx - w / 2, y);
}

lv_obj_t* build_caption(lv_obj_t* group, int32_t cx, int32_t y, const char* text) {
  lv_obj_t* l = make_boxed_label(group, &lv_font_montserrat_14, kColorSubText, kCellW,
                                 LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(l, text);
  lv_obj_set_pos(l, cx - kCellW / 2, y);
  return l;
}

// A stepper cell: caption, then the control under it. Returns the control's
// container so the caller can add it to the fade set.
lv_obj_t* build_stepper_cell(lv_obj_t* group, int32_t cx, int32_t y, const char* caption,
                             StepperState* st, lv_obj_t** out_caption) {
  *out_caption = build_caption(group, cx, y, caption);
  lv_obj_t* row = build_stepper(group, st, kStepperCfg);
  place(row, cx, stepper_width(kStepperCfg), y + kFont14H + kCaptionGap);
  return row;
}

// A toggle cell: same caption, and a capsule whose label *is* the current value.
// A switch would need a legend to say which end is which; a capsule reading "°F"
// says it outright, and the tap target is the whole word.
lv_obj_t* build_toggle_cell(lv_obj_t* group, int32_t cx, int32_t y, const char* caption,
                            lv_event_cb_t on_tap, lv_obj_t** out_caption) {
  *out_caption = build_caption(group, cx, y, caption);
  lv_obj_t* pill = build_pill(group, "", kToggleW, kToggleH, kColorText,
                              &lv_font_montserrat_24, on_tap, nullptr);
  place(pill, cx, kToggleW, y + kFont14H + kCaptionGap + kPillBandDy);
  return pill;
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
// Thresholds — the alarm band and its recovery margin.
// ===========================================================================
namespace thresholds {

PageChrome    s_page = {};
StepperState  s_min = {};
StepperState  s_max = {};
StepperState  s_hyst = {};

// Largest whole-degree hysteresis whose *stored* value still fits under
// alert::sanitize()'s half-band cap. Rounded down and then verified, because
// rounding the cap to the nearest degree can land a degree above it — and a stepper
// whose top value gets silently clamped on its way to NVS is precisely the failure
// this coupling exists to prevent.
int16_t hysteresis_ceiling_deg(int16_t min_c100, int16_t max_c100) {
  const int16_t cap = static_cast<int16_t>((max_c100 - min_c100) / 2);
  int16_t deg = settings::display_span_deg(cap);
  while (deg > 0 && settings::c100_from_span_deg(deg) > cap) --deg;
  return deg;
}

void refresh_footer() {
  const settings::Settings s = settings::get();
  char text[kTextMax];
  // The stored hundredths, plainly, because the steppers above show whole degrees:
  // a band set in °F is not a round number of °C, and this is where that shows
  // rather than looking like drift.
  // The margin is a span, so it is the difference between two converted points
  // rather than one converted value — 0.5 °C of hysteresis is 0.9 °F of it, not
  // 32.9.
  const int16_t hyst_ref = static_cast<int16_t>(s.min_c100 + s.hysteresis_c100);
  std::snprintf(text, sizeof(text), "stored %.1f to %.1f, clears by %.1f %s",
                static_cast<double>(settings::display_value(s.min_c100)),
                static_cast<double>(settings::display_value(s.max_c100)),
                static_cast<double>(settings::display_value(hyst_ref) -
                                    settings::display_value(s.min_c100)),
                settings::unit_suffix());
  lv_label_set_text(s_page.footer, text);
}

// Re-derive every bound from what is actually stored. Called on show and after each
// tap, so the coupling holds no matter which of the three moved.
void rebound() {
  const settings::Settings s = settings::get();
  const int16_t min_deg = settings::display_deg(s.min_c100);
  const int16_t max_deg = settings::display_deg(s.max_c100);

  s_min.min = settings::display_deg(settings::kTempFloorC100);
  s_min.max = static_cast<int16_t>(max_deg - kBandMinDeg);
  s_max.min = static_cast<int16_t>(min_deg + kBandMinDeg);
  s_max.max = settings::display_deg(settings::kTempCeilC100);
  s_hyst.min = 0;
  s_hyst.max = hysteresis_ceiling_deg(s.min_c100, s.max_c100);

  // Only the hysteresis is clamped here. A narrowed band can leave the stored value
  // above its own new ceiling; sanitize() would cap it for the alarm logic anyway,
  // so writing the capped value now means the stepper and NVS agree rather than the
  // screen showing a number the state machine has quietly stopped using.
  if (stepper_clamp(&s_hyst)) {
    settings::set_hysteresis_c100(settings::c100_from_span_deg(s_hyst.value));
  }

  // The two thresholds are deliberately NOT clamped to match. Two stored values a
  // fraction apart — 24.5 and 25.0 — round to the same whole degree, which leaves
  // the minimum sitting one above its own ceiling. Pulling the display down to
  // satisfy the bound would show a number that isn't what is stored; leaving it
  // alone just means (+) has nowhere to go, which is the truth. The footer prints
  // the stored hundredths, so the apparently-equal pair is explained rather than
  // mysterious.
}

// Each stepper writes only its own field, taking its sibling from what is stored
// rather than from the sibling stepper. The difference matters: a stored 20.5 °C
// minimum displays as 21, and writing that back while the user was only touching
// the maximum would move a threshold they never touched.
void on_min_change() {
  settings::set_band(settings::c100_from_deg(s_min.value), settings::get().max_c100);
  rebound();
  stepper_refresh(&s_hyst);
  refresh_footer();
}

void on_max_change() {
  settings::set_band(settings::get().min_c100, settings::c100_from_deg(s_max.value));
  rebound();
  stepper_refresh(&s_hyst);
  refresh_footer();
}

void on_hyst_change() {
  settings::set_hysteresis_c100(settings::c100_from_span_deg(s_hyst.value));
  refresh_footer();
}

void load() {
  const settings::Settings s = settings::get();
  const char* suffix = settings::unit_suffix();
  s_min.suffix = suffix;
  s_max.suffix = suffix;
  // No suffix on a span: "0.5 °C" would be right but "1°C" for a margin reads as a
  // temperature rather than a distance, and the caption already says which it is.
  s_hyst.suffix = "";

  s_min.value  = settings::display_deg(s.min_c100);
  s_max.value  = settings::display_deg(s.max_c100);
  s_hyst.value = settings::display_span_deg(s.hysteresis_c100);

  rebound();
  stepper_refresh(&s_min);
  stepper_refresh(&s_max);
  stepper_refresh(&s_hyst);
  refresh_footer();
}

void build(lv_obj_t* scr) {
  build_chrome(scr, "Thresholds", &s_page);

  s_min.step = 1;
  s_min.on_change = on_min_change;
  s_max.step = 1;
  s_max.on_change = on_max_change;
  s_hyst.step = 1;
  s_hyst.on_change = on_hyst_change;

  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_page.back.btn;
  fade[n++] = s_page.heading;
  fade[n++] = s_page.footer;
  lv_obj_t* cap = nullptr;
  fade[n++] = build_stepper_cell(s_page.group, kColLeftCx, kRow1Y, "Alarm below",
                                 &s_min, &cap);
  fade[n++] = cap;
  fade[n++] = build_stepper_cell(s_page.group, kColRightCx, kRow1Y, "Alarm above",
                                 &s_max, &cap);
  fade[n++] = cap;
  fade[n++] = build_stepper_cell(s_page.group, kColMidCx, kRow2Y, "Recovery margin",
                                 &s_hyst, &cap);
  fade[n++] = cap;

  load();
  nav::register_screen(nav::Screen::Thresholds, s_page.group, fade, n, load);
}

}  // namespace thresholds

// ===========================================================================
// Recording — how often a record is written, and how many are plotted.
// ===========================================================================
namespace recording {

PageChrome   s_page = {};
StepperState s_interval = {};
StepperState s_points = {};

// Nearest offered interval to what is stored, so a value written by the web form (or
// an older firmware) lands on a real stepper position instead of snapping to the
// bottom of the table.
int16_t nearest_interval_index(uint16_t seconds) {
  int16_t best = 0;
  int32_t best_err = INT32_MAX;
  for (int16_t i = 0; i < kIntervalChoiceCount; ++i) {
    const int32_t err = std::abs(static_cast<int32_t>(kIntervalChoicesS[i]) - seconds);
    if (err < best_err) {
      best_err = err;
      best = i;
    }
  }
  return best;
}

void format_interval(char* out, size_t n, int16_t index) {
  const int16_t i = static_cast<int16_t>(
      std::clamp<int16_t>(index, 0, kIntervalChoiceCount - 1));
  format_span(out, n, kIntervalChoicesS[i]);
}

void refresh_footer() {
  const settings::Settings s = settings::get();
  char span[kSpanTextMax];
  format_span(span, sizeof(span),
              static_cast<uint32_t>(s.graph_points) * s.interval_s);
  char text[kTextMax];
  std::snprintf(text, sizeof(text), "the graph spans %s", span);
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

void load() {
  const settings::Settings s = settings::get();
  s_interval.value = nearest_interval_index(s.interval_s);
  s_points.value = static_cast<int16_t>(s.graph_points);
  stepper_refresh(&s_interval);
  stepper_refresh(&s_points);
  refresh_footer();
}

void build(lv_obj_t* scr) {
  build_chrome(scr, "Recording", &s_page);

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
  build_chrome(scr, "Sound", &s_page);

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
                      &lv_font_montserrat_24, on_test, nullptr);
  place(s_test, kColMidCx, kTestW, kRow2Y + kFont14H + kCaptionGap + kPillBandDy);
  fade[n++] = s_test;

  load();
  lv_timer_create(tick, kTestPollMs, nullptr);
  nav::register_screen(nav::Screen::Sound, s_page.group, fade, n, load);
}

}  // namespace sound

// ===========================================================================
// Display — the unit, and deliberately sparse. M9's backlight and idle timings
// land here, which is why it is a page of its own rather than a row elsewhere.
// ===========================================================================
namespace display_page {

PageChrome s_page = {};
lv_obj_t*  s_toggle = nullptr;

// The readout on the main screen toggles the unit too. A menu entry is how anyone
// finds that out, so the footer says it rather than leaving the shortcut hidden.
constexpr const char* kTapHint = "tapping the reading does this too";

void refresh_state() {
  lv_label_set_text(pill_label(s_toggle), settings::unit_suffix());
  lv_label_set_text(s_page.footer, kTapHint);
}

void on_toggle(lv_event_t*) {
  const settings::TempUnit next = settings::get().unit == settings::TempUnit::Celsius
                                      ? settings::TempUnit::Fahrenheit
                                      : settings::TempUnit::Celsius;
  settings::set_unit(next);
  refresh_state();
}

void load() { refresh_state(); }

void build(lv_obj_t* scr) {
  build_chrome(scr, "Display", &s_page);

  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_page.back.btn;
  fade[n++] = s_page.heading;
  fade[n++] = s_page.footer;
  lv_obj_t* cap = nullptr;
  s_toggle = build_toggle_cell(s_page.group, kColMidCx, kRow1Y, "Temperature unit",
                               on_toggle, &cap);
  fade[n++] = s_toggle;
  fade[n++] = cap;

  load();
  nav::register_screen(nav::Screen::Display, s_page.group, fade, n, load);
}

}  // namespace display_page

}  // namespace

void build_all(lv_obj_t* scr) {
  thresholds::build(scr);
  recording::build(scr);
  sound::build(scr);
  display_page::build(scr);
}

}  // namespace thermopost::ui::settings_screen
