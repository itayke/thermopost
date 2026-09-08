#include "display.hpp"

#include <cstdio>
#include <cstdlib>

#include "board_pins.hpp"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

namespace thermopost::display {

// External linkage within this namespace so lock()/unlock() at the bottom of
// the file can reach it.
SemaphoreHandle_t s_lvgl_mutex = nullptr;

namespace {

constexpr const char* kTag = "display";

constexpr spi_host_device_t kSpiHost = SPI2_HOST;

// Partial draw buffer: 40 lines × 320 px × 2 B = 25 KiB per buffer, two of them
// in internal SRAM. Full-frame would be 150 KiB, which fits but buys nothing —
// the ILI9341's SPI link, not the render, is the bottleneck.
constexpr int    kDrawBufLines = 40;
constexpr size_t kDrawBufBytes =
    static_cast<size_t>(kDrawBufLines) * board::kLcdHRes * sizeof(uint16_t);

// Landscape orientation, set from what the panel actually did rather than from
// the datasheet. The glass is 240×320 portrait; swapping X/Y in the panel's
// MADCTL turns it into the 320×240 LVGL sees, and the mirror flags pick which
// of the four landscape rotations that lands on.
//
// Mind the axes: the mirror flags are in PANEL coordinates, which swap_xy has
// already transposed relative to the viewer. So kMirrorX flips what you see as
// the vertical axis, not the horizontal one — clearing it is what put the image
// the right way up. Any future flip belongs here rather than in LVGL: a
// panel-side flip is free, an LVGL rotation costs a full software transform on
// every flush.
constexpr bool kSwapXy   = true;
constexpr bool kMirrorX  = false;
constexpr bool kMirrorY  = false;

// This unit's panel needs INVON. Confirmed empirically: with inversion off it
// came up black-on-white with a 0x00A0FF accent rendering orange-red, and
// 0x00A0FF inverted is exactly 0xFF5F00. One fault, two symptoms.
//
// Worth distinguishing from the other way colours go wrong here: a BGR/RGB
// mismatch swaps red and blue but leaves black black and white white. Black
// and white trading places means inversion, every time.
//
// Common on the "IPS" ILI9341 variants — which vendor listings for this board
// do advertise — where the panel is wired normally-black rather than
// normally-white and the controller has to compensate.
constexpr bool kInvertColor = true;

// Backlight PWM. 5 kHz is well above anything audible from the inductor and
// well below the point where the LED driver's switching losses matter; 10-bit
// resolution gives 1024 steps, far finer than the 101 the API exposes.
constexpr ledc_timer_t      kBacklightTimer   = LEDC_TIMER_0;
constexpr ledc_channel_t    kBacklightChannel = LEDC_CHANNEL_0;
constexpr ledc_mode_t       kBacklightMode    = LEDC_LOW_SPEED_MODE;  // S3 has no high-speed mode
constexpr ledc_timer_bit_t  kBacklightResBits = LEDC_TIMER_10_BIT;
constexpr uint32_t          kBacklightFreqHz  = 5'000;
constexpr uint32_t          kBacklightMaxDuty = (1u << kBacklightResBits) - 1;
constexpr uint8_t           kBrightnessPctMax = 100;
constexpr uint8_t           kBrightnessDefaultPct = 100;

// LVGL tick period. Fed from esp_timer rather than the FreeRTOS tick so
// animations stay smooth even when the LVGL task is briefly preempted.
//
// **esp_timer dispatches every callback from one shared task, so any other timer
// in this firmware that blocks stops this clock too.** With no tick LVGL has no
// time: the screen stops repainting and the input device — polled by an LVGL
// timer, not by the driver — stops being read, so touch dies as well. It looks
// like a hung UI and is in fact a slow callback somewhere else entirely (the
// Wi-Fi reconnect timer in cloud.cpp was exactly this). Anything that can block
// belongs in a task, not in an esp_timer callback.
constexpr int64_t kLvglTickPeriodUs = 2'000;
constexpr uint32_t kLvglTickMs = kLvglTickPeriodUs / 1000;

// Bounds on how long the LVGL task sleeps between lv_timer_handler() calls.
constexpr uint32_t kLvglIdleMaxMs = 30;
constexpr uint32_t kLvglIdleMinMs = 2;

constexpr int kLvglTaskStack = 8192;
constexpr int kLvglTaskPrio  = 2;
constexpr int kLvglTaskCore  = 1;

esp_lcd_panel_io_handle_t s_panel_io  = nullptr;
esp_lcd_panel_handle_t    s_panel     = nullptr;
lv_display_t*             s_lvgl_disp = nullptr;

void*        s_draw_buf_a = nullptr;
void*        s_draw_buf_b = nullptr;
TaskHandle_t s_lvgl_task  = nullptr;

uint8_t s_brightness_pct = kBrightnessDefaultPct;

bool on_color_trans_done(esp_lcd_panel_io_handle_t /*io*/,
                         esp_lcd_panel_io_event_data_t* /*evt*/,
                         void* user_ctx) {
  auto* disp = static_cast<lv_display_t*>(user_ctx);
  lv_display_flush_ready(disp);
  return false;
}

void flush_cb(lv_display_t* /*disp*/, const lv_area_t* area, uint8_t* px_map) {
  // LVGL is configured with LV_COLOR_FORMAT_RGB565_SWAPPED, so the software
  // renderer already emits the big-endian byte order the ILI9341 wants — no
  // CPU byte-swap on the flush hot path.
  esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                            area->x2 + 1, area->y2 + 1, px_map);
}

void lvgl_tick_cb(void* /*arg*/) { lv_tick_inc(kLvglTickMs); }

void lvgl_task(void* /*arg*/) {
  ESP_LOGI(kTag, "LVGL task started");
  for (;;) {
    uint32_t time_to_next_ms = kLvglIdleMaxMs;
    if (xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
      time_to_next_ms = lv_timer_handler();
      xSemaphoreGiveRecursive(s_lvgl_mutex);
    }
    if (time_to_next_ms > kLvglIdleMaxMs) time_to_next_ms = kLvglIdleMaxMs;
    if (time_to_next_ms < kLvglIdleMinMs) time_to_next_ms = kLvglIdleMinMs;
    vTaskDelay(pdMS_TO_TICKS(time_to_next_ms));
  }
}

esp_err_t init_backlight() {
  const ledc_timer_config_t timer_cfg = {
      .speed_mode      = kBacklightMode,
      .duty_resolution = kBacklightResBits,
      .timer_num       = kBacklightTimer,
      .freq_hz         = kBacklightFreqHz,
      .clk_cfg         = LEDC_AUTO_CLK,
  };
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), kTag, "ledc_timer");

  const ledc_channel_config_t ch_cfg = {
      .gpio_num   = board::kLcdBacklight,
      .speed_mode = kBacklightMode,
      .channel    = kBacklightChannel,
      .timer_sel  = kBacklightTimer,
      .duty       = 0,  // stay dark until the panel has something to show
      .hpoint     = 0,
  };
  return ledc_channel_config(&ch_cfg);
}

