#pragma once

#include <cstdint>

#include "alert_logic.hpp"
#include "esp_err.h"

// The alarm, running. alert_logic.hpp decides what the state should be; this
// drives that decision with real readings, holds the answer where the UI can see
// it, and makes the speaker do something about it.
//
// Everything IDF-facing lives here so alert_logic.cpp can keep compiling under
// tests/host/ with the system compiler. The one thing this does NOT own is the
// alarm's appearance: the panel on the main screen is ui_main.cpp's, built out
// of the same widgets as everything else on that screen. Splitting it that way
// keeps every LVGL object in the `ui` component, which is the rule that stops
// two files from disagreeing about what a button looks like.
namespace thermopost::alert {

// Start the tick that evaluates every sample. Requires settings::init() and
// sensor::init() to have run; audio::init() is optional, and a board with no
// codec simply alarms on screen only.
//
// Safe to call once; ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// A quiet wake's alarm check: one reading stepped against the episode the post went
// to sleep with, saved for the next wake. True when the alarm needs raising, which
// means the post has to come up properly. Requires settings::init(); no task, no tone.
bool judge_wake(int16_t temp_c100, bool valid);

// The current alarm state. Safe before init() — it reads as Normal until the
// first tick, which is what the UI should draw anyway. Cheap enough for the
// LVGL task's refresh.
Status status();

// The main screen's two buttons.
//
// Both are the user saying "I have seen this", and differ only in how long they
// last: silence() is until the temperature changes what it is doing, snooze() is
// until snooze_minutes() have passed. Both stop a tone that is already sounding
// rather than letting the current burst finish — a button that goes quiet a
// second later reads as a button that was missed.
void silence();
void snooze();

// How long snooze() lasts, so the button can label itself from the same number
// it acts on rather than restating it.
uint32_t snooze_minutes();

}  // namespace thermopost::alert
