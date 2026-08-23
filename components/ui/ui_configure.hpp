#pragma once

#include "lvgl.h"

// The provisioning page: a QR code that joins the post's own setup network.
//
// Scanning a `WIFI:` code is how a phone joins a network without anyone reading a
// name off a screen and typing it — and the post's AP is open, so there is no
// password to convey either. Once joined, the OS captive check hits the config
// form and it opens by itself. The whole flow is one scan.
//
// The page owns the setup network's lifetime from the user's side: it is entered
// by opening the AP, Back closes it again rather than leaving an open network up,
// and it leaves on its own the moment the post has a link — because the AP going
// down *is* the signal that provisioning worked.
namespace thermopost::ui::configure_screen {

// Builds the page into `scr` and registers it with nav. Call after nav::init().
void build(lv_obj_t* scr);

}  // namespace thermopost::ui::configure_screen