esp_err_t apply_backlight_duty(uint8_t pct) {
  const uint32_t duty = (kBacklightMaxDuty * pct) / kBrightnessPctMax;
  ESP_RETURN_ON_ERROR(ledc_set_duty(kBacklightMode, kBacklightChannel, duty),
                      kTag, "ledc_set_duty");
  return ledc_update_duty(kBacklightMode, kBacklightChannel);
}

esp_err_t init_spi_bus() {
  const spi_bus_config_t bus_cfg = {
      .mosi_io_num     = board::kLcdMosi,
      .miso_io_num     = board::kLcdMiso,
      .sclk_io_num     = board::kLcdSclk,
      .quadwp_io_num   = -1,
      .quadhd_io_num   = -1,
      .max_transfer_sz = static_cast<int>(kDrawBufBytes) + 16,
  };
  return spi_bus_initialize(kSpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
}

esp_err_t init_panel() {
  const esp_lcd_panel_io_spi_config_t io_cfg = {
      .cs_gpio_num       = board::kLcdCs,
      .dc_gpio_num       = board::kLcdDc,
      .spi_mode          = 0,
      .pclk_hz           = board::kLcdSpiFreqHz,
      .trans_queue_depth = 10,
      .on_color_trans_done = nullptr,  // registered against the LVGL display later
      .user_ctx            = nullptr,
      .lcd_cmd_bits      = 8,
      .lcd_param_bits    = 8,
  };
  ESP_RETURN_ON_ERROR(
      esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost),
                               &io_cfg, &s_panel_io),
      kTag, "panel_io_spi");

  const esp_lcd_panel_dev_config_t panel_cfg = {
      .reset_gpio_num = board::kLcdReset,  // -1: reset is tied to the module rail
      // Most ILI9341 modules wire the panel BGR. If red and blue come out
      // swapped, this is the single line to change — nothing else in the stack
      // reorders channels.
      .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
      .bits_per_pixel = board::kLcdBitsPerPixel,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(s_panel_io, &panel_cfg, &s_panel),
                      kTag, "new_panel_ili9341");

  // With reset_gpio_num == -1 this issues the software reset command (0x01)
  // instead of pulsing a pin, which is what we want on this board.
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), kTag, "panel_reset");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), kTag, "panel_init");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, kSwapXy), kTag, "swap_xy");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, kMirrorX, kMirrorY), kTag, "mirror");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, kInvertColor), kTag, "invert");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), kTag, "disp_on");
  return ESP_OK;
}

