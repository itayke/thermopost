#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// The app's spine: on the configured interval, take whatever `sensor` last read,
// write it to the log, and keep the same values in a RAM ring for the graph.
//
// This is the component that turns two independent cadences into one policy.
// `sensor` samples at a fixed 1 Hz so the readout is always fresh; the *log*
// advances at settings::interval_s (a minute by default) so flash lasts and the
// Sheet stays readable. Nothing else in the system is allowed to write records —
// one writer means the absolute indices storage hands out stay meaningful.
//
// The ring exists so the UI never touches the filesystem to draw. It is
// backfilled from storage on init(), which is what makes the graph come up
// populated after a reboot rather than starting empty and taking two hours to
// mean anything.
namespace thermopost::recorder {

// Sentinel for a slot with no valid reading — matches storage::kInvalidTempC100
// so the ring and the log agree. Callers plotting history must skip these rather
// than draw them as a temperature.
constexpr int16_t kInvalidTempC100 = INT16_MIN;

// One interval's worth of history: its mean and its extremes. The extremes are the
// point — a record is written once a minute from sixty readings, so a graph drawn
// from the mean alone would smooth away exactly the excursions this device exists
// to catch. All three are kInvalidTempC100 together when the interval produced no
// valid reading at all.
struct Sample {
  int16_t mean_c100;
  int16_t min_c100;
  int16_t max_c100;
};

// Backfill the ring from the log and start the recording task. Requires
// storage::init(), settings::init() and sensor::init() to have run — sensor may
// have failed, which is fine: fault records are worth logging.
//
// Safe to call once; ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// Copy up to `n` of the most recent samples into `out`, oldest first. Returns how
// many were written, which is fewer than `n` until the ring fills. Cheap enough
// to call from the LVGL task, but see generation() before doing it every frame.
size_t history(Sample* out, size_t n);

// Bumped every time a record is written. The UI polls this and rebuilds the chart
// only when it changes — at a one-minute interval and a 4 Hz refresh that skips
// 239 pointless redraws out of every 240.
uint32_t generation();

// Records SUCCESSFULLY appended since boot. Counts completed writes, not
// attempts: a counter that keeps climbing while the filesystem is full or broken
// defeats the only job it has on screen, which is to distinguish "recording
// normally" from "stopped recording" when the temperature itself is not moving.
// So a frozen number here means exactly that, and generation() — which does
// advance on a failed append, because the graph still got its point — is the one
// to watch for liveness of sampling rather than of logging.
uint32_t written();

// Total records ever written, across every boot and every rotation. This is the
// absolute index storage assigns, so it is directly comparable to cloud sync's
// high-water mark; it is NOT the number currently on disk, which is smaller once
// rotation has dropped a block.
uint32_t total();

}  // namespace thermopost::recorder
