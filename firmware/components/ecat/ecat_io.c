/*
 * IO coupler process image for the HMI (ecat_io.h).
 *
 * ecat_io_cycle() runs in the cyclic task: no allocation, no logging, no blocking. It copies at
 * most 2 x 128 bytes per coupler into the snapshot.
 */

#include <stdatomic.h>
#include <string.h>
#include "ecat_coupler.h"
#include "ecat_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "soem/soem.h"

static const char *TAG = "ecat_io";

#define CMD_QUEUE_LEN  16
#define CMDS_PER_CYCLE 4

/* Analog modules of the R1EC (ecat_io.h). */
#define IDENT_SC_A0400_IV 0x71000025u
#define IDENT_SC_A0004_IV 0x71000205u

typedef struct {
    uint8_t code; /* 0: force mode, 1: force bit */
    uint8_t coupler;
    uint8_t value;
    uint16_t bit;
} io_cmd_t;

typedef struct {
    uint16_t slave;
    uint8_t *in;
    uint8_t *out;
    uint16_t in_bytes, out_bytes;
    bool has_status;
} io_slot_t;

static QueueHandle_t s_q;
static io_slot_t s_c[ECAT_IO_MAX_COUPLERS];
static uint8_t s_n;
static bool s_in_op;
static bool s_force;
static uint8_t s_fmask[ECAT_IO_MAX_COUPLERS][ECAT_IO_IMAGE_BYTES];
static uint8_t s_fval[ECAT_IO_MAX_COUPLERS][ECAT_IO_IMAGE_BYTES];

/* Snapshots, seqlock: one writer (ecat task), readers on core 0. */
static atomic_uint s_lseq, s_iseq;
static ecat_io_layout_t *s_layout; /* ~5 KB each, PSRAM: written only at bind/unbind */
static ecat_io_layout_t *s_tmp;
static ecat_io_image_t s_image;

static void layout_publish(const ecat_io_layout_t *l)
{
    unsigned seq = atomic_load_explicit(&s_lseq, memory_order_relaxed);
    atomic_store_explicit(&s_lseq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    *s_layout = *l;
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s_lseq, seq + 2, memory_order_relaxed);
}

static void image_publish(bool bus_ok)
{
    unsigned seq = atomic_load_explicit(&s_iseq, memory_order_relaxed);
    atomic_store_explicit(&s_iseq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    s_image.gen = s_layout->gen;
    s_image.in_op = s_in_op;
    s_image.bus_ok = bus_ok;
    s_image.force = s_force;
    for (uint8_t k = 0; k < s_n; k++) {
        const io_slot_t *c = &s_c[k];
        memcpy(s_image.in[k], c->in, c->in_bytes);
        memcpy(s_image.out[k], c->out, c->out_bytes);
        s_image.device_status[k] = c->has_status && c->in_bytes >= 4
                                       ? (uint32_t)c->in[0] | ((uint32_t)c->in[1] << 8) |
                                             ((uint32_t)c->in[2] << 16) | ((uint32_t)c->in[3] << 24)
                                       : 0;
    }
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s_iseq, seq + 2, memory_order_relaxed);
}

/* ---- Core 0 ---- */

uint32_t ecat_io_layout_gen(void)
{
    unsigned before, after;
    uint32_t gen;
    do {
        before = atomic_load_explicit(&s_lseq, memory_order_acquire);
        gen = s_layout ? s_layout->gen : 0;
        atomic_thread_fence(memory_order_acquire);
        after = atomic_load_explicit(&s_lseq, memory_order_relaxed);
    } while ((before & 1u) || before != after);
    return gen;
}

void ecat_io_layout_read(ecat_io_layout_t *out)
{
    unsigned before, after;
    do {
        before = atomic_load_explicit(&s_lseq, memory_order_acquire);
        if (s_layout) {
            *out = *s_layout;
        } else {
            memset(out, 0, sizeof(*out));
        }
        atomic_thread_fence(memory_order_acquire);
        after = atomic_load_explicit(&s_lseq, memory_order_relaxed);
    } while ((before & 1u) || before != after);
}

