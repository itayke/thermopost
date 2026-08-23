#include "ui_network.hpp"

#include <cstdint>
#include <cstdio>

#include "cloud.hpp"
#include "settings.hpp"
#include "ui_chrome.hpp"
#include "ui_nav.hpp"
#include "ui_theme.hpp"

namespace thermopost::ui::network_screen {
namespace {

constexpr size_t kTextMax = 64;

// --- Layout -----------------------------------------------------------------
// A block of status lines under the heading, two actions centred below it, one
// footer line. Full width and centred rather than a two-column grid: these are
// sentences, not values, and the longest of them (an SSID plus its signal) needs
// the whole line.
constexpr int32_t kLineGap = 4;
constexpr int32_t kLineH   = kFont14H + kLineGap;
constexpr int     kLines   = 5;
constexpr int32_t kInfoY   = kHeadingY + kFont24H + 8;
constexpr int32_t kInfoW   = kScreenW - 2 * kEdgePad;
constexpr int32_t kInfoBottom = kInfoY + kLines * kLineH;

constexpr int32_t kBtnW = 132;
constexpr int32_t kBtnH = kPillH;
// The two buttons' hit areas must not meet between them, or the gap belongs to
// whichever was built last — the same rule ui_menu's gutters and ui_settings'
// columns are derived from. kBtnSeam is the dead strip deliberately left over.
constexpr int32_t kBtnExtClick = ext_click_for(kBtnW, kBtnH);
constexpr int32_t kBtnSeam = 2;
constexpr int32_t kBtnGap  = 2 * kBtnExtClick + kBtnSeam;
constexpr int32_t kBtnRowW = 2 * kBtnW + kBtnGap;
static_assert(kBtnRowW <= kScreenW - 2 * kEdgePad, "the action row overhangs the screen");

// Centred in the space left between the status block and the footer, so adding a
// status line rebalances the page instead of pushing the buttons into the footer.
constexpr int32_t kBtnY = kInfoBottom + (kFooterY - kInfoBottom - kBtnH) / 2;
static_assert(kBtnY >= kInfoBottom, "the action row does not fit under the status lines");

// Refreshed once a second while the page is up: an RSSI and a pending count both
// move on their own, and a page that only updated on entry would be read as stale.
constexpr uint32_t kRefreshMs = 1000;

// --- Wording ----------------------------------------------------------------
constexpr const char* kWifiNone       = "no network set";
constexpr const char* kWifiConnecting = "connecting...";
constexpr const char* kWifiOffline    = "offline - retrying";
constexpr const char* kNoAddress      = "no address";
constexpr const char* kNoEndpoint     = "no Sheet endpoint set";
constexpr const char* kUploading      = "uploading...";
constexpr const char* kRetryPending   = "upload retry pending";

// "Configure", not "Setup Wi-Fi": the form behind it carries the thresholds, the
// interval and the post's name as well as the credentials, and the AP is only the
// way to reach it on a post that has no network yet.
constexpr const char* kConfigureText = "Configure";
constexpr const char* kForgetText  = "Forget";
constexpr const char* kConfirmText = "Sure?";

// The footer answers "and where do I do the rest of this", which is the question
// a page with no text entry on it invites.
constexpr const char* kHintProvisioning = "join the post's Wi-Fi and open any page";
constexpr const char* kHintForm         = "thresholds and the name are editable there too";
constexpr const char* kSetupFailed      = "could not open the setup network";

// Forget drops a credential that has to be retyped from a phone, and the button
// sits a finger's width from Setup. So it asks once, and un-asks itself rather
// than staying armed for a stray tap minutes later.
constexpr uint32_t kConfirmMs = 5000;

// --- State ------------------------------------------------------------------
PageChrome s_page = {};
lv_obj_t*  s_line[kLines] = {};
lv_obj_t*  s_setup  = nullptr;
lv_obj_t*  s_forget = nullptr;
lv_timer_t* s_confirm_timer = nullptr;
bool       s_confirming = false;

void set_line(int i, lv_color_t color, const char* text) {
  lv_obj_set_style_text_color(s_line[i], color, LV_PART_MAIN);
  lv_label_set_text(s_line[i], text);
}

void unarm() {
  s_confirming = false;
  lv_label_set_text(pill_label(s_forget), kForgetText);
  pill_set_color(s_forget, kColorText);
}

void disarm_confirm() {
  if (s_confirm_timer != nullptr) {
    lv_timer_delete(s_confirm_timer);
    s_confirm_timer = nullptr;
  }
  unarm();
}

// The timer is one-shot, so LVGL deletes it after this returns — dropping the
// handle here is what keeps disarm_confirm from freeing it twice.
void on_confirm_timeout(lv_timer_t*) {
  s_confirm_timer = nullptr;
  unarm();
}

void refresh() {
  const cloud::Status net = cloud::status();
  char text[kTextMax];

  // --- link
  switch (net.wifi) {
    case cloud::WifiState::Connected:
      std::snprintf(text, sizeof(text), "%s   %d dBm", net.ssid, net.rssi_dbm);
      set_line(0, kColorOk, text);
      break;
    case cloud::WifiState::Connecting:
      set_line(0, kColorSubText, kWifiConnecting);
      break;
    case cloud::WifiState::Offline:
      set_line(0, kColorFault, kWifiOffline);
      break;
    case cloud::WifiState::Disabled:
      set_line(0, kColorMuted, kWifiNone);
      break;
  }

  // --- address, then the name that resolves to it. Both, always: mDNS is the
  // nicer half and the one a network is allowed to drop.
  set_line(1, kColorText, net.ip[0] != '\0' ? net.ip : kNoAddress);
  std::snprintf(text, sizeof(text), "%s.local", net.host);
  set_line(2, kColorSubText, text);

  // --- the Sheet
  if (!net.endpoint_set) {
    set_line(3, kColorMuted, kNoEndpoint);
  } else if (net.sync == cloud::SyncState::Syncing) {
    set_line(3, kColorSubText, kUploading);
  } else if (net.sync == cloud::SyncState::Backoff) {
    set_line(3, kColorFault, kRetryPending);
  } else {
    std::snprintf(text, sizeof(text), "%lu uploaded   %lu pending",
                  static_cast<unsigned long>(net.uploaded),
                  static_cast<unsigned long>(net.pending));
    set_line(3, kColorText, text);
  }

  // --- the last upload failure, kept on its own line rather than replacing the
  // count: "how far behind am I" and "why" are different questions.
  if (net.last_error != ESP_OK) {
    std::snprintf(text, sizeof(text), "last error: %s", esp_err_to_name(net.last_error));
    set_line(4, kColorFault, text);
  } else {
    set_line(4, kColorMuted, "");
  }

  lv_label_set_text(s_page.footer, net.provisioning ? kHintProvisioning : kHintForm);
}

void tick(lv_timer_t*) {
  if (!nav::visible(nav::Screen::Network)) return;
  refresh();
}

void on_configure(lv_event_t*) {
  disarm_confirm();
  // Already up on an unprovisioned post, where the AP came on at boot — then this
  // button is only the way to the code that joins it.
  if (!cloud::status().provisioning) {
    const esp_err_t e = cloud::start_provisioning();
    if (e != ESP_OK) {
      lv_label_set_text(s_page.footer, kSetupFailed);
      return;
    }
  }
  nav::go(nav::Screen::Configure);
}

void on_forget(lv_event_t*) {
  if (!s_confirming) {
    s_confirming = true;
    lv_label_set_text(pill_label(s_forget), kConfirmText);
    pill_set_color(s_forget, kColorHigh);
    s_confirm_timer = lv_timer_create(on_confirm_timeout, kConfirmMs, nullptr);
    lv_timer_set_repeat_count(s_confirm_timer, 1);
    return;
  }
  disarm_confirm();
  cloud::forget();
  refresh();
}

// Called at the swap midpoint. Anything armed belongs to the visit that armed it.
void load() {
  disarm_confirm();
  refresh();
}

}  // namespace

void build(lv_obj_t* scr) {
  build_page_chrome(scr, "Network", &s_page);

  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_page.back.btn;
  fade[n++] = s_page.heading;
  fade[n++] = s_page.footer;

  for (int i = 0; i < kLines; ++i) {
    s_line[i] = make_boxed_label(s_page.group, &lv_font_montserrat_14, kColorText,
                                 kInfoW, LV_TEXT_ALIGN_CENTER);
    // An SSID may be 32 characters, which is wider than the panel at this font.
    lv_label_set_long_mode(s_line[i], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_line[i], kEdgePad, kInfoY + i * kLineH);
    fade[n++] = s_line[i];
  }

  const int32_t row_x = (kScreenW - kBtnRowW) / 2;
  s_setup = build_pill(s_page.group, kConfigureText, kBtnW, kBtnH, kColorText,
                       &lv_font_montserrat_14, on_configure, nullptr);
  lv_obj_set_pos(s_setup, row_x, kBtnY);
  fade[n++] = s_setup;

  s_forget = build_pill(s_page.group, kForgetText, kBtnW, kBtnH, kColorText,
                        &lv_font_montserrat_14, on_forget, nullptr);
  lv_obj_set_pos(s_forget, row_x + kBtnW + kBtnGap, kBtnY);
  fade[n++] = s_forget;

  refresh();
  lv_timer_create(tick, kRefreshMs, nullptr);

  nav::register_screen(nav::Screen::Network, s_page.group, fade, n, load);
}

}  // namespace thermopost::ui::network_screen
