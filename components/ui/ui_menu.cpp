#include "ui_menu.hpp"

#include <cstdint>
#include <cstdio>

#include "recorder.hpp"
#include "settings.hpp"
#include "ui_chrome.hpp"
#include "ui_nav.hpp"
#include "ui_theme.hpp"

namespace thermopost::ui::menu_screen {
namespace {

constexpr size_t kTextMax = 48;
constexpr const char* kNoClock = "clock not set";
constexpr const char* kClockFmt = "%a %d %b   %H:%M";
// Sized to its own longest rendering ("Tue 28 Jul   14:32") rather than reusing
// kTextMax: formatting a 48-byte buffer into another 48-byte one is a truncation
// GCC is right to refuse, and the honest fix is the smaller source, not a bigger
// target.
constexpr size_t kClockTextMax = 24;

// The name is read-only here, and this says why rather than leaving the user to
// hunt for the edit that isn't there. It is the one free-text setting, and an
// on-screen keyboard on a 2.8" panel would be the worst part of the product.
constexpr const char* kRenameHint = "rename from the setup page";

// One row per entry, so adding a screen is a row here rather than an edit in three
// places. Order is grid order: left to right, then top to bottom. Declared above the
// layout because the grid's geometry is derived from how many there are.
struct Entry {
  const char*  text;
  nav::Screen  target;
};

constexpr Entry kEntries[] = {
    {"Thresholds",  nav::Screen::Thresholds},
    {"Recording",   nav::Screen::Recording},
    {"Sound",       nav::Screen::Sound},
    {"Display",     nav::Screen::Display},
    {"Network",     nav::Screen::Network},
};
constexpr int kEntryCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));
// Back pill, name, hint, footer, plus one per entry.
static_assert(4 + kEntryCount <= nav::kMaxFadeWidgets,
              "the menu's fade set no longer fits nav::kMaxFadeWidgets");

// --- Layout -----------------------------------------------------------------
// Heading + hint at the top, a grid of entry pills, one status line along the
// bottom. Two columns rather than one because four stacked pills tall enough to hit
// reliably do not fit under a heading on a 240 px panel.
constexpr int32_t kHintY = kHeadingY + kFont24H + 2;
// The hint spans the full width, so it has to start below the nav pill rather than
// beside it — a full-width line at the heading's height would run under the pill.
static_assert(kHintY >= kNavPillY + kNavPillH,
              "a full-width hint at this height would run under the nav pill");

constexpr int32_t kGridCols = 2;

// The shared text-button height. A fifth shorter than the grid launched with: the
// capsule is a fill now, so it reads as a target from its colour rather than from its
// bulk, and the height it gives up is what lets a third row sit under the hint
// without crowding it.
constexpr int32_t kEntryH = kPillH;

// A cell's hit margin is set by its height, because a cell is always wider than it is
// tall and ext_click_area keys off the smaller side. Taking it from the height alone
// breaks what would otherwise be a circle — the gutters below are derived from this,
// and the width is derived from the gutters. The assert under kEntryW is what keeps
// that shortcut honest.
constexpr int32_t kEntryExtClick = ext_click_for(kEntryH, kEntryH);

// Gutters, not padding. Each has to clear twice a cell's extended click area, or the
// two hit areas meet inside the gap and it belongs to whichever cell was built last —
// a tap near a seam opening the wrong page, which reads as a flaky touch panel rather
// than as arithmetic. Derived rather than typed, so raising kExtClickPad widens these
// instead of breaking them; kGridSeam is the dead strip deliberately left between the
// two hit areas, and one pixel would do — two is legible in a screenshot.
constexpr int32_t kGridSeam   = 2;
constexpr int32_t kGridColGap = 2 * kEntryExtClick + kGridSeam;
constexpr int32_t kGridRowGap = kGridColGap;

constexpr int32_t kEntryW = (kScreenW - 2 * kEdgePad - kGridColGap) / kGridCols;
static_assert(kEntryW > kEntryH,
              "a cell taller than it is wide takes its hit margin from the width, "
              "which kEntryExtClick above assumes it never does");
// The grid is centred in whatever is left between the hint and the footer rather than
// pinned below the hint, so changing kEntryH or adding a row rebalances the screen
// instead of piling the slack at the bottom.
constexpr int32_t kGridTop = kHintY + kFont14H;
// Rounded up, because an odd entry count is allowed and takes a row of its own,
// centred. The alternatives are worse: a hole beside it reads as a button that
// failed to draw, and padding the list with a page nobody asked for is a page
// nobody asked for.
constexpr int32_t kGridRows = (kEntryCount + kGridCols - 1) / kGridCols;
constexpr int32_t grid_height(int32_t rows) {
  return rows * kEntryH + (rows - 1) * kGridRowGap;
}
constexpr int32_t kGridH = grid_height(kGridRows);
constexpr int32_t kGridY = kGridTop + (kFooterY - kGridTop - kGridH) / 2;
static_assert(kGridY >= kGridTop, "the entry grid does not fit between the hint and the footer");