void ecat_io_image_read(ecat_io_image_t *out)
{
    unsigned before, after;
    do {
        before = atomic_load_explicit(&s_iseq, memory_order_acquire);
        *out = s_image;
        atomic_thread_fence(memory_order_acquire);
        after = atomic_load_explicit(&s_iseq, memory_order_relaxed);
    } while ((before & 1u) || before != after);
}

bool ecat_io_force_mode(bool on)
{
    const io_cmd_t c = {.code = 0, .value = on};
    return s_q && xQueueSend(s_q, &c, 0) == pdTRUE;
}

bool ecat_io_force_bit(uint8_t coupler, uint16_t out_bit, bool value)
{
    const io_cmd_t c = {.code = 1, .coupler = coupler, .bit = out_bit, .value = value};
    return s_q && xQueueSend(s_q, &c, 0) == pdTRUE;
}

/* ---- Core 1 ---- */

void ecat_io_init(void)
{
    if (s_q == NULL) {
        s_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(io_cmd_t));
        s_layout = heap_caps_calloc(1, sizeof(*s_layout), MALLOC_CAP_SPIRAM);
        s_tmp = heap_caps_calloc(1, sizeof(*s_tmp), MALLOC_CAP_SPIRAM);
        configASSERT(s_q && s_layout && s_tmp);
    }
}

static const char *slave_name(const ecat_topology_t *bus, uint16_t slave)
{
    for (uint16_t k = 0; k < bus->n_slaves; k++) {
        if (bus->slave[k].position == slave) {
            return bus->slave[k].name;
        }
    }
    return "RIO?";
}

void ecat_io_bind(struct ecx_context *ctx, const ecat_topology_t *bus)
{
    ecat_io_layout_t *l = s_tmp;
    const uint32_t gen = s_layout->gen + 1;
    memset(l, 0, sizeof(*l));
    memset(s_c, 0, sizeof(s_c));
    memset(s_fmask, 0, sizeof(s_fmask));
    memset(s_fval, 0, sizeof(s_fval));
    s_force = false;
    s_n = 0;

    uint16_t list[ECAT_IO_MAX_COUPLERS];
    const int n = ecat_coupler_list(list, ECAT_IO_MAX_COUPLERS);
    uint16_t x = 0, y = 0;
    for (int k = 0; k < n; k++) {
        const ecat_coupler_t *cp = ecat_coupler_get(list[k]);
        const ec_slavet *sl = &ctx->slavelist[list[k]];
        if (cp == NULL || !ecat_coupler_ready(list[k]) || sl->Istartbit != 0 || sl->Ostartbit != 0) {
            ESP_LOGW(TAG, "coupler at slave %u not mapped for the HMI", list[k]);
            continue;
        }
        io_slot_t *c = &s_c[s_n];
        ecat_io_coupler_t *lc = &l->c[s_n];
        c->slave = list[k];
        c->in = sl->inputs;
        c->out = sl->outputs;
        c->in_bytes = (uint16_t)(sl->Ibytes < ECAT_IO_IMAGE_BYTES ? sl->Ibytes : ECAT_IO_IMAGE_BYTES);
        c->out_bytes = (uint16_t)(sl->Obytes < ECAT_IO_IMAGE_BYTES ? sl->Obytes : ECAT_IO_IMAGE_BYTES);
        c->has_status = cp->dev->status_bits == 32;
        if (c->in == NULL) {
            c->in_bytes = 0;
        }
        if (c->out == NULL) {
            c->out_bytes = 0;
        }
        strlcpy(lc->name, slave_name(bus, list[k]), sizeof(lc->name));
        lc->slave = list[k];
        lc->in_bytes = c->in_bytes;
        lc->out_bytes = c->out_bytes;
        for (uint8_t m = 0; m < cp->rack.n_modules && m < ECAT_TOPO_MAX_MODULES; m++) {
            const ecat_module_info_t *mi = ecat_catalog_module(cp->dev->product, cp->rack.ident[m]);
            ecat_io_module_t *lm = &lc->mod[lc->n_modules++];
            lm->ident = cp->rack.ident[m];
            lm->slot = m;
            lm->in_bit = cp->slot_in_bit[m];
            lm->out_bit = cp->slot_out_bit[m];
            if (mi == NULL) {
                strlcpy(lm->name, "unknown", sizeof(lm->name));
                continue;
            }
            strlcpy(lm->name, mi->name, sizeof(lm->name));
            lm->in_bits = mi->in_bits;
            lm->out_bits = mi->out_bits;
            lm->di = mi->di;
            lm->dout = mi->dout;
            lm->ai = lm->ident == IDENT_SC_A0400_IV ? 4 : 0;
            lm->ao = lm->ident == IDENT_SC_A0004_IV ? 4 : 0;
            lm->x_first = x;
            lm->y_first = y;
            x += lm->di;
            y += lm->dout;
            l->n_di += lm->di;
            l->n_do += lm->dout;
            l->n_ai += lm->ai;
            l->n_ao += lm->ao;
        }
        s_n++;
    }
    l->n = s_n;
    l->gen = gen;
    l->in_op = true;
    xQueueReset(s_q);
    s_in_op = true;
    layout_publish(l);
    image_publish(true);
    ESP_LOGI(TAG, "%u coupler(s) for the HMI: %u DI, %u DO, %u AI, %u AO", s_n, l->n_di, l->n_do, l->n_ai, l->n_ao);
}

