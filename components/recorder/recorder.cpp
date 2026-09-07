#include "recorder.hpp"

#include <algorithm>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "alert_logic.hpp"
#include "cloud.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "sensor.hpp"
#include "settings.hpp"
#include "storage.hpp"

namespace thermopost::recorder {
namespace {

constexpr const char* kTag = "recorder";

// The ring is sized for the widest graph the settings allow, so changing
// graph_points never needs a reallocation — 480 samples is under a kilobyte.
constexpr size_t kRingCap = settings::kGraphPointsMax;

// The task wakes on this tick to fold the newest reading into the interval's
// running aggregate, and accumulates towards interval_s rather than sleeping for
// the whole period. Sleeping the full period would mean an interval changed from
// an hour to ten seconds takes up to an hour to apply, which reads as the setting
// being broken.
//
// Faster than the sensor on purpose, and *derived* from its period rather than
// stated independently. The two are separate timers and will drift against each
// other, so a tick equal to the sample period would now and then step over a
// reading entirely — and if that reading is the spike, min/max miss the one thing
// they exist to catch. Oversampling makes that impossible; the duplicates it
// produces are filtered by sequence number in accumulate().
//
// Written as a ratio because the failure it prevents is silent: two hard-coded
// periods in two components would still compile, still run, and only show up as an
// envelope that occasionally flattens a spike it should have caught.
constexpr uint32_t kTicksPerSample = 4;
constexpr uint32_t kTickMs = sensor::kSamplePeriodMs / kTicksPerSample;
static_assert(kTicksPerSample >= 2,
              "the recorder must tick at least twice per sample, or drift between the "
              "two timers lets a reading slip past the min/max envelope");
static_assert(pdMS_TO_TICKS(kTickMs) >= 1,
              "the tick rounds to zero FreeRTOS ticks, which turns the recorder task "
              "into a busy loop — raise the sample period or CONFIG_FREERTOS_HZ");

constexpr uint32_t kMsPerSec = 1000;
constexpr int64_t  kUsPerSec = 1'000'000;

// Below this, the system clock has not been set by SNTP and epoch_s must be
// stored as 0 (see storage.hpp). Roughly Nov 2023 — comfortably after any
// plausible build date and far above the 1970 an unsynced ESP32 reports.
constexpr uint32_t kEpochValidFrom = 1'700'000'000;

// Records per read during the boot backfill. Small enough to sit on the stack.
constexpr size_t kBackfillChunk = 32;

constexpr uint32_t kTaskStack = 4096;  // LittleFS write + fsync need the room
constexpr UBaseType_t kTaskPrio = 2;   // below sensor's 3: freshness beats logging
constexpr BaseType_t kTaskCore = 0;

Sample   s_ring[kRingCap] = {};
size_t   s_count = 0;  // valid entries, <= kRingCap
size_t   s_head  = 0;  // next write position
uint32_t s_generation = 0;
uint32_t s_written = 0;

// The interval's running aggregate, reset after every record.
int32_t  s_sum_c100 = 0;
uint32_t s_sum_n = 0;
int16_t  s_min_c100 = 0;
int16_t  s_max_c100 = 0;
// The sensor's own successful-sample counter, last time one was folded in. It is
// how a genuinely new reading is told from the same one seen again on the next
// tick — polling latest() cannot distinguish them, and counting a stalled value
// sixty times would drag the mean towards it.
uint32_t s_seen_samples = 0;

SemaphoreHandle_t s_lock = nullptr;
bool s_inited = false;

// A mutex, not a portMUX spinlock: history() copies nearly 3 KB and holding
// interrupts off for that long to serve a redraw would be the wrong trade.
struct Guard {
  Guard()  { if (s_lock != nullptr) xSemaphoreTake(s_lock, portMAX_DELAY); }
  ~Guard() { if (s_lock != nullptr) xSemaphoreGive(s_lock); }
};

void ring_push(const Sample& s) {
  s_ring[s_head] = s;
  s_head = (s_head + 1) % kRingCap;
  if (s_count < kRingCap) ++s_count;
}

void reset_aggregate() {
  s_sum_c100 = 0;
  s_sum_n = 0;
  s_min_c100 = 0;
  s_max_c100 = 0;
}

// Fold the newest reading in, if it is one we have not already counted.
void accumulate() {
  const sensor::Reading r = sensor::latest();
  if (!r.valid || r.samples == s_seen_samples) return;
  s_seen_samples = r.samples;

  if (s_sum_n == 0) {
    s_min_c100 = r.temp_c100;
    s_max_c100 = r.temp_c100;
  } else {
    if (r.temp_c100 < s_min_c100) s_min_c100 = r.temp_c100;
    if (r.temp_c100 > s_max_c100) s_max_c100 = r.temp_c100;
  }
  s_sum_c100 += r.temp_c100;
  ++s_sum_n;
}

// Close the interval. An interval that produced nothing valid is a fault record
// rather than a gap, so the log says the device was awake and the sensor was not.
Sample close_aggregate() {
  if (s_sum_n == 0) {
    return {kInvalidTempC100, kInvalidTempC100, kInvalidTempC100};
  }
  // Rounds to nearest rather than truncating, so a steady 22.375 °C does not
  // report as 22.37 for the life of the device.
  const int32_t n = static_cast<int32_t>(s_sum_n);
  const int32_t half = (s_sum_c100 < 0 ? -n : n) / 2;
  return {static_cast<int16_t>((s_sum_c100 + half) / n), s_min_c100, s_max_c100};
}

// Did this interval breach the band? A bare compare, deliberately — not
// alert::step().
//
// The two answer different questions. The state machine's hysteresis governs when
// the *device* stops alarming, which is about not chattering the speaker. These
// flags are a property of the record: they have to stay re-derivable from the
// values stored beside them and the band, or a Sheet column disagrees with its own
// numbers. So the alarm's hysteresis, its snooze and its acknowledgement change
// how the device behaves without changing a byte of what is written here.
//
// Tested against the interval's extremes rather than its mean, and both bits can
// be set: a minute that swung through the band from below the minimum to above the
// maximum genuinely breached both, and reporting only one would be a choice about
// which half of the truth to keep. Testing the mean instead would let a five-second
// excursion average its way back inside the band, which is the whole failure the
// aggregate record exists to prevent.
uint8_t interval_flags(const Sample& s) {
  if (s.mean_c100 == kInvalidTempC100) return storage::kFlagSensorFault;
  const alert::Thresholds t = settings::thresholds();
  uint8_t f = 0;
  if (s.max_c100 > t.max_c100) f |= storage::kFlagAlarmHigh;
  if (s.min_c100 < t.min_c100) f |= storage::kFlagAlarmLow;
  return f;
}

void write_one() {
  const Sample s = close_aggregate();
  const bool valid = s.mean_c100 != kInvalidTempC100;

  const std::time_t now = std::time(nullptr);

  storage::TempRecord rec = {};
  // Flagged on the extremes, not the mean: an excursion that lasted five seconds
  // out of sixty barely moves the average, and a record that averaged its way back
  // inside the band while its own max says otherwise is the failure this whole
  // change exists to prevent.
  rec.flags     = interval_flags(s);
  rec.temp_c100 = s.mean_c100;
  rec.min_c100  = s.min_c100;
  rec.max_c100  = s.max_c100;
  rec.epoch_s   = now > static_cast<std::time_t>(kEpochValidFrom)
                      ? static_cast<uint32_t>(now)
                      : 0;
  rec.uptime_s  = static_cast<uint32_t>(esp_timer_get_time() / kUsPerSec);

  // The ring updates whether or not the append succeeds: a full or broken
  // filesystem should cost the log, not the graph the user is looking at. The
  // written counter is deliberately NOT bumped here alongside it — see below.
  {
    Guard g;
    ring_push(s);
    ++s_generation;
  }

  const esp_err_t err = storage::append(rec);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "append failed: %s", esp_err_to_name(err));
    return;
  }

