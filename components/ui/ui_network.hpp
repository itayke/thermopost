#pragma once

#include "lvgl.h"

// The network page: what the post is connected to, where to reach it, how far the
// Sheet has caught up — and the two actions that need no keyboard.
//
// It is deliberately not an editor. Every field this page reports on is free text
// (a network name, a password, an /exec URL), which is exactly what the config
// form owns and a 2.8" panel handles worst. What is left is a status read and two
// buttons: reopen the setup AP, and forget the stored network.
//
// It is also the fallback when mDNS does not work — some networks drop multicast,
// and a post that could only offer a .local name would then have no address to
// give at all. So the IP is shown beside the name, always.
namespace thermopost::ui::network_screen {

// Builds the page into `scr` and registers it with nav. Call after nav::init().
void build(lv_obj_t* scr);

}  // namespace thermopost::ui::network_screen
