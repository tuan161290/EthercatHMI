/*
 * Small expedited-SDO helpers (no Complete Access: the Leadshine slaves do not support it).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "soem/soem.h"

static inline bool ecat_sdo_read(ecx_contextt *ctx, uint16_t slave, uint16_t index, uint8_t sub, void *buf, int size)
{
    int len = size;
    memset(buf, 0, (size_t)size);
    return ecx_SDOread(ctx, slave, index, sub, FALSE, &len, buf, EC_TIMEOUTRXM) > 0;
}

/* Unsigned object of 1..4 bytes. The buffer is always 4 bytes: a slave that answers an
 * expedited upload without the size indicator sends 4 bytes, which SOEM refuses to copy into
 * a smaller buffer (ec_coe.c ecx_SDOread). Little-endian target, so zero-extension is free. */
static inline bool ecat_sdo_read_uint(ecx_contextt *ctx, uint16_t slave, uint16_t index, uint8_t sub, uint32_t *v)
{
    return ecat_sdo_read(ctx, slave, index, sub, v, sizeof(*v));
}

static inline bool ecat_sdo_write(ecx_contextt *ctx, uint16_t slave, uint16_t index, uint8_t sub, const void *buf,
                                  int size)
{
    return ecx_SDOwrite(ctx, slave, index, sub, FALSE, size, buf, EC_TIMEOUTRXM) > 0;
}

static inline bool ecat_sdo_write_u8(ecx_contextt *ctx, uint16_t slave, uint16_t index, uint8_t sub, uint8_t v)
{
    return ecat_sdo_write(ctx, slave, index, sub, &v, 1);
}

static inline bool ecat_sdo_write_u16(ecx_contextt *ctx, uint16_t slave, uint16_t index, uint8_t sub, uint16_t v)
{
    return ecat_sdo_write(ctx, slave, index, sub, &v, 2);
}

static inline bool ecat_sdo_write_u32(ecx_contextt *ctx, uint16_t slave, uint16_t index, uint8_t sub, uint32_t v)
{
    return ecat_sdo_write(ctx, slave, index, sub, &v, 4);
}
