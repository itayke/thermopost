#include "alert.hpp"

#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio.hpp"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "sensor.hpp"
#include "settings.hpp"
#include "sleepstate.hpp"

namespace thermopost::alert {
namespace {

constexpr const char* kTag = "alert";

constexpr int64_t kUsPerMs = 1000;
constexpr uint32_t kMsPerMin = 60 * 1000;

// One evaluation per sample. Unlike the recorder, this does not oversample: the
// alarm is a level test, not an envelope, so a tick that now and then lines up
// with the sampler and re-reads the same value costs nothing and misses nothing.
// The worst case is that a threshold crossing is noticed one period late, which
// is a second on a device whose readings move over minutes.
constexpr uint32_t kTickMs = sensor::kSamplePeriodMs;

// The tone is a burst rather than a continuous noise: kToneMs of the beeping
// pattern, then quiet until kToneRepeatMs is up. Continuous is louder but not
// more informative, and a room with an unattended post in it has to stay a room
// someone can work in — the burst is what makes ignoring the alarm a decision
// rather than an impossibility.
//
// Rounded to the tick, because the tick is the only clock this gets checked on;
// a repeat that is not a multiple of it would drift by up to a period each time.
constexpr uint32_t kToneMs = 1500;
constexpr uint32_t kToneRepeatMs = 4 * kTickMs;
static_assert(kToneMs < kToneRepeatMs, "the alarm tone would never stop for breath");

// How long the Snooze button silences the alarm for. A Kconfig entry rather than
// a constant because the right answer depends on the tank: long enough to walk
// over and do something about it, short enough that walking away instead is not
// the same as switching the alarm off.
constexpr uint32_t kSnoozeMinutes = CONFIG_THERMOPOST_ALERT_SNOOZE_MINUTES;
constexpr int64_t  kSnoozeMs = static_cast<int64_t>(kSnoozeMinutes) * kMsPerMin;

constexpr int kTaskStack = 3072;
// Above the UI, below the sensor that feeds it: a busy screen must not be able to
// delay the alarm, and the alarm must not be able to delay the sample it reads.
constexpr int kTaskPrio = 5;
constexpr int kTaskCore = 0;

SemaphoreHandle_t s_mutex = nullptr;
bool s_inited = false;

// Guarded by s_mutex.
Status  s_status = kInitialStatus;
bool    s_tone_ours = false;    // the codec is playing OUR tone, not the test one
int64_t s_next_tone_ms = 0;

struct Lock {
  bool ok;
  explicit Lock(TickType_t t = portMAX_DELAY) : ok(xSemaphoreTake(s_mutex, t) == pdTRUE) {}
  ~Lock() { if (ok) xSemaphoreGive(s_mutex); }
};

int64_t now_ms() { return esp_timer_get_time() / kUsPerMs; }

// The alarm as it went to sleep. Without it an acknowledged excursion re-alerts on
// every wake, which turns Off into a button that does nothing. The snooze travels as
// wall-clock time because esp_timer restarts at zero on a deep-sleep wake.
struct SleepMirror {
  State   state;
  bool    acked;
  int64_t snooze_until_wall_ms;  // 0 when not snoozed
};
RTC_DATA_ATTR SleepMirror s_mirror;

void save_for_sleep() {
  Lock l;
  s_mirror = {s_status.state, s_status.acked,
              sleepstate::to_wall_ms(s_status.snooze_until_ms)};
}

const char* state_name(State s) {
  switch (s) {
    case State::High:  return "high";
    case State::Low:   return "low";
    case State::Fault: return "fault";
    default:           return "normal";
  }
}

// Cut a tone we started. Never touches one we did not: the Sound page's test
// button uses the same codec, and an alarm clearing while someone is judging the
// speaker should not silence what they are listening to.
void hush_locked() {
  if (s_tone_ours && audio::playing()) audio::stop();
  s_tone_ours = false;
  s_next_tone_ms = 0;
}

// Decide whether the speaker should be making noise, and act on it. The master
// mute and the volume are read here rather than inside the state machine — see
// should_alert()'s comment in alert_logic.hpp for why that split exists.
void drive_speaker(bool alerting, int64_t now) {
  const settings::Settings cfg = settings::get();
  Lock l;
  if (!alerting || !cfg.sound_enabled || !audio::available()) {
    hush_locked();
    return;
  }
  if (now < s_next_tone_ms || audio::playing()) return;
  if (audio::play(kToneMs, cfg.volume_pct) == ESP_OK) {
    s_tone_ours = true;
    s_next_tone_ms = now + kToneRepeatMs;
  }
}

void task(void*) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    xTaskDelayUntil(&last, pdMS_TO_TICKS(kTickMs));