static void release_forced(void)
{
    for (uint8_t k = 0; k < s_n; k++) {
        for (uint16_t b = 0; b < s_c[k].out_bytes; b++) {
            s_c[k].out[b] &= (uint8_t)~s_fmask[k][b];
        }
    }
    memset(s_fmask, 0, sizeof(s_fmask));
    memset(s_fval, 0, sizeof(s_fval));
}

void ecat_io_cycle(bool wkc_ok)
{
    if (!s_in_op) {
        return;
    }
    io_cmd_t cmd;
    for (int k = 0; k < CMDS_PER_CYCLE && xQueueReceive(s_q, &cmd, 0) == pdTRUE; k++) {
        if (cmd.code == 0) {
            if (s_force && !cmd.value) {
                release_forced();
            }
            s_force = cmd.value;
        } else if (s_force && cmd.coupler < s_n && cmd.bit / 8 < s_c[cmd.coupler].out_bytes) {
            const uint8_t m = (uint8_t)(1u << (cmd.bit % 8));
            s_fmask[cmd.coupler][cmd.bit / 8] |= m;
            if (cmd.value) {
                s_fval[cmd.coupler][cmd.bit / 8] |= m;
            } else {
                s_fval[cmd.coupler][cmd.bit / 8] &= (uint8_t)~m;
            }
        }
    }
    if (s_force) {
        for (uint8_t k = 0; k < s_n; k++) {
            for (uint16_t b = 0; b < s_c[k].out_bytes; b++) {
                const uint8_t m = s_fmask[k][b];
                if (m) {
                    s_c[k].out[b] = (uint8_t)((s_c[k].out[b] & ~m) | (s_fval[k][b] & m));
                }
            }
        }
    }
    image_publish(wkc_ok);
}

void ecat_io_unbind(void)
{
    ecat_io_layout_t *l = s_tmp;
    s_force = false;
    memset(s_fmask, 0, sizeof(s_fmask));
    memset(s_fval, 0, sizeof(s_fval));
    s_in_op = false;
    s_n = 0;
    memset(l, 0, sizeof(*l));
    l->gen = s_layout->gen + 1;
    layout_publish(l);
    image_publish(false);
}
