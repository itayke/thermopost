#pragma once

namespace thermopost::bringup {

// Runs the hardware bring-up checks and logs a GATE PASS / GATE FAIL summary.
// Parks forever on failure so the finding isn't buried under the driver errors
// a wrong pin map would then cause.
//
// DESTRUCTIVE: erases the temperature log and resets settings to defaults so
// the storage and settings checks start from a known state. Compiled in and
// called only under CONFIG_THERMOPOST_RUN_BRINGUP_PROBE.
//
// Leaves NVS, storage and settings initialized — the boot sequence that follows
// will see ESP_ERR_INVALID_STATE from their init() calls and treat it as "up".
void run();

}  // namespace thermopost::bringup
