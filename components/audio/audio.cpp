#include "audio.hpp"

#include "board_i2c.hpp"
#include "board_pins.hpp"
#include "sdkconfig.h"

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstddef>

namespace thermopost::audio {
namespace {

constexpr const char* kTag = "audio";

// --- Tone shape -------------------------------------------------------------
// 16 kHz is eight times the tone and a quarter of the buffer memory a CD rate
// would want; there is no content here above 2 kHz to lose.
constexpr uint32_t kSampleRateHz = 16'000;
// Roughly where a small sealed speaker is loudest and where hearing is most
// sensitive, which is the same reason smoke alarms live up here.
constexpr uint32_t kToneHz = 2'000;

// Beeping, not a continuous tone. A steady note reads as a fault in the device;
// an interrupted one reads as the device trying to tell you something, and it
// stays audible against room noise that a constant pitch disappears into.
constexpr uint32_t kBeepOnMs  = 150;
constexpr uint32_t kBeepOffMs = 100;

// ~73% of full scale. Loudness is the volume setting's job, but the codec's own
// curve already puts the default 70% volume at -15 dB, and leaving another 8 dB
// unused on top of that made a working speaker sound like a broken one. This keeps
// enough headroom that the peaks stay a tone rather than a rasp and no more.
constexpr int16_t kAmplitude = 24'000;

constexpr float kTwoPi = 6.283185307179586f;

// --- Buffering --------------------------------------------------------------
// One buffer is written over and over, so it has to hold a whole number of
// periods: a fractional one puts a phase jump at every buffer boundary, which is
// an audible click at 31 Hz — a defect that sounds like a driver bug and isn't.
constexpr uint32_t kSamplesPerPeriod = kSampleRateHz / kToneHz;
static_assert(kSampleRateHz % kToneHz == 0,
              "the tone must divide the sample rate, or the repeated buffer clicks");

constexpr uint32_t kBufSamples = 512;
static_assert(kBufSamples % kSamplesPerPeriod == 0,
              "the buffer must be a whole number of tone periods; see above");

constexpr uint32_t ms_to_samples(uint32_t ms) { return ms * kSampleRateHz / 1000; }

// Same argument one level up: a beep that ends mid-period clicks into the gap.
static_assert(ms_to_samples(kBeepOnMs) % kSamplesPerPeriod == 0,
              "the beep length must be a whole number of tone periods");

// --- Codec ------------------------------------------------------------------
// The driver takes the 8-bit (shifted) address and halves it internally, so this
// is board_pins' 0x18/0x19 shifted left, not the address the bus scan prints.
constexpr uint8_t kAddrShift = 1;
constexpr uint32_t kProbeTimeoutMs = 20;
constexpr int kCodecI2cFreqHz = 100'000;
constexpr int kMclkMultiple = 256;
constexpr uint8_t kBitsPerSample = 16;
constexpr uint8_t kChannels = 1;  // mono: one speaker, and the tone is a sine
constexpr int kVolumeMaxPct = 100;

// --- Task -------------------------------------------------------------------
constexpr int kTaskStack = 3072;
// Above the sampler (3) and LVGL (2). This is the only task on the board with a
// hard deadline measured in DMA buffers: miss one and the speaker stutters, where
// a late sample or a late repaint is invisible.
constexpr int kTaskPrio = 4;
// Core 0 with the sampler, leaving core 1 to the display flush.
constexpr int kTaskCore = 0;

bool s_started = false;
i2c_master_bus_handle_t s_bus = nullptr;
uint8_t s_addr = 0;
i2s_chan_handle_t s_tx = nullptr;
const audio_codec_ctrl_if_t* s_ctrl_if = nullptr;
const audio_codec_if_t*      s_codec_if = nullptr;
const audio_codec_data_if_t* s_data_if = nullptr;
esp_codec_dev_handle_t s_dev = nullptr;
TaskHandle_t s_task = nullptr;

// One period-aligned cycle of the tone, and the same length of silence for the
// gaps. Writing real zeros rather than pausing keeps the gap exactly as long as it
// says it is — a pause would leave the DMA to drain at its own pace.
int16_t s_tone[kBufSamples];
int16_t s_silence[kBufSamples];

// Written by the caller's task, read by the tone task. Both are single-word and
// the handshake is the notification, so there is nothing here a lock would add.
volatile bool     s_playing = false;
volatile bool     s_stop = false;
volatile uint32_t s_req_ms = 0;
volatile uint8_t  s_req_vol = 0;

void build_tone() {
  for (uint32_t i = 0; i < kBufSamples; ++i) {
    const float phase = kTwoPi * static_cast<float>(i % kSamplesPerPeriod) /
                        static_cast<float>(kSamplesPerPeriod);
    s_tone[i] = static_cast<int16_t>(static_cast<float>(kAmplitude) * std::sin(phase));
    s_silence[i] = 0;
  }
}

// Which of the two strap-selectable ES8311 addresses actually answers, or 0.
// Asked rather than assumed for the same reason the sensor asks: a board revision
// that strapped CE the other way should log "found at 0x19", not "silent".
uint8_t probe_codec(i2c_master_bus_handle_t bus) {
  for (const uint8_t a : {board::kEs8311I2cAddrLow, board::kEs8311I2cAddrHigh}) {
    if (i2c_master_probe(bus, a, kProbeTimeoutMs) == ESP_OK) return a;
  }
  return 0;
}

esp_err_t start_i2s() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // Zero the DMA buffer once it drains, so an underrun — or the tail left behind
  // by stop() — comes out as silence instead of the last buffer looping.
  chan_cfg.auto_clear = true;
  const esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
  if (err != ESP_OK) return err;

