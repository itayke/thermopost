#pragma once

#include <cstdint>
#include <string>

// Pure, IDF-free serialization of temperature records into the JSON body the
// device POSTs to the Google Apps Script Web App.
//
// Hand-rolled rather than cJSON, and with zero ESP-IDF includes, so it builds
// and unit-tests on the host under tests/host/ — the same split alert_logic
// uses. If a header from IDF ever becomes necessary here, the code belongs in
// cloud.cpp instead.
namespace thermopost::cloud::json {

// Mirror of storage::kFlag* / storage::kInvalidTempC100. Duplicated rather than
// including storage.hpp, which pulls in esp_err.h and would end the host build.
// cloud.cpp static_asserts each against the original, so the two copies cannot
// silently drift.
namespace bits {
constexpr uint8_t kFlagAlarmHigh   = 1u << 0;
constexpr uint8_t kFlagAlarmLow    = 1u << 1;
constexpr uint8_t kFlagSensorFault = 1u << 2;
}  // namespace bits

constexpr int16_t kInvalidTempC100 = INT16_MIN;

// The fields of a storage::TempRecord that go on the wire, plus the record's
// absolute index in the log. A separate POD keeps this layer independent of the
// on-disk struct — the two are allowed to diverge, and the copy in cloud.cpp is
// the one place that has to know both.
struct RecordJson {
  uint32_t index;      // absolute position in the log; half of the dedupe key
  uint8_t  version;    // TempRecord::version, so the Sheet can tell v1 from v2
  uint8_t  flags;      // bits::kFlag*
  int16_t  temp_c100;  // interval mean; kInvalidTempC100 for none
  int16_t  min_c100;
  int16_t  max_c100;
  uint32_t epoch_s;    // 0 until SNTP has landed
  uint32_t uptime_s;   // ordering fallback when epoch_s is 0
};

// What every record in a batch shares.
//
// **`device` is the dedupe key, not `name`, and not `index` alone.** A fleet of
// posts each number their records from zero, so an index by itself collides
// across the group on the very first row. The name cannot stand in for it
// either: it is renameable from the config form by design, and a key that
// changes when someone fixes a typo is not a key. So `device` is derived from
// the MAC — stable for the life of the board, never edited — and `name` rides
// along as a display column the Sheet can be filtered and sorted by.
//
// The battery fields are here, in the envelope, and NOT in each record — that is
// the whole point of them. A record is a fact about one interval; the pack is a
// fact about the post right now. Putting it in TempRecord would also have been
// impossible without breaking the log: the record is nailed to 16 bytes because
// record N lives at offset N x 16, so growing it invalidates every absolute index
// and every cloud high-water mark in the fleet. Here it costs nothing on flash,
// travels once per POST instead of sixty near-identical times, and is naturally
// "as of now", which is what an alert wants. The Sheet still gets a usable curve:
// one point per upload is ample for a pack that discharges over hours.
struct Envelope {
  std::string token;   // shared secret; a soft gate, see cloud.cpp
  std::string device;  // stable per-device id (MAC hex)
  std::string name;    // human label, mutable, display only
  // Millivolts at the pack, or negative when there is no usable reading — no
  // divider, an ADC that did not answer, or a batch built before power::init().
  // Serializes as JSON null in that case, never as a number a reader could act on.
  int32_t     batt_mv  = -1;
  // 0..100, and an estimate rather than a measurement — lithium is nearly flat
  // through the middle of its range, so this comes off a published curve. Sent
  // alongside the millivolts rather than instead of them: a fleet dashboard wants
  // to threshold on a percentage, and anyone who does not trust it has the volts.
  int32_t     batt_pct = -1;
  // On external power. The field that stops a fleet alert being noise: a post at
  // 3.6 V and charging is recovering, a post at 3.6 V and discharging needs a
  // visit, and the voltage alone cannot tell them apart.
  bool        external = true;
  // The longest this post means to go between uploads, in seconds; 0 when unknown,
  // sent as null. What lets the Sheet call a post silent without knowing its
  // cadence settings: a fixed threshold would fire all day on a post set to an hour.
  uint32_t    gap_s = 0;
};

// Escape a string's contents for embedding between JSON double quotes (does NOT
// add the quotes). Handles " and \ and the C0 control set: \b \f \n \r \t as
// their short escapes, anything else below 0x20 as \u00XX.
std::string escape(const std::string& s);

// Format hundredths of a degree as a JSON number: 2135 -> "21.35", -5 -> "-0.05".
//
// Integer arithmetic, never a float: the stored value IS exact hundredths, and
// routing it through a double to print it can only introduce error. It also
// makes the output byte-for-byte predictable, which is what lets the tests
// compare whole strings instead of parsing them back.
std::string fmt_c100(int16_t c100);

// Append one record as a JSON object to `out`. Field order is fixed so the
// output is deterministic. An invalid temperature serializes as `null`, never
// as a sentinel a reader might mistake for -327.68 °C; `epoch` is omitted
// entirely when the clock had not been set.
void append_record(std::string& out, const RecordJson& r);

// Build the full request body:
//   {"token":"…","device":"…","name":"…","batt_mv":…,"batt_pct":…,"ext":…,
//    "gap_s":…,"records":[ {…}, … ]}
std::string build_payload(const Envelope& env, const RecordJson* recs, size_t n);

}  // namespace thermopost::cloud::json