// Network took the grid to three rows, which this had already checked before the
// page existed — so it cost a row in kEntries[] and nothing else. An About page would
// fill the gap beside it at the same price. Checking a taller grid than is built is
// what keeps the squeeze from being discovered while writing a screen, which is where
// a hint line quietly gets deleted to make room.
constexpr int32_t kGridRowsPlanned = 3;
static_assert(kGridTop + grid_height(kGridRowsPlanned) <= kFooterY,
              "a third row of entries would not fit under the hint");

// Refreshed once a second while the screen is up.
constexpr uint32_t kInfoRefreshMs = 1000;

NavPill   s_back = {};
lv_obj_t* s_group   = nullptr;
lv_obj_t* s_name    = nullptr;
lv_obj_t* s_footer  = nullptr;
lv_obj_t* s_entries[kEntryCount] = {};

void on_back(lv_event_t*) { nav::go(nav::Screen::Main); }

// The target comes from the pill's user_data, so one handler serves every entry.
void on_entry(lv_event_t* e) {
  const auto target = static_cast<nav::Screen>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
  nav::go(target);
}

// Name + clock + record count. Called at the swap midpoint and then once a second,
// but only while the screen is up — polling to reformat labels nobody can see is
// exactly the kind of work a 250 ms UI timer should not be doing.
void refresh_info() {
  lv_label_set_text(s_name, settings::get().name);

  char clock[kClockTextMax];
  if (!format_clock(clock, sizeof(clock), kClockFmt)) {
    std::snprintf(clock, sizeof(clock), "%s", kNoClock);
  }
  char text[kTextMax];
  std::snprintf(text, sizeof(text), "%s   %lu records", clock,
                static_cast<unsigned long>(recorder::total()));
  lv_label_set_text(s_footer, text);
}

void tick(lv_timer_t*) {
  if (!nav::visible(nav::Screen::Menu)) return;
  refresh_info();
}

}  // namespace

void build(lv_obj_t* scr) {
  s_group = build_group(scr);

  build_nav_pill(s_group, &s_back, NavGlyph::Chevron, "Back", on_back);

  // Boxed and ellipsised: 24 characters of name at Montserrat 24 is wider than the
  // band beside the pill, and a name that wraps would push the grid down.
  s_name = make_boxed_label(s_group, &lv_font_montserrat_24, kColorText, kHeadingW,
                            LV_TEXT_ALIGN_CENTER);
  lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_name, kEdgePad + kNavPillW, kHeadingY);

  // Full width, not the heading's band: the hint is longer than 148 px at this
  // font and wrapped onto a second line that sat on top of the first row of
  // entries. It clears the nav pill by sitting below it, so the whole line is
  // available. DOTS as well as the width, so a longer wording ellipsises on one
  // line rather than quietly reintroducing the overlap.
  lv_obj_t* hint = make_boxed_label(s_group, &lv_font_montserrat_14, kColorMuted,
                                    kScreenW - 2 * kEdgePad, LV_TEXT_ALIGN_CENTER);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_text(hint, kRenameHint);
  lv_obj_set_pos(hint, kEdgePad, kHintY);

  for (int i = 0; i < kEntryCount; ++i) {
    const int32_t col = i % kGridCols;
    const int32_t row = i / kGridCols;
    // A lone final entry centres on the screen rather than sitting in column 0.
    const bool lone = (i == kEntryCount - 1) && (kEntryCount % kGridCols == 1);
    const int32_t x = lone ? (kScreenW - kEntryW) / 2
                           : kEdgePad + col * (kEntryW + kGridColGap);
    s_entries[i] = build_pill(
        s_group, kEntries[i].text, kEntryW, kEntryH, kColorText,
        &lv_font_montserrat_24, on_entry,
        reinterpret_cast<void*>(static_cast<uintptr_t>(kEntries[i].target)));
    lv_obj_set_pos(s_entries[i], x, kGridY + row * (kEntryH + kGridRowGap));
  }

  s_footer = make_boxed_label(s_group, &lv_font_montserrat_14, kColorMuted,
                              kScreenW - 2 * kEdgePad, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_pos(s_footer, kEdgePad, kFooterY);

  refresh_info();
  lv_timer_create(tick, kInfoRefreshMs, nullptr);

  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_back.btn;
  fade[n++] = s_name;
  fade[n++] = hint;
  for (int i = 0; i < kEntryCount; ++i) fade[n++] = s_entries[i];
  fade[n++] = s_footer;
  nav::register_screen(nav::Screen::Menu, s_group, fade, n, refresh_info);
}

}  // namespace thermopost::ui::menu_screen