  // esp_codec_dev rewrites the clock and slot config from the sample info at
  // open(), so what matters here is the pin map; the rest is a valid starting
  // point rather than the format we end up running.
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRateHz),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          // Not driven at all when the trace isn't there: the codec derives its
          // master clock from BCLK instead, which leaves IO4 spare.
          .mclk = board::kI2sMclkRouted ? static_cast<gpio_num_t>(board::kI2sMclk)
                                        : I2S_GPIO_UNUSED,
          .bclk = static_cast<gpio_num_t>(board::kI2sBclk),
          .ws   = static_cast<gpio_num_t>(board::kI2sLrck),
          .dout = static_cast<gpio_num_t>(board::kI2sDout),
          // The microphone is unused in v1; claiming its pin would only stop
          // something else from using it.
          .din  = I2S_GPIO_UNUSED,
          .invert_flags = {false, false, false},
      },
  };
  std_cfg.clk_cfg.mclk_multiple = static_cast<i2s_mclk_multiple_t>(kMclkMultiple);
  return i2s_channel_init_std_mode(s_tx, &std_cfg);
}

void teardown() {
  if (s_dev != nullptr) {
    esp_codec_dev_delete(s_dev);
    s_dev = nullptr;
  }
  if (s_data_if != nullptr) {
    audio_codec_delete_data_if(s_data_if);
    s_data_if = nullptr;
  }
  if (s_codec_if != nullptr) {
    audio_codec_delete_codec_if(s_codec_if);
    s_codec_if = nullptr;
  }
  if (s_ctrl_if != nullptr) {
    audio_codec_delete_ctrl_if(s_ctrl_if);
    s_ctrl_if = nullptr;
  }
  if (s_tx != nullptr) {
    i2s_del_channel(s_tx);
    s_tx = nullptr;
  }
}

// Push `count` samples from a buffer that is only kBufSamples long, repeating it.
// esp_codec_dev_write blocks until the DMA has taken the data, so this loop runs
// at the sample clock and needs no timing of its own.
//
// Returns false the moment a write fails. That matters more than it looks: a write
// that fails does not block, so an unchecked loop would spin at priority 4 for the
// whole length of the tone and starve core 0 — a silent speaker turning into a
// watchdog reset.
bool write_samples(const int16_t* buf, uint32_t count) {
  while (count > 0 && !s_stop) {
    const uint32_t n = std::min(count, kBufSamples);
    const int err = esp_codec_dev_write(s_dev, const_cast<int16_t*>(buf),
                                        static_cast<int>(n * sizeof(int16_t)));
    if (err != ESP_CODEC_DEV_OK) {
      ESP_LOGE(kTag, "I2S write failed (%d) — the codec is configured but the data "
                     "path is not carrying samples", err);
      return false;
    }
    count -= n;
  }
  return true;
}

#if CONFIG_THERMOPOST_AUDIO_DEBUG
// The seven registers that answer "is this codec actually driving its output".
// esp_codec_dev_dump_reg() offers all 255, but the ES8311 only implements about
// seventy of them and reading the rest back as unlabelled hex is not debugging.
// Expected readings are taken from what the driver writes in es8311_start(), so
// they are what a working part looks like rather than what the datasheet allows.
struct RegProbe {
  uint8_t     reg;
  const char* meaning;
};

