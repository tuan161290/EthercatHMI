/*
 * LVGL heap (CONFIG_LV_USE_CUSTOM_MALLOC): PSRAM first, internal RAM only as a fallback.
 *
 * Objects, styles and the TinyTTF glyph cache are small allocations that the default C malloc
 * would place in internal RAM (below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL). Internal RAM is kept
 * for EtherCAT, the EMAC DMA and the LVGL draw buffer.
 */

#include "esp_heap_caps.h"
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define CAPS_PSRAM    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CAPS_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, CAPS_PSRAM, CAPS_INTERNAL);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc_prefer(p, new_size, 2, CAPS_PSRAM, CAPS_INTERNAL);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, CAPS_PSRAM);
    mon->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_size = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->max_used = info.total_free_bytes + info.total_allocated_bytes - info.minimum_free_bytes;
    mon->used_cnt = info.allocated_blocks;
    mon->free_cnt = info.free_blocks;
    mon->used_pct = mon->total_size ? (uint8_t)(100 - (uint64_t)mon->free_size * 100 / mon->total_size) : 0;
    mon->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(CAPS_PSRAM, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

#endif
