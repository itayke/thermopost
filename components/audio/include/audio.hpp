#pragma once

#include <cstdint>

#include "esp_err.h"

// The alarm tone: ES8311 codec → FM8002E amplifier → onboard speaker.
//
// Deliberately dumber than "audio". It plays one thing — a beeping alarm tone —
// for a caller-supplied duration, and that is the whole surface. The alert state
// machine (alert_logic.hpp) decides *when* to make noise and the Sound settings
// page decides whether the user wants any; neither belongs here, and keeping the
// policy out is what lets the settings page's Test button and M7's alarm share one
// code path instead of two that drift.
//
// Nothing here blocks: play() hands the request to a task and returns, because the
// only two callers are the LVGL task and a 1 Hz alarm tick, and a two-second
// blocking write from either would freeze the screen.
namespace thermopost::audio {

// Bring up I²S, find the codec on the shared I²C bus and identify it. Requires
// board_i2c::init() to have run.
//
// Returns ESP_ERR_NOT_FOUND when nothing answers at either ES8311 address —
// non-fatal, and by design: an alarm that can only be seen is still an alarm, and
// the plan puts the codec behind this API precisely so a silent board does not
// block anything else. ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// False when init() did not find a codec. Callers should use it to explain
// themselves rather than to fail quietly — a Test button that does nothing is
// worse than one that says why.
bool available();

// Beep for `ms`, at `volume_pct` (0..100), then release the amplifier. Returns
// immediately; ESP_ERR_INVALID_STATE if there is no codec or a tone is already
// running, so a caller can treat "already playing" as a toggle.
esp_err_t play(uint32_t ms, uint8_t volume_pct);

// Cut the current tone short. Safe to call when nothing is playing. Returns once
// the request is posted, not once the speaker is quiet — silence follows within
// one buffer.
void stop();

// Whether a tone is running right now. Cheap enough to poll from a UI timer.
bool playing();

}  // namespace thermopost::audio
