#pragma once

#include "lvgl.h"

// The editor screens the menu hub opens. Four pages, each a heading, a chevron pill
// in the shared top-left corner, up to three controls on a two-column grid, and a
// footer line stating something derived — the exact stored value behind a
// whole-degree stepper, the time span a graph window works out to, the consequence
// of a toggle being off.
//
// That footer is the answer to the one honest cost of integer stepping: a threshold
// edited in °F does not land on a round number of °C, and a page that hid that
// would look like it was drifting.
//
// Every write goes through the same settings::set_* setters the web form will use in
// M8, so there is no second source of truth and no merge to get wrong. Each page
// re-reads on show, which is what keeps a page opened after a form write from
// showing — and then clobbering — a stale value.
namespace thermopost::ui::settings_screen {

// Builds all four pages into `scr` and registers them with nav. Call after
// nav::init().
void build_all(lv_obj_t* scr);

}  // namespace thermopost::ui::settings_screen
