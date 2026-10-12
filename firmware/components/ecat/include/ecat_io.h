/*
 * Process image of the IO couplers (R1EC) for the HMI: IO monitor and output forcing.
 *
 * Channels are numbered over the whole bus, in rack and slot order:
 *   digital inputs  X<byte>.<bit>  (X0.0 = first DI channel of the first coupler),
 *   digital outputs Y<byte>.<bit>.
 * Channel n of a module is bit n of its process data, LSB first (R1EC ESI SC-1600,
 * vendor/ESI/Leadshine/R1EC/R1EC-v1.4.0.0.xml:1056-1080: "IN BIT1" is the first entry).
 * Analog modules: SC-A0400-IV has 4 INT inputs then 4 USINT diagnostics (ESI :3062-3110),
 * SC-A0004-IV 4 INT outputs and 4 USINT diagnostics (ESI :3622-3670).
 *
 * Force mode: the HMI drives single outputs. There is no machine logic yet, so an output
 * that is not forced is 0; leaving force mode switches every forced output off.
 *
 * Threads, as ecat_axis.h: core 0 reads snapshots (seqlock) and queues force commands;
 * core 1 (ecat task) binds, runs ecat_io_cycle() every cycle and unbinds.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ecat_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ECAT_IO_MAX_COUPLERS ECAT_TOPO_MAX_RACKS
#define ECAT_IO_IMAGE_BYTES  128 /*!< Per coupler and direction; larger images are cut */
#define ECAT_IO_MOD_NAME_LEN 16

typedef struct {
    uint32_t ident;
    char name[ECAT_IO_MOD_NAME_LEN]; /*!< Catalog name, "SC-1600" */
    uint8_t slot;
    uint8_t di, dout;                /*!< Digital channels */
    uint8_t ai, ao;                  /*!< Analog channels (INT) */
    uint16_t in_bit, out_bit;        /*!< Offset in the coupler's input / output image */
    uint16_t in_bits, out_bits;
    uint16_t x_first, y_first;       /*!< Global number of the first DI / DO channel */
} ecat_io_module_t;

typedef struct {
    char name[ECAT_TOPO_NAME_LEN];   /*!< "RIO1" */
    uint16_t slave;
    uint16_t in_bytes, out_bytes;    /*!< Image bytes copied (at most ECAT_IO_IMAGE_BYTES) */
    uint8_t n_modules;
    ecat_io_module_t mod[ECAT_TOPO_MAX_MODULES];
} ecat_io_coupler_t;

/** Static part, published when the bus reaches or leaves OP (about 5 KB: keep it static). */
typedef struct {
    uint32_t gen;
    bool in_op;
    uint8_t n;
    uint16_t n_di, n_do, n_ai, n_ao;
    ecat_io_coupler_t c[ECAT_IO_MAX_COUPLERS];
} ecat_io_layout_t;

/** Cyclic part. */
typedef struct {
    uint32_t gen;            /*!< Layout generation the image belongs to */
    bool in_op;
    bool bus_ok;
    bool force;              /*!< Force mode is on */
    uint32_t device_status[ECAT_IO_MAX_COUPLERS]; /*!< R1EC Device Status, 0 = no fault */
    uint8_t in[ECAT_IO_MAX_COUPLERS][ECAT_IO_IMAGE_BYTES];
    uint8_t out[ECAT_IO_MAX_COUPLERS][ECAT_IO_IMAGE_BYTES];
} ecat_io_image_t;

/* ---- Core 0 ---- */

uint32_t ecat_io_layout_gen(void);
void ecat_io_layout_read(ecat_io_layout_t *out);
void ecat_io_image_read(ecat_io_image_t *out);

/** Switch force mode on or off. @return false if the queue is full */
bool ecat_io_force_mode(bool on);

/** Force output bit `out_bit` of coupler `coupler` (only in force mode). @return false if the queue is full */
bool ecat_io_force_bit(uint8_t coupler, uint16_t out_bit, bool value);

/** @return bit `bit` of an image row */
static inline bool ecat_io_bit(const uint8_t *img, uint16_t bit)
{
    return bit / 8 < ECAT_IO_IMAGE_BYTES && (img[bit / 8] >> (bit % 8)) & 1u;
}

/* ---- Core 1, ecat task ---- */

struct ecx_context;

void ecat_io_init(void);

/** After the bus reached OP: map the prepared couplers (names from the accepted topology). */
void ecat_io_bind(struct ecx_context *ctx, const ecat_topology_t *bus);

/** Every cycle after the process data exchange. */
void ecat_io_cycle(bool wkc_ok);

/** Bus left OP: force mode off, layout empty. */
void ecat_io_unbind(void);

#ifdef __cplusplus
}
#endif