esp_err_t init_lvgl() {
  lv_init();

  s_lvgl_disp = lv_display_create(board::kLcdHRes, board::kLcdVRes);
  if (s_lvgl_disp == nullptr) return ESP_ERR_NO_MEM;

  // Draw buffers MUST be internal DMA RAM — the SPI master cannot source
  // color data from PSRAM. Internal headroom for them comes from keeping
  // LVGL's own allocator pool in PSRAM (lv_port_mem.c / LV_USE_CUSTOM_MALLOC).
  s_draw_buf_a = heap_caps_malloc(kDrawBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  s_draw_buf_b = heap_caps_malloc(kDrawBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (s_draw_buf_a == nullptr || s_draw_buf_b == nullptr) {
    ESP_LOGE(kTag, "failed to allocate LVGL draw buffers (need %u B each)",
             static_cast<unsigned>(kDrawBufBytes));
    return ESP_ERR_NO_MEM;
  }

  lv_display_set_buffers(s_lvgl_disp, s_draw_buf_a, s_draw_buf_b, kDrawBufBytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_color_format(s_lvgl_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_flush_cb(s_lvgl_disp, flush_cb);

  // Fire flush_ready exactly when DMA completes, rather than after a guess.
  const esp_lcd_panel_io_callbacks_t cbs = {
      .on_color_trans_done = on_color_trans_done,
  };
  ESP_RETURN_ON_ERROR(
      esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, s_lvgl_disp),
      kTag, "register_color_trans_done");

  const esp_timer_create_args_t tick_args = {
      .callback = &lvgl_tick_cb,
      .name     = "lvgl_tick",
  };
  esp_timer_handle_t tick_timer = nullptr;
  ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), kTag, "tick_create");
  ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, kLvglTickPeriodUs),
                      kTag, "tick_start");

  s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
  if (s_lvgl_mutex == nullptr) return ESP_ERR_NO_MEM;

  const BaseType_t task_ok =
      xTaskCreatePinnedToCore(lvgl_task, "lvgl", kLvglTaskStack, nullptr,
                              kLvglTaskPrio, &s_lvgl_task, kLvglTaskCore);
  return task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

// --- Screenshots ------------------------------------------------------------
// `shot` on the console prints the active screen as base64 and
// tools/screenshot.py turns it back into a PNG. The point is documentation: a
// real 320x240 capture beats a photograph of a lit panel held at an angle.
//
// A re-render rather than a framebuffer read, because there is no framebuffer to
// read — the draw buffers above are 40 lines each, and the only complete image
// lives inside the ILI9341, which this wiring cannot read back. lv_snapshot_take()
// draws the screen again into a buffer of its own, so a post nobody is
// screenshotting pays nothing for this beyond the code.
constexpr lv_color_format_t kShotFormat     = LV_COLOR_FORMAT_RGB565;
constexpr const char*       kShotFormatName = "rgb565";

// Base64 only concatenates on 3-byte boundaries — a chunk size that is not a
// multiple of 3 puts padding in the middle of the stream, where a decoder stops.
// 768 bytes encode to exactly 1024 characters and divide a 320x240 frame into 200
// whole lines.
constexpr size_t kShotChunkBytes = 768;
constexpr size_t kShotChunkChars = kShotChunkBytes / 3 * 4;
static_assert(kShotChunkBytes % 3 == 0, "base64 concatenates only on 3-byte bounds");

// Data lines carry a marker because the REPL and ESP_LOG share the one USB serial
// channel: an INFO line landing between two chunks would corrupt the image, and a
// prefix lets the host keep only what it asked for.
constexpr const char* kShotLinePrefix = "S:";

// Long enough to outlast a frame the LVGL task is mid-render on, short enough that
// a wedged UI reports itself rather than hanging the console.
constexpr uint32_t kShotLockTimeoutMs = 1000;

