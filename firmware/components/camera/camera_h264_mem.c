/*
 * Keeps the H.264 encoder's large buffers out of internal RAM.
 *
 * esp_h264 1.3.0 asks for its hardware encoder's line buffers in internal RAM: `ref`, 1152 bytes per
 * macroblock column (esp_h264_enc_hw_param.c:264-268 and :364; 46 KB at 640x360, 138 KB at
 * 1920x1080) and `db_tmp` (esp_h264_enc_single_hw.c:363, 5-15 KB), and it prefers internal RAM for `db`
 * (esp_h264_enc_hw_param.c:366). With Wi-Fi running this firmware has about 32 KB of internal RAM in one
 * piece (CLAUDE.md), so the encoder would not start, or would take what Wi-Fi and the HMI need.
 *
 * The linker sends esp_h264's calls of these two allocators here (--wrap in CMakeLists.txt): a request
 * above SMALL_BYTES for internal RAM is made in PSRAM instead. The DMA descriptors (16-32 bytes) and
 * the small control blocks stay in internal RAM. The H.264 DMA reads and writes PSRAM anyway (input
 * frame, bitstream).
 */

#include <stdint.h>
#include "esp_h264_alloc.h"
#include "esp_heap_caps.h"

#define SMALL_BYTES 1024

void *__real_esp_h264_aligned_calloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size,
                                     uint32_t caps);
void *__real_esp_h264_calloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps1,
                                    uint32_t caps2);
void *__wrap_esp_h264_aligned_calloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size,
                                     uint32_t caps);
void *__wrap_esp_h264_calloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps1,
                                    uint32_t caps2);

static bool large_internal(uint32_t n, uint32_t size, uint32_t caps)
{
    return (caps & MALLOC_CAP_INTERNAL) && (uint64_t)n * size > SMALL_BYTES;
}

void *__wrap_esp_h264_aligned_calloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size,
                                     uint32_t caps)
{
    if (large_internal(n, size, caps)) {
        caps = (caps & ~MALLOC_CAP_INTERNAL) | MALLOC_CAP_SPIRAM;
    }
    return __real_esp_h264_aligned_calloc(alignment, n, size, actual_size, caps);
}

void *__wrap_esp_h264_calloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps1,
                                    uint32_t caps2)
{
    if (large_internal(n, size, caps1) && (caps2 & MALLOC_CAP_SPIRAM)) {
        return __real_esp_h264_calloc_prefer(n, size, actual_size, caps2, caps1); /* PSRAM first */
    }
    return __real_esp_h264_calloc_prefer(n, size, actual_size, caps1, caps2);
}
