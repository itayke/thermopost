#include "sleepstate.hpp"

#include <sys/time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace thermopost::sleepstate {
namespace {

constexpr const char* kTag = "sleepstate";

// RTC memory is undefined after power-on, so a seal is a magic, not a flag.
constexpr uint32_t kSealMagic = 0x51EE9A7Eu;
RTC_DATA_ATTR uint32_t s_seal;

// alert, cloud, recorder and power today; room for one more before it is a decision.
constexpr int kMaxHooks = 5;
void (*s_hooks[kMaxHooks])() = {};
int s_hook_count = 0;

bool s_resumed = false;

constexpr int64_t kUsPerMs = 1000;
constexpr int64_t kMsPerSec = 1000;

int64_t mono_now_ms() { return esp_timer_get_time() / kUsPerMs; }

int64_t wall_now_ms() {
  timeval tv = {};
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * kMsPerSec + tv.tv_usec / kUsPerMs;
}

}  // namespace

void begin_boot(bool kept_wake) {
  s_resumed = kept_wake && s_seal == kSealMagic;
  s_seal = 0;
  if (s_resumed) ESP_LOGI(kTag, "resuming the state it slept with");
}

bool resumed() { return s_resumed; }

void on_sleep(void (*hook)()) {
  if (s_hook_count >= kMaxHooks) {
    // Loud rather than silent: a mirror that is never filled is a device that
    // wakes up forgetting something, and nothing else would say so.
    ESP_LOGE(kTag, "no room for another sleep hook; raise kMaxHooks");
    return;
  }
  s_hooks[s_hook_count++] = hook;
}

void save_all() {
  for (int i = 0; i < s_hook_count; ++i) s_hooks[i]();
  s_seal = kSealMagic;
}

int64_t to_wall_ms(int64_t mono_ms) {
  if (mono_ms == 0) return 0;
  return wall_now_ms() + (mono_ms - mono_now_ms());
}

int64_t to_mono_ms(int64_t wall_ms) {
  if (wall_ms == 0) return 0;
  const int64_t now = mono_now_ms();
  const int64_t mono = now + (wall_ms - wall_now_ms());
  // Already passed: due now. Never zero, which would mean "none" to the caller.
  return mono > now ? mono : (now > 0 ? now : 1);
}

}  // namespace thermopost::sleepstate