int cmd_shot(int /*argc*/, char** /*argv*/) {
  if (s_lvgl_disp == nullptr) {
    std::printf("display is not up\n");
    return 1;
  }
  if (!lock(kShotLockTimeoutMs)) {
    std::printf("could not take the LVGL lock\n");
    return 1;
  }
  // LVGL's own state, not the panel's, so a screen the idle ladder has blanked
  // still captures exactly what a wake would put back on it.
  lv_draw_buf_t* shot = lv_snapshot_take(lv_screen_active(), kShotFormat);
  unlock();
  if (shot == nullptr) {
    std::printf("snapshot failed — no PSRAM for a %dx%d frame?\n",
                board::kLcdHRes, board::kLcdVRes);
    return 1;
  }

  char* line = static_cast<char*>(std::malloc(kShotChunkChars + 1));
  if (line == nullptr) {
    lv_draw_buf_destroy(shot);
    std::printf("out of memory for the encode buffer\n");
    return 1;
  }

  // Stride goes in the header rather than being assumed equal to the row: LVGL
  // pads rows to CONFIG_LV_DRAW_BUF_STRIDE_ALIGN, and a host that guessed would
  // shear the image by a pixel per row the day that value changes.
  const uint32_t stride = shot->header.stride;
  const uint32_t bytes  = stride * shot->header.h;
  std::printf("# shot %u %u %u %s\n", static_cast<unsigned>(shot->header.w),
              static_cast<unsigned>(shot->header.h), static_cast<unsigned>(stride),
              kShotFormatName);

  // Encoded off the LVGL lock: the dump takes long enough that holding it would
  // freeze the UI for the whole transfer.
  for (uint32_t off = 0; off < bytes; off += kShotChunkBytes) {
    const size_t left = bytes - off;
    const size_t n    = left < kShotChunkBytes ? left : kShotChunkBytes;
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(line), kShotChunkChars + 1,
                              &written, shot->data + off, n) != 0) {
      std::printf("# shot abort\n");
      break;
    }
    line[written] = '\0';
    std::printf("%s%s\n", kShotLinePrefix, line);
  }
  std::printf("# shot end\n");
  std::fflush(stdout);

  std::free(line);
  // Freed without the lock: nothing else has ever seen this buffer, and with
  // LV_USE_CUSTOM_MALLOC the destroy is a heap_caps_free (lv_port_mem.c).
  lv_draw_buf_destroy(shot);
  return 0;
}

}  // namespace

esp_err_t init() {
  if (s_panel != nullptr) return ESP_ERR_INVALID_STATE;
  ESP_RETURN_ON_ERROR(init_backlight(), kTag, "backlight");
  ESP_RETURN_ON_ERROR(init_spi_bus(),   kTag, "spi_bus");
  ESP_RETURN_ON_ERROR(init_panel(),     kTag, "panel");
  ESP_RETURN_ON_ERROR(init_lvgl(),      kTag, "lvgl");
  ESP_LOGI(kTag, "display + LVGL ready (%dx%d, %d-line partial buffers)",
           board::kLcdHRes, board::kLcdVRes, kDrawBufLines);
  return ESP_OK;
}

lv_display_t* lvgl_display() { return s_lvgl_disp; }

esp_err_t register_console() {
  // Trailing members left defaulted for the same reason power.cpp leaves them:
  // esp_console_cmd_t has grown fields across IDF releases.
  const esp_console_cmd_t cmd = {
      .command = "shot",
      .help = "print the active screen as base64; see tools/screenshot.py",
      .hint = nullptr,
      .func = &cmd_shot,
  };
  return esp_console_cmd_register(&cmd);
}

bool lock(uint32_t timeout_ms) {
  if (s_lvgl_mutex == nullptr) return false;
  const TickType_t ticks =
      (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(s_lvgl_mutex, ticks) == pdTRUE;
}

void unlock() {
  if (s_lvgl_mutex != nullptr) xSemaphoreGiveRecursive(s_lvgl_mutex);
}

esp_err_t set_brightness(uint8_t pct) {
  if (pct > kBrightnessPctMax) pct = kBrightnessPctMax;
  s_brightness_pct = pct;
  return apply_backlight_duty(pct);
}

uint8_t brightness() { return s_brightness_pct; }

esp_err_t set_on(bool on) {
  if (s_panel == nullptr) return ESP_ERR_INVALID_STATE;
  // Backlight first when going dark, last when coming back — otherwise the
  // panel's scan-on transient is visible as a flash of garbage.
  if (!on) {
    ESP_RETURN_ON_ERROR(apply_backlight_duty(0), kTag, "backlight_off");
    return esp_lcd_panel_disp_on_off(s_panel, false);
  }
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), kTag, "disp_on");
  return apply_backlight_duty(s_brightness_pct);
}

}  // namespace thermopost::display
