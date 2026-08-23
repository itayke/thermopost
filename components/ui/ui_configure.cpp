#include "ui_configure.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "cloud.hpp"
#include "settings.hpp"
#include "ui_chrome.hpp"
#include "ui_nav.hpp"
#include "ui_theme.hpp"

namespace thermopost::ui::configure_screen {
namespace {

constexpr const char* kTag = "ui.configure";

// --- The WIFI: URI ----------------------------------------------------------
// The format phone cameras understand: WIFI:S:<ssid>;T:<auth>;P:<pass>;;
//
// **Field order matches espressopost's byte for byte, deliberately.** The de-facto
// spec is order-independent and ZXing-derived parsers read it that way, but not
// every scanner is ZXing-derived, and espressopost's is the payload known to work
// on the phones these get set up from. There is nothing to gain by differing.
//
// The setup AP is open, so the type is `nopass` and there is no P field — which is
// also why nothing secret is encoded into something anyone can photograph.
constexpr const char* kWifiUriHead = "WIFI:S:";
constexpr const char* kWifiUriTail = ";T:nopass;;";

// The five characters the URI reserves. A post name may legally contain any of
// them (set_name only strips non-ASCII), and an unescaped `;` would truncate the
// SSID at the scanner — silently, into a network that does not exist.
constexpr const char* kUriSpecials = "\\;,\":";

// Derived rather than guessed, so editing either literal above resizes the buffer:
// head + tail + every character of a maximum-length name escaped + NUL.
constexpr size_t cstr_len(const char* s) {
  size_t n = 0;
  while (s[n] != '\0') ++n;
  return n;
}
constexpr size_t kUriMax =
    cstr_len(kWifiUriHead) + 2 * settings::kNameMaxLen + cstr_len(kWifiUriTail) + 1;

// --- Layout -----------------------------------------------------------------
// The code centred with the network's name under it. The name is not decoration:
// it is what the phone's Wi-Fi list will show, so a camera that will not focus
// still leaves someone able to join by hand.
constexpr int32_t kQrTopGap = 5;
constexpr int32_t kNameGap  = 4;
constexpr int32_t kQrY      = kBelowHeadingY + kQrTopGap;

// The white plate fills the band left between the heading and the name line, so the
// modules come out as large as the page allows rather than as large as some number
// that got typed here.
constexpr int32_t kQrCard = kFooterY - kNameGap - kFontBodyH - kQrY;

// The quiet zone is this padding, NOT lv_qrcode_set_quiet_zone(). The widget's own
// version fits the code *and* its border inside the size it is given, so the border
// comes out of the module budget: at 29 modules plus four each side, a 160 px widget
// draws 4.3 px modules where the same footprint with the border outside draws 5.0.
// A payload up to "WIFI:T:nopass;S:" plus a 24-character name lands on version 3, so
// that difference is the difference between scanning at arm's length and not.
constexpr int32_t kQrPad  = 8;
constexpr int32_t kQrSize = kQrCard - 2 * kQrPad;
constexpr int32_t kQrRadius = 8;

constexpr int32_t kNameY = kQrY + kQrCard + kNameGap;
static_assert(kNameY + kFontBodyH <= kFooterY, "the network name runs into the footer");

// A QR is only scannable if its light modules are actually light, and the panel's
// background is near-black — so the code brings its own plate rather than inheriting
// the page's palette like everything else here does.
constexpr lv_color_t kQrDark  = color(0x000000);
constexpr lv_color_t kQrLight = color(0xFFFFFF);

// Polled rather than pushed: the page has one thing to watch and it is already
// visible in cloud::status(). Twice a second, so the automatic exit lands close
// enough to the connection to read as a consequence of it.
constexpr uint32_t kPollMs = 500;

constexpr const char* kFooterText = "scan to join, then open any page";

// Labelled, not bare: under a QR that joins a Wi-Fi network, an unlabelled string
// reads as easily as a password or a page title as it does as a network name — and
// it is there precisely for the case where the scan fails and someone has to find
// it in a Wi-Fi list by hand.
constexpr const char* kNameFmt = "Network: %s";
constexpr size_t kNameTextMax = cstr_len(kNameFmt) + settings::kNameMaxLen + 1;

PageChrome s_page = {};
lv_obj_t*  s_card = nullptr;
lv_obj_t*  s_qr   = nullptr;
lv_obj_t*  s_name = nullptr;

bool is_special(char c) {
  for (const char* p = kUriSpecials; *p != '\0'; ++p) {
    if (c == *p) return true;
  }
  return false;
}

void build_uri(const char* ssid, char* out, size_t cap) {
  size_t o = 0;
  for (const char* p = kWifiUriHead; *p != '\0' && o + 1 < cap; ++p) out[o++] = *p;
  for (const char* p = ssid; *p != '\0' && o + 2 < cap; ++p) {
    if (is_special(*p)) out[o++] = '\\';
    out[o++] = *p;
  }
  for (const char* p = kWifiUriTail; *p != '\0' && o + 1 < cap; ++p) out[o++] = *p;
  out[o] = '\0';
}

// Re-read on every show: the name can have changed from the config form since the
// last visit, and the code and the caption both encode it.
void refresh() {
  const settings::Settings s = settings::get();
  char uri[kUriMax];
  build_uri(s.name, uri, sizeof(uri));
  lv_qrcode_update(s_qr, uri, static_cast<uint32_t>(std::strlen(uri)));
  // Logged because the code itself cannot be proofread: a scanner that declines to
  // join gives no clue whether the payload is wrong or the app simply cannot join
  // networks, and this is the half that can be checked from a serial terminal.
  ESP_LOGI(kTag, "QR payload: %s", uri);
  char label[kNameTextMax];
  std::snprintf(label, sizeof(label), kNameFmt, s.name);
  lv_label_set_text(s_name, label);
}

void tick(lv_timer_t*) {
  if (!nav::visible(nav::Screen::Configure)) return;
  // The GOT_IP handler tears the AP down, so the AP being gone is exactly "the
  // post is on a network now". Nothing else needs watching, and leaving on that
  // signal means the page does not linger over a code that no longer joins
  // anything.
  if (!cloud::status().provisioning) nav::go(nav::Screen::Network);
}

// Back closes the setup network rather than leaving it open behind the page that
// opened it — an open AP nobody can see is exactly the kind of thing that stays up
// for a week.
void on_back(lv_event_t*) {
  cloud::stop_provisioning();
  nav::go(nav::Screen::Network);
}

}  // namespace

void build(lv_obj_t* scr) {
  build_page_chrome(scr, "Configure", &s_page, on_back);
  lv_label_set_text(s_page.footer, kFooterText);

  s_card = lv_obj_create(s_page.group);
  lv_obj_set_size(s_card, kQrCard, kQrCard);
  lv_obj_set_pos(s_card, (kScreenW - kQrCard) / 2, kQrY);
  lv_obj_set_style_bg_color(s_card, kQrLight, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_card, kQrRadius, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_card, kQrPad, LV_PART_MAIN);
  lv_obj_remove_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_card, LV_OBJ_FLAG_CLICKABLE);

  s_qr = lv_qrcode_create(s_card);
  lv_qrcode_set_size(s_qr, kQrSize);
  lv_qrcode_set_dark_color(s_qr, kQrDark);
  lv_qrcode_set_light_color(s_qr, kQrLight);
  lv_obj_center(s_qr);

  s_name = make_boxed_label(s_page.group, kFontBody, kColorText,
                            kScreenW - 2 * kEdgePad, LV_TEXT_ALIGN_CENTER);
  lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_pos(s_name, kEdgePad, kNameY);

  refresh();
  lv_timer_create(tick, kPollMs, nullptr);

  lv_obj_t* fade[nav::kMaxFadeWidgets];
  int n = 0;
  fade[n++] = s_page.back.btn;
  fade[n++] = s_page.heading;
  fade[n++] = s_page.footer;
  // The card, not the code: LVGL applies an object's opacity to its children, so
  // fading the plate takes the QR with it and the two cannot come apart mid-fade.
  fade[n++] = s_card;
  fade[n++] = s_name;
  nav::register_screen(nav::Screen::Configure, s_page.group, fade, n, refresh);
}

}  // namespace thermopost::ui::configure_screen
