#include "ui_nav.hpp"

#include "esp_log.h"
#include "ui_theme.hpp"

namespace thermopost::ui::nav {
namespace {

constexpr const char* kTag = "ui.nav";

// Per phase, so a full swap takes twice this. Espressopost's value, which reads as
// deliberate rather than sluggish on a panel this size.
constexpr uint32_t kFadeMs = 140;

constexpr int kScreenCount = static_cast<int>(Screen::kCount);

struct Entry {
  lv_obj_t* group;
  lv_obj_t* fade[kMaxFadeWidgets];
  int       fade_n;
  void (*on_show)();
};

Entry s_screens[kScreenCount] = {};
Screen s_current = Screen::Main;

// Transparent full-screen click-eater, normally hidden. Brought to the foreground
// for the length of a swap so a stray tap cannot land on a screen that is halfway
// through fading out — on espressopost that meant a Submit landing on a form that
// was already gone.
lv_obj_t* s_block = nullptr;

// In-flight swap. One at a time (input is blocked and go() refuses to re-enter),
// so a single context is enough.
struct SwapCtx {
  Screen from;
  Screen to;
};
SwapCtx s_swap = {};
bool s_swapping = false;

// No-op animation target — see the header on why the phase clock is its own anim.
int32_t s_driver = 0;

bool valid(Screen s) { return static_cast<int>(s) < kScreenCount; }

Entry& entry(Screen s) { return s_screens[static_cast<int>(s)]; }

void opa_exec(void* var, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(v),
                       LV_PART_MAIN);
}

// Set a uniform opacity across a fade set, skipping hidden widgets so their own
// visibility and opacity are left exactly as their screen arranged them.
void set_opa(const Entry& e, int32_t opa) {
  for (int i = 0; i < e.fade_n; ++i) {
    lv_obj_t* w = e.fade[i];
    if (w == nullptr || lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_obj_set_style_opa(w, static_cast<lv_opa_t>(opa), LV_PART_MAIN);
  }
}

void fade_set(const Entry& e, int32_t from, int32_t to, lv_anim_path_cb_t path) {
  for (int i = 0; i < e.fade_n; ++i) {
    lv_obj_t* w = e.fade[i];
    if (w == nullptr || lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_obj_set_style_opa(w, static_cast<lv_opa_t>(from), LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_duration(&a, kFadeMs);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_var(&a, w);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_start(&a);
  }
}

void driver_exec(void* var, int32_t v) {
  *static_cast<int32_t*>(var) = v;  // no visible effect; just advances the clock
}

void start_driver(lv_anim_ready_cb_t ready) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_duration(&a, kFadeMs);
  lv_anim_set_var(&a, &s_driver);
  lv_anim_set_exec_cb(&a, driver_exec);
  lv_anim_set_values(&a, 0, 1);
  lv_anim_set_ready_cb(&a, ready);
  lv_anim_start(&a);
}

void phase_b_done(lv_anim_t*) {
  lv_obj_add_flag(s_block, LV_OBJ_FLAG_HIDDEN);
  s_swapping = false;
}

void phase_a_done(lv_anim_t*) {
  Entry& out = entry(s_swap.from);
  Entry& in  = entry(s_swap.to);

  // The outgoing set has faded away. Restore its widgets to full opacity while it
  // is hidden, so it is ready for its own next entrance rather than arriving
  // already transparent.
  set_opa(out, LV_OPA_COVER);
  lv_obj_add_flag(out.group, LV_OBJ_FLAG_HIDDEN);

  // Reseed the incoming screen before anyone can see it.
  if (in.on_show != nullptr) in.on_show();
  lv_obj_remove_flag(in.group, LV_OBJ_FLAG_HIDDEN);

  fade_set(in, LV_OPA_TRANSP, LV_OPA_COVER, lv_anim_path_ease_out);
  start_driver(phase_b_done);
}

}  // namespace

void init(lv_obj_t* scr) {
  s_block = lv_obj_create(scr);
  lv_obj_set_size(s_block, kScreenW, kScreenH);
  lv_obj_set_pos(s_block, 0, 0);
  lv_obj_set_style_bg_opa(s_block, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_block, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_block, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_block, LV_OBJ_FLAG_CLICKABLE);  // the whole point of it
  lv_obj_add_flag(s_block, LV_OBJ_FLAG_HIDDEN);
}

void register_screen(Screen s, lv_obj_t* group, lv_obj_t* const* fade, int n,
                     void (*on_show)()) {
  if (!valid(s)) return;
  Entry& e = entry(s);
  e.group = group;
  e.on_show = on_show;
  e.fade_n = 0;
  if (n > kMaxFadeWidgets) {
    ESP_LOGW(kTag, "screen %d has %d fade widgets, room for %d — the rest will pop",
             static_cast<int>(s), n, kMaxFadeWidgets);
    n = kMaxFadeWidgets;
  }
  for (int i = 0; i < n; ++i) e.fade[e.fade_n++] = fade[i];
}

void start() {
  for (int i = 0; i < kScreenCount; ++i) {
    lv_obj_t* g = s_screens[i].group;
    if (g == nullptr) continue;
    if (i == static_cast<int>(s_current)) lv_obj_remove_flag(g, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
  }
  // The blocker is created before the screens, so it would otherwise sit under
  // them and never intercept anything.
  lv_obj_move_foreground(s_block);
  lv_obj_add_flag(s_block, LV_OBJ_FLAG_HIDDEN);
}

void go(Screen s) {
  if (!valid(s) || s == s_current) return;
  if (s_swapping) {
    ESP_LOGD(kTag, "swap already in flight; ignoring go(%d)", static_cast<int>(s));
    return;
  }
  Entry& out = entry(s_current);
  if (out.group == nullptr || entry(s).group == nullptr) {
    ESP_LOGW(kTag, "screen %d is not registered", static_cast<int>(s));
    return;
  }

  s_swap = {s_current, s};
  s_current = s;
  s_swapping = true;

  lv_obj_move_foreground(s_block);
  lv_obj_remove_flag(s_block, LV_OBJ_FLAG_HIDDEN);

  fade_set(out, LV_OPA_COVER, LV_OPA_TRANSP, lv_anim_path_ease_in);
  start_driver(phase_a_done);
}

Screen current() { return s_current; }

// True from the moment go() is called, not from the midpoint — a screen fading in
// is on display, and so is one fading out. Both want their refresh running: the
// alternative is a page that arrives holding whatever it last painted.
bool visible(Screen s) {
  return s == s_current || (s_swapping && s == s_swap.from);
}

}  // namespace thermopost::ui::nav