    const sensor::Reading r = sensor::latest();
    const Thresholds t = settings::thresholds();
    const int64_t now = now_ms();

    // Keep the part's own comparator on the same band this task is judging against.
    // Pushed from here rather than from the settings setter because this is where the
    // sanitized band already is, and because re-pushing it every tick is what makes an
    // edit from the menu apply promptly without any change-notification machinery —
    // sensor::set_alert_window() is a no-op when nothing moved.
    //
    // The window is what lets the post sleep between records without missing an
    // excursion between them; it does nothing for a post that is awake, which is why
    // it can be wired up and proved long before anything sleeps.
    sensor::set_alert_window(t.min_c100, t.max_c100);

    State was;
    Status is;
    {
      Lock l;
      was = s_status.state;
      s_status = step(s_status, r.temp_c100, r.valid, t, now);
      is = s_status;
    }

    // Logged on the transition only. An alarm that reprints itself every second
    // buries the moment it started, which is the one line worth having when the
    // question afterwards is when the tank went wrong.
    if (is.state != was) {
      ESP_LOGW(kTag, "%s -> %s (%.2f C, band %.2f..%.2f)", state_name(was),
               state_name(is.state), r.valid ? r.temp_c100 / 100.0 : 0.0,
               t.min_c100 / 100.0, t.max_c100 / 100.0);
    }

    drive_speaker(should_alert(is), now);
  }
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;

  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) return ESP_ERR_NO_MEM;

  // Before the task, so its first tick judges the restored episode, not a fresh one.
  if (sleepstate::resumed()) {
    s_status = {s_mirror.state, s_mirror.acked,
                sleepstate::to_mono_ms(s_mirror.snooze_until_wall_ms)};
  }
  sleepstate::on_sleep(&save_for_sleep);

  if (xTaskCreatePinnedToCore(task, "alert", kTaskStack, nullptr, kTaskPrio, nullptr,
                              kTaskCore) != pdPASS) {
    vSemaphoreDelete(s_mutex);
    s_mutex = nullptr;
    return ESP_ERR_NO_MEM;
  }

  s_inited = true;
  ESP_LOGI(kTag, "watching every %" PRIu32 " ms; snooze %" PRIu32 " min%s",
           kTickMs, kSnoozeMinutes,
           audio::available() ? "" : " (no codec — the alarm is visual only)");
  return ESP_OK;
}

Status status() {
  if (s_mutex == nullptr) return kInitialStatus;
  Lock l;
  return s_status;
}

void silence() {
  if (s_mutex == nullptr) return;
  Lock l;
  s_status = acknowledge(s_status);
  hush_locked();
}

// The three-argument snooze() this calls is the pure one in alert_logic.hpp;
// this no-argument one is the button. Same verb, one tier apart.
void snooze() {
  if (s_mutex == nullptr) return;
  const int64_t now = now_ms();
  Lock l;
  s_status = snooze(s_status, now, kSnoozeMs);
  hush_locked();
}

uint32_t snooze_minutes() { return kSnoozeMinutes; }

}  // namespace thermopost::alert
