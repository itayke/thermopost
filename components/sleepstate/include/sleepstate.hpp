#pragma once

#include <cstdint>

// What has to survive a deep sleep, and the one clock it can be kept on.
//
// Deep sleep loses RAM. Each component that must come back as the same device
// keeps its own RTC_DATA_ATTR mirror and registers a hook that fills it just before
// sleeping; this component decides whether those mirrors mean anything on the next
// boot. They do only after a sleep that sealed them — a power-on leaves RTC memory
// undefined, and Turn Off deliberately does not seal, because Off is meant to forget.
//
// Deadlines are the trap. Everything runs on esp_timer, which restarts at zero on
// every deep-sleep wake, so a deadline mirrored as-is would be meaningless. They
// travel as absolute times on the wall clock instead, which IDF keeps running
// through deep sleep; absolute rather than remaining so a deadline survives any
// number of intermediate wakes that never touched it.
namespace thermopost::sleepstate {

// First thing at boot, from power::resume_from_off(). `kept_wake` is a deep-sleep
// wake that is not Turn Off. Consumes the seal, so it cannot be honoured twice.
void begin_boot(bool kept_wake);

// This boot continues the device that went to sleep: its mirrors are valid.
bool resumed();

// Called by a component's init. A hook already registered is ignored. The hook fills
// its mirror; it runs from the task that puts the post to sleep, so it must take
// whatever lock its data needs.
void on_sleep(void (*hook)());

// Just before a state-keeping deep sleep: run every hook, then seal the mirrors.
void save_all();

// An esp_timer deadline (ms) as an absolute wall-clock time (ms), and back. Zero
// means "no deadline" in both directions. A deadline already behind the clock
// converts to the current instant, never to zero, so it reads as due rather than
// as absent.
int64_t to_wall_ms(int64_t mono_ms);
int64_t to_mono_ms(int64_t wall_ms);

}  // namespace thermopost::sleepstate
