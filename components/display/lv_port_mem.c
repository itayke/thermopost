// Custom LVGL allocator backed by PSRAM (selected via LV_USE_CUSTOM_MALLOC /
// CONFIG_LV_USE_CUSTOM_MALLOC=y). This moves LVGL's widget pool, style data and
// image buffers off scarce internal RAM into the 8 MB PSRAM.
//
// Why this exists: the SPI master can only DMA color data out of INTERNAL RAM,
// so display.cpp's two 25 KB draw buffers have to stay internal. Once Wi-Fi
// also claims its share, a fixed internal LVGL pool on top of that gets tight.
// LVGL's own allocations are CPU-only — the final pixels land in the internal
// draw buffers, never in LVGL's pool — so routing them to PSRAM is free of the
// SPI constraint. It also removes the fixed pool ceiling entirely: allocations
// simply grow into PSRAM as screens are built and torn down.
//
// Implements the full lv_*_core surface LVGL expects when LV_STDLIB_CUSTOM is
// selected (the builtin mem core is compiled out). C linkage on purpose.

#include "lvgl.h"

#include "esp_heap_caps.h"

void lv_mem_init(void) { /* nothing to init — heap_caps owns the PSRAM heap */ }

void lv_mem_deinit(void) { /* nothing to deinit */ }

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
  (void)mem;
  (void)bytes;
  return NULL;  // single PSRAM heap; explicit pools unsupported
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void* lv_malloc_core(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void* lv_realloc_core(void* p, size_t new_size) {
  return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void* p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
  const size_t total = info.total_free_bytes + info.total_allocated_bytes;
  mon_p->total_size        = total;
  mon_p->free_cnt          = info.free_blocks;
  mon_p->free_size         = info.total_free_bytes;
  mon_p->free_biggest_size = info.largest_free_block;
  mon_p->used_cnt          = info.allocated_blocks;
  mon_p->max_used          = info.total_allocated_bytes;
  mon_p->used_pct =
      total ? (uint8_t)((info.total_allocated_bytes * 100) / total) : 0;
  mon_p->frag_pct =
      info.total_free_bytes
          ? (uint8_t)(100 - (info.largest_free_block * 100) / info.total_free_bytes)
          : 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