  // Only after the append succeeded, for the same reason the counter below is:
  // there is now a record on disk for the uploader to find. A failed append has
  // nothing to offer it, and waking it to discover that would spend a TLS
  // handshake on a no-op.
  cloud::notify_new_record();

  // Only now, because this counter is shown on screen as the answer to "is it
  // still recording?". Incrementing it before the append would make it climb
  // serenely through exactly the failure it exists to reveal.
  {
    Guard g;
    ++s_written;
  }

  if (valid) {
    ESP_LOGI(kTag, "#%lu mean %.2f  min %.2f  max %.2f  (%lu samples) flags=0x%02X%s",
             static_cast<unsigned long>(storage::next_index() - 1),
             static_cast<double>(s.mean_c100) / 100.0,
             static_cast<double>(s.min_c100) / 100.0,
             static_cast<double>(s.max_c100) / 100.0,
             static_cast<unsigned long>(s_sum_n), rec.flags,
             rec.epoch_s == 0 ? " (clock not set)" : "");
  } else {
    ESP_LOGW(kTag, "#%lu no valid reading in the whole interval",
             static_cast<unsigned long>(storage::next_index() - 1));
  }
}

void task(void*) {
  TickType_t last = xTaskGetTickCount();
  uint32_t elapsed_ms = 0;

  for (;;) {
    xTaskDelayUntil(&last, pdMS_TO_TICKS(kTickMs));
    elapsed_ms += kTickMs;

    // Every tick, not just at the boundary. This is what makes a record a summary
    // of its interval instead of a snapshot of the instant it happened to end.
    accumulate();

    // Re-read the interval every tick so an edit from the menu or the web form
    // applies promptly. A shortened interval that is already overdue fires
    // immediately rather than waiting out the old period.
    const uint32_t period_ms =
        static_cast<uint32_t>(settings::get().interval_s) * kMsPerSec;
    if (elapsed_ms < period_ms) continue;

    elapsed_ms = 0;
    write_one();
    reset_aggregate();
  }
}

