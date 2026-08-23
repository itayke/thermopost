#include "ui_configure.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "cloud.hpp"
#include "settings.hpp"
#include "ui_chrome.hpp"
#include "ui_nav.hpp"
#include "ui_theme.hpp"

namespace thermopost::ui::configure_screen {
namespace {

// --- The WIFI: URI ----------------------------------------------------------
// The format every phone camera understands: WIFI:T:<auth>;S:<ssid>;P:<pass>;;
// The setup AP is open, so the type is `nopass` and there is no P field — which
// is also why nothing secret is being encoded into something anyone can photograph.
constexpr const char* kWifiUriHead = "WIFI:T:nopass;S:";
constexpr const char* kWifiUriTail = ";;";

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
constexpr int32_t kQrSize = 140;
constexpr int32_t kQrY    = kHeadingY + kFont24H + 5;
constexpr int32_t kNameGap = 6;
constexpr int32_t kNameY  = kQrY + kQrSize + kNameGap;
static_assert(kNameY + kFont14H <= kFooterY, "the network name runs into the footer");
static_assert(kQrY >= kNavPillY + kNavPillH,
              "a centred code this high would sit beside the nav pill, not under it");

// A QR is only scannable if its light modules are actually light. The panel's own
// background is near-black, so the code brings its own plate and quiet zone rather
// than inheriting the page's palette like everything else here does.
constexpr lv_color_t kQrDark  = color(0x000000);
constexpr lv_color_t kQrLight = color(0xFFFFFF);

// Polled rather than pushed: the page has one thing to watch and it is already
// visible in cloud::status(). Twice a second, so the automatic exit lands close
// enough to the connection to read as a consequence of it.
constexpr uint32_t kPollMs = 500;

constexpr const char* kFooterText = "scan to join, then open any page";

PageChrome s_page = {};
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
  lv_label_set_text(s_name, s.name);
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

  s_qr = lv_qrcode_create(s_page.group);
  lv_qrcode_set_size(s_qr, kQrSize);
  lv_qrcode_set_dark_color(s_qr, kQrDark);
  lv_qrcode_set_light_color(s_qr, kQrLight);
  lv_qrcode_set_quiet_zone(s_qr, true);
  lv_obj_set_pos(s_qr, (kScreenW - kQrSize) / 2, kQrY);

  s_name = make_boxed_label(s_page.group, &lv_font_montserrat_14, kColorText,
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
  fade[n++] = s_qr;
  fade[n++] = s_name;
  nav::register_screen(nav::Screen::Configure, s_page.group, fade, n, refresh);
}

}  // namespace thermopost::ui::configure_screen
