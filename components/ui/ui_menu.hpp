#pragma once

#include "lvgl.h"

// The menu hub — what the main screen's Menu pill opens. Its heading is the post's
// name rather than the word "MENU": in a rack of five identical boxes the useful
// thing to say at the top of the settings screen is which box this is, and the
// chevron pill in the corner already says what the screen is for.
namespace thermopost::ui::menu_screen {

// Builds the screen into `scr` and registers it with nav. Call after nav::init().
void build(lv_obj_t* scr);

}  // namespace thermopost::ui::menu_screen