// Seed the ring from the log so the graph is populated the moment the screen
// draws. Reads at most kRingCap records; a longer log yields its tail.
//
// Chunked rather than one read_recent(kRingCap) call: that would want a 7.5 KB
// buffer, too much for this task's stack and not worth holding in BSS for the
// rest of the device's uptime to serve something that runs once.
void backfill() {
  const uint32_t total = storage::count();
  const uint32_t take = std::min<uint32_t>(total, kRingCap);
  if (take == 0) {
    ESP_LOGI(kTag, "log is empty; the graph starts blank and fills from here");
    return;
  }

  // out_first_index is safely nullptr here: take <= count, so this start index
  // cannot land below first_index() and rotation can't have dropped it.
  uint32_t idx = storage::next_index() - take;
  storage::TempRecord chunk[kBackfillChunk];
  uint32_t done = 0;

  while (done < take) {
    const size_t want = std::min<uint32_t>(kBackfillChunk, take - done);
    const size_t got = storage::read_from(idx, chunk, want, nullptr);
    if (got == 0) break;  // short read shouldn't happen; don't spin on it
    for (size_t i = 0; i < got; ++i) {
      // storage upgrades v1 records on the way out, so min/max are always present
      // here — for a v1 record they equal the single sample it stored, which is
      // genuinely all the history that interval ever had.
      if (chunk[i].flags & storage::kFlagSensorFault) {
        ring_push({kInvalidTempC100, kInvalidTempC100, kInvalidTempC100});
      } else {
        ring_push({chunk[i].temp_c100, chunk[i].min_c100, chunk[i].max_c100});
      }
    }
    idx  += got;
    done += got;
  }

  ESP_LOGI(kTag, "backfilled %lu of %lu logged records",
           static_cast<unsigned long>(done), static_cast<unsigned long>(total));
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;

  s_lock = xSemaphoreCreateMutex();
  if (s_lock == nullptr) return ESP_ERR_NO_MEM;

  reset_aggregate();
  backfill();

  if (xTaskCreatePinnedToCore(task, "recorder", kTaskStack, nullptr, kTaskPrio,
                              nullptr, kTaskCore) != pdPASS) {
    vSemaphoreDelete(s_lock);
    s_lock = nullptr;
    return ESP_ERR_NO_MEM;
  }

  s_inited = true;
  ESP_LOGI(kTag, "logging every %us", static_cast<unsigned>(settings::get().interval_s));
  return ESP_OK;
}

size_t history(Sample* out, size_t n) {
  if (out == nullptr || n == 0) return 0;
  Guard g;
  const size_t take = std::min(n, s_count);
  size_t idx = (s_head + kRingCap - take) % kRingCap;
  for (size_t i = 0; i < take; ++i) {
    out[i] = s_ring[idx];
    idx = (idx + 1) % kRingCap;
  }
  return take;
}

uint32_t generation() { Guard g; return s_generation; }
uint32_t written()    { Guard g; return s_written; }

// Forwarded rather than letting the UI reach into storage: this component is the
// only thing that writes records, so "how many are there" is its question to
// answer, and the screens stay free of any filesystem dependency.
uint32_t total() { return storage::next_index(); }

}  // namespace thermopost::recorder
