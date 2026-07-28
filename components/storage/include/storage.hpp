#pragma once

#include <cstdint>

#include "esp_err.h"

// Append-only temperature log on LittleFS, plus the rotation policy that keeps
// it bounded.
//
// Cloud sync tracks what it has uploaded with a high-water mark — an absolute
// record number — so a record's index has to mean the same thing forever. That
// is free for a log that only ever grows. This one doesn't: at a record a minute
// (~1440/day, ~23 KB) it MUST be rotated, and rotation moves every record's
// file position.
//
// The fix is a persistent **base index**: the absolute record number of whatever
// currently sits at file offset 0, kept in NVS and advanced by exactly as many
// records as each rotation drops. Absolute indices are therefore still assigned
// once and never reused — they just stop starting at zero. Everything public
// below speaks absolute indices; file offsets never leave this component.
namespace thermopost::storage {

// Bit positions for TempRecord::flags. Records the alarm state *at sample time*
// so the Sheet shows when the band was breached without the reader having to
// re-derive it against thresholds that may have been edited since.
//   kFlagAlarmHigh   — reading was above the configured maximum
//   kFlagAlarmLow    — reading was below the configured minimum
//   kFlagSensorFault — the sample failed; temp_c100 is kInvalidTempC100
// Bits 3-7 are reserved.
constexpr uint8_t kFlagAlarmHigh   = 1u << 0;
constexpr uint8_t kFlagAlarmLow    = 1u << 1;
constexpr uint8_t kFlagSensorFault = 1u << 2;

// Sentinel for `temp_c100` when no valid reading was obtained. Chosen as the
// bottom of the int16 range so it can never collide with a real measurement —
// the MCP9808 only spans -40..+125 °C.
constexpr int16_t kInvalidTempC100 = INT16_MIN;

// Fixed-width on-disk record for one temperature sample. Packed and explicit so
// the file is parseable off-device; little-endian (ESP32 native).
//
// `version` lets a future schema change be detected without a separate file
// header, so a later layout change can be migrated in place. v1 is the first
// schema, so there is no migration path to write yet; the byte exists so there
// can be one later without a flag day.
//
// `epoch_s` is 0 until SNTP lands (this board has no battery-backed RTC — see
// the cloud component). Fall back to `uptime_us` for ordering when it is 0.
struct __attribute__((packed)) TempRecord {
  uint8_t  version;    // 1 (current)
  uint8_t  flags;      // see kFlag* above
  int16_t  temp_c100;  // °C × 100 (e.g. 2237 = 22.37 °C); kInvalidTempC100 if none
  uint32_t epoch_s;    // wall-clock seconds since 1970; 0 until SNTP has synced
  int64_t  uptime_us;  // esp_timer microseconds since boot — fallback ordering key
};
static_assert(sizeof(TempRecord) == 16, "TempRecord wire size must be stable");

// Convenience for the °C ↔ stored-int conversion, so call sites don't sprinkle
// magic 100s. Kept inline and IDF-free.
constexpr int16_t temp_to_c100(float c) { return static_cast<int16_t>(c * 100.0f + (c < 0 ? -0.5f : 0.5f)); }
constexpr float   c100_to_temp(int16_t v) { return static_cast<float>(v) / 100.0f; }

// ---------------------------------------------------------------------------
// Rotation policy
// ---------------------------------------------------------------------------
// At one record a minute, kMaxRecords is ~70 days of history in 1.6 MB. When the
// log reaches it, the oldest kRotateBlock records are dropped in one pass.
// Rotating a tenth of the log at a time (rather than one record per append)
// keeps the expensive full-file rewrite to roughly once a week instead of once
// a minute; the Sheet is the real archive, so the on-device tail is only ever
// needed for the graph window and any not-yet-uploaded backlog.
constexpr uint32_t kMaxRecords  = 100'000;
constexpr uint32_t kRotateBlock =  10'000;

// Mount LittleFS on the `storage` partition (label matches partitions.csv) and
// load the base index from NVS. Formats the partition on first boot or after
// corruption. Requires nvs_flash_init() to have run. Safe to call once; returns
// ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// Append one record. `version` is stamped internally — callers leave it zeroed;
// every other field passes through verbatim. Flushes and fsyncs before
// returning, so a power loss immediately afterward still retains the sample.
//
// Triggers a rotation when the log has grown past kMaxRecords. A rotation
// failure is logged but does NOT fail the append — losing the newest reading to
// protect a housekeeping invariant is the wrong trade.
esp_err_t append(const TempRecord& record);

// Absolute index that will be assigned to the next appended record. Equivalently
// the total number of records ever written, across all rotations.
uint32_t next_index();

// Absolute index of the oldest record still on disk. 0 until the first rotation;
// after that it is the count of records rotated away. Cloud sync compares its
// high-water mark against this to notice it has fallen behind rotation.
uint32_t first_index();

// Number of records currently in the file (== next_index() - first_index()).
uint32_t count();

// Read up to `max` records starting at absolute index `from_index`.
//
// If `from_index` points at records that rotation has already dropped, the read
// silently begins at first_index() instead — callers learn where it actually
// started through `out_first_index`, which receives the absolute index of
// out[0]. That is the signal that history was missed; it is never inferable
// from the return value alone. Pass nullptr only if you already know
// `from_index >= first_index()`.
//
// Returns the number of records read; never exceeds `max`. Records arrive in
// append order (chronological).
size_t read_from(uint32_t from_index, TempRecord* out, size_t max,
                 uint32_t* out_first_index);

// Read the most recent `max` records, in chronological order (oldest first).
// Returns the number read — fewer than `max` when the log is shorter. This is
// what the graph consumes on boot and after a deep-sleep wake.
size_t read_recent(TempRecord* out, size_t max);

// Drop the oldest `drop_count` records immediately, advancing the base index by
// the same amount. This is the exact path append() takes when the log passes
// kMaxRecords — exposed because the alternative way to exercise the rotation
// transaction is to write 100,000 records and wait, which means in practice it
// would never be tested until it failed in the field. Also backs a `log rotate`
// serial command. A no-op success when the log holds no more than `drop_count`
// records (rotating everything away is never what the caller meant).
esp_err_t force_rotate(uint32_t drop_count);

// Delete every record. Resets the base index to 0, so absolute numbering
// restarts — cloud sync must have its high-water mark reset alongside this or
// it will consider the fresh records already uploaded.
esp_err_t erase_all();

// Print the whole log to the console as parseable `dump[i]: ...` lines.
// Diagnostic only, triggered from the serial console — streams in small batches
// so a 100k-record log costs no extra RAM.
void dump();

}  // namespace thermopost::storage