constexpr RegProbe kProbes[] = {
    {0x01, "clock manager: bit7 0 = MCLK pin, 1 = derived from BCLK; 0x00 = nothing clocked"},
    // Not a health check: 0x00 is correct at a 256x MCLK ratio, since it holds
    // (pre_div - 1) << 5 | pre_multi << 3 and both are 1. A rate the coefficient
    // table cannot match fails open() outright, so this register can never be the
    // thing that tells you about it.
    {0x02, "MCLK pre-divider/multiplier: 0x00 is correct for a 256x ratio"},
    {0x09, "DAC serial port: bits[4:2] word length (011 = 16 bit), bit6 set = input muted"},
    {0x0D, "power: 0x01 = running, 0xFA = powered down"},
    {0x12, "DAC: 0x00 = enabled, 0x02 = powered down"},
    {0x31, "DAC mute: 0x60 set = muted"},
    {0x32, "DAC volume: 0x00 = silent, ~0xA8 at 70%, 0xBF ~ 0 dB"},
};

void dump_codec() {
  for (const RegProbe& probe : kProbes) {
    int value = 0;
    if (esp_codec_dev_read_reg(s_dev, probe.reg, &value) == ESP_CODEC_DEV_OK) {
      ESP_LOGI(kTag, "  reg 0x%02X = 0x%02X  %s", probe.reg, value, probe.meaning);
    } else {
      ESP_LOGW(kTag, "  reg 0x%02X unreadable — the control port stopped answering",
               probe.reg);
    }
  }
}
#else
inline void dump_codec() {}
#endif

void play_tone(uint32_t ms, uint8_t volume_pct) {
  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = kBitsPerSample,
      .channel = kChannels,
      .channel_mask = 0,
      .sample_rate = kSampleRateHz,
      .mclk_multiple = kMclkMultiple,
  };
  // open() is what powers the DAC and raises the amplifier's enable line, and
  // close() drops it again — which is why the whole tone is bracketed by them
  // rather than left open. The FM8002E idles at a few mA and pops audibly if it
  // is held enabled between alarms.
  // The first tone after a boot logs `i2s_common: i2s_channel_disable(): the
  // channel has not been enabled yet` at E level. It is expected and harmless:
  // esp_codec_dev's set_fmt disables the channel before reconfiguring it, and on
  // the first open there is nothing to disable. The return is discarded and the
  // open succeeds. It does not repeat, because close() leaves the channel disabled
  // and every later open finds the state that disable expects.
  const int open_err = esp_codec_dev_open(s_dev, &fs);
  if (open_err != ESP_CODEC_DEV_OK) {
    ESP_LOGE(kTag, "could not open the codec for playback: %d", open_err);
    return;
  }

  const int vol = std::min<int>(volume_pct, kVolumeMaxPct);
  const int vol_err = esp_codec_dev_set_out_vol(s_dev, vol);
  if (vol_err != ESP_CODEC_DEV_OK) {
    ESP_LOGW(kTag, "volume %d%% rejected (%d); playing at whatever the codec had",
             vol, vol_err);
  }
  // The codec's own view of itself, read back after the DAC is powered and the
  // volume written. This separates "the codec is not playing" from "the codec was
  // never configured", which look identical from outside and share no fix.
#if CONFIG_THERMOPOST_AUDIO_DEBUG
  ESP_LOGI(kTag, "playing %" PRIu32 " ms at %d%% — the default curve puts 100%% at "
                 "0 dB and 0%% at silence", ms, vol);
#endif
  dump_codec();

  uint32_t left = ms_to_samples(ms);
  bool sounding = true;
  while (left > 0 && !s_stop) {
    const uint32_t seg = std::min(ms_to_samples(sounding ? kBeepOnMs : kBeepOffMs), left);
    if (!write_samples(sounding ? s_tone : s_silence, seg)) break;
    left -= seg;
    sounding = !sounding;
  }
  esp_codec_dev_close(s_dev);
}

