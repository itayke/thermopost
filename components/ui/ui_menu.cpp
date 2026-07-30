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
    {"Thresholds", nav::Screen::Thresholds},
    {"Recording",  nav::Screen::Recording},
    {"Sound",      nav::Screen::Sound},
    {"Display",    nav::Screen::Display},
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
constexpr int32_t kFooterY = kScreenH - kEdgePad - kFont14H;

constexpr int32_t kGridCols = 2;
constexpr int32_t kGridColGap = 12;
constexpr int32_t kGridRowGap = 14;
constexpr int32_t kEntryW = (kScreenW - 2 * kEdgePad - kGridColGap) / kGridCols;
constexpr int32_t kEntryH = 44;
// Softly rounded rather than a full capsule. At 44 px a half-height radius turns the
// pill into a lozenge whose ends eat into the label's space; these read as buttons in
// a grid, which is what they are, and it separates them from the navigation chrome —
// where the capsule shape is the affordance.
constexpr int32_t kEntryRadius = 10;
static_assert(kEntryRadius <= kEntryH / 2, "a radius past half the height is just a capsule");
static_assert(kEntryCount % kGridCols == 0,
              "an odd entry leaves a hole in the grid; give it a full row or add one");

// The grid is centred in whatever is left between the hint and the footer rather than
// pinned below the hint, so changing kEntryH or adding a row rebalances the screen
// instead of piling the slack at the bottom.
constexpr int32_t kGridRows = kEntryCount / kGridCols;
constexpr int32_t kGridH = kGridRows * kEntryH + (kGridRows - 1) * kGridRowGap;
constexpr int32_t kGridTop = kHintY + kFont14H;
constexpr int32_t kGridY = kGridTop + (kFooterY - kGridTop - kGridH) / 2;
static_assert(kGridY >= kGridTop, "the entry grid does not fit between the hint and the footer");

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

  lv_obj_t* hint = make_boxed_label(s_group, &lv_font_montserrat_14, kColorMuted,
                                    kHeadingW, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(hint, kRenameHint);
  lv_obj_set_pos(hint, kEdgePad + kNavPillW, kHintY);

  for (int i = 0; i < kEntryCount; ++i) {
    const int32_t col = i % kGridCols;
    const int32_t row = i / kGridCols;
    s_entries[i] = build_pill(
        s_group, kEntries[i].text, kEntryW, kEntryH, kColorText,
        &lv_font_montserrat_24, on_entry,
        reinterpret_cast<void*>(static_cast<uintptr_t>(kEntries[i].target)));
    // build_pill makes a full capsule; these are grid buttons, so the corners come
    // back in afterwards rather than threading a radius through every caller that
    // does want a capsule.
    lv_obj_set_style_radius(s_entries[i], kEntryRadius, LV_PART_MAIN);
    lv_obj_set_pos(s_entries[i], kEdgePad + col * (kEntryW + kGridColGap),
                   kGridY + row * (kEntryH + kGridRowGap));
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
