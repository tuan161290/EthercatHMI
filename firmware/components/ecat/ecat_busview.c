/*
 * Topology view for the HMI (ecat_busview.h). Seqlock as in hmi.c and ecat_axis.c.
 */

#include <stdatomic.h>
#include <string.h>
#include "esp_attr.h"
#include "ecat_busview.h"

static atomic_uint s_seq;
static EXT_RAM_BSS_ATTR ecat_busview_t s_view; /* ~3 KB, PSRAM: never touched by the cyclic loop */

void ecat_busview_publish(const ecat_busview_src_t *src)
{
    unsigned seq = atomic_load_explicit(&s_seq, memory_order_relaxed);
    atomic_store_explicit(&s_seq, seq + 1, memory_order_relaxed); /* odd: write in progress */
    atomic_thread_fence(memory_order_release);

    s_view.gen++;
    if (src->new_scan) {
        s_view.scan_gen++;
    }
    s_view.phase = src->phase;
    s_view.have_saved = src->have_saved;
    s_view.cmp_valid = src->cmp_valid && src->cmp != NULL;
    s_view.n_blocked = src->n_blocked;
    if (src->meta) {
        s_view.meta = *src->meta;
    } else {
        memset(&s_view.meta, 0, sizeof(s_view.meta));
    }
    if (src->saved && src->have_saved) {
        s_view.saved = *src->saved;
    } else {
        memset(&s_view.saved, 0, sizeof(s_view.saved));
    }
    if (src->bus) {
        s_view.bus = *src->bus;
    } else {
        memset(&s_view.bus, 0, sizeof(s_view.bus));
    }
    if (src->issues) {
        s_view.issues = *src->issues;
    } else {
        memset(&s_view.issues, 0, sizeof(s_view.issues));
    }
    if (s_view.cmp_valid) {
        s_view.cmp = *src->cmp;
    } else {
        memset(&s_view.cmp, 0, sizeof(s_view.cmp));
    }
    strlcpy(s_view.message, src->message ? src->message : "", sizeof(s_view.message));

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s_seq, seq + 2, memory_order_relaxed);
}

uint32_t ecat_busview_gen(void)
{
    unsigned before, after;
    uint32_t gen;
    do {
        before = atomic_load_explicit(&s_seq, memory_order_acquire);
        gen = s_view.gen;
        atomic_thread_fence(memory_order_acquire);
        after = atomic_load_explicit(&s_seq, memory_order_relaxed);
    } while ((before & 1u) || before != after);
    return gen;
}

void ecat_busview_read(ecat_busview_t *out)
{
    unsigned before, after;
    do {
        before = atomic_load_explicit(&s_seq, memory_order_acquire);
        *out = s_view;
        atomic_thread_fence(memory_order_acquire);
        after = atomic_load_explicit(&s_seq, memory_order_relaxed);
    } while ((before & 1u) || before != after);
}