// Everything between a probed address and a playable codec. Split out of init()
// so the hardware stack can be stood up independently of the probe and the task.
esp_err_t bring_up() {
  esp_err_t err = start_i2s();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "I2S would not start: %s", esp_err_to_name(err));
    teardown();
    return err;
  }

  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = 0,
      .addr = static_cast<uint8_t>(s_addr << kAddrShift),
      .bus_handle = s_bus,
      .clock_speed_hz = kCodecI2cFreqHz,
  };
  s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

  audio_codec_i2s_cfg_t i2s_cfg = {
      .port = 0,
      .rx_handle = nullptr,
      .tx_handle = s_tx,
      .clk_src = 0,
  };
  s_data_if = audio_codec_new_i2s_data(&i2s_cfg);

  es8311_codec_cfg_t codec_cfg = {};
  codec_cfg.ctrl_if = s_ctrl_if;
  codec_cfg.gpio_if = audio_codec_new_gpio();
  codec_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  // The driver owns the amplifier's enable line, so it goes up and down with the
  // DAC instead of needing a second thing to remember.
  codec_cfg.pa_pin = board::kAudioAmpEnable;
  codec_cfg.pa_reverted = board::kAudioAmpActiveLow;
  codec_cfg.use_mclk = board::kI2sMclkRouted;
  codec_cfg.mclk_div = kMclkMultiple;
  // hw_gain left at its defaults: it exists to back the amplifier's gain out of
  // the requested decibels, and the FM8002E's is unmeasured on this board. The
  // consequence is only that the volume percentage is the codec's own attenuation
  // curve and not a calibrated loudness.
  s_codec_if = es8311_codec_new(&codec_cfg);

  if (s_ctrl_if == nullptr || s_data_if == nullptr || s_codec_if == nullptr) {
    ESP_LOGE(kTag, "codec at 0x%02X answered but would not initialise", s_addr);
    teardown();
    return ESP_FAIL;
  }

  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_OUT,
      .codec_if = s_codec_if,
      .data_if = s_data_if,
  };
  s_dev = esp_codec_dev_new(&dev_cfg);
  if (s_dev == nullptr) {
    ESP_LOGE(kTag, "could not create the codec device");
    teardown();
    return ESP_FAIL;
  }
  return ESP_OK;
}

void log_wiring() {
  ESP_LOGI(kTag, "playback on IO%d, bclk IO%d, ws IO%d, mclk %s, amp enable IO%d "
                 "active %s",
           board::kI2sDout, board::kI2sBclk, board::kI2sLrck,
           board::kI2sMclkRouted ? "external on IO4" : "derived from bclk",
           board::kAudioAmpEnable, board::kAudioAmpActiveLow ? "low" : "high");
}

void tone_task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    play_tone(s_req_ms, s_req_vol);
    s_stop = false;
    s_playing = false;
  }
}

}  // namespace

esp_err_t init() {
  if (s_started) return ESP_ERR_INVALID_STATE;

  s_bus = board_i2c::bus();
  if (s_bus == nullptr) {
    ESP_LOGE(kTag, "I2C bus is not up — call board_i2c::init() first");
    return ESP_ERR_INVALID_STATE;
  }

  s_addr = probe_codec(s_bus);
  if (s_addr == 0) {
    ESP_LOGE(kTag, "no ES8311 at 0x%02X or 0x%02X — alarms will be visual only",
             board::kEs8311I2cAddrLow, board::kEs8311I2cAddrHigh);
    return ESP_ERR_NOT_FOUND;
  }

  const esp_err_t err = bring_up();
  if (err != ESP_OK) return err;

  build_tone();

  if (xTaskCreatePinnedToCore(tone_task, "audio", kTaskStack, nullptr, kTaskPrio,
                              &s_task, kTaskCore) != pdPASS) {
    teardown();
    return ESP_ERR_NO_MEM;
  }

  s_started = true;
  // The resolved map, not the documented one: three of these can be flipped from
  // menuconfig, and a log that prints the defaults would be worse than none while
  // someone is bisecting them.
  ESP_LOGI(kTag, "ES8311 at 0x%02X", s_addr);
  log_wiring();
  ESP_LOGI(kTag, "%" PRIu32 " Hz tone, %" PRIu32 " Hz sample rate, %" PRIu32
                 " ms on / %" PRIu32 " ms off",
           kToneHz, kSampleRateHz, kBeepOnMs, kBeepOffMs);
  return ESP_OK;
}

bool available() { return s_started; }

bool playing() { return s_playing; }

esp_err_t play(uint32_t ms, uint8_t volume_pct) {
  if (!s_started) return ESP_ERR_INVALID_STATE;
  // Not a lock: the tone task is the only writer of s_playing once it is set, so
  // the worst a race between two callers can do is start one tone instead of two.
  if (s_playing) return ESP_ERR_INVALID_STATE;
  s_req_ms = ms;
  s_req_vol = volume_pct;
  s_stop = false;
  // Set here rather than in the task, so playing() is already true by the time
  // play() returns and a caller can render the state without waiting on a
  // scheduling decision.
  s_playing = true;
  xTaskNotifyGive(s_task);
  return ESP_OK;
}

void stop() {
  if (!s_started) return;
  s_stop = true;
}

}  // namespace thermopost::audio
