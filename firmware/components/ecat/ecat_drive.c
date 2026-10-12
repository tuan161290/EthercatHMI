/*
 * CiA402 drive configuration in the PRE-OP -> SAFE-OP hook (ecat_drive.h).
 *
 * The hook writes the PDO mapping and assignment with expedited SDOs (the drives have no
 * Complete Access, L5N:6875), in the usual order: clear the assignment, clear the mapping,
 * write the entries, set the entry count, assign. SOEM then reads the mapping back over CoE
 * and sizes SM2/SM3 itself (ec_config.c ecx_map_coe_soe), which ecat_drive_layout_ok() checks.
 */

#include <inttypes.h>
#include <string.h>
#include "ecat_drive.h"
#include "ecat_sdo.h"
#include "ecat_topology.h"
#include "esp_log.h"

static const char *TAG = "ecat_drv";

#define OBJ_RX_ASSIGN 0x1C12
#define OBJ_TX_ASSIGN 0x1C13
#define OBJ_RX_PDO    0x1600
#define OBJ_TX_PDO    0x1A00

#define MAP(index, sub, bits) (((uint32_t)(index) << 16) | ((uint32_t)(sub) << 8) | (uint32_t)(bits))

/* Same order as ecat_drive_out_t / ecat_drive_in_t. */
static const uint32_t s_rx_map[] = {
    MAP(0x6040, 0, 16), MAP(0x607A, 0, 32), MAP(0x60FF, 0, 32), MAP(0x6081, 0, 32),
    MAP(0x6083, 0, 32), MAP(0x6084, 0, 32), MAP(0x6060, 0, 8),
};
static const uint32_t s_tx_map[] = {
    MAP(0x6041, 0, 16), MAP(0x603F, 0, 16), MAP(0x6061, 0, 8), MAP(0x6064, 0, 32),
    MAP(0x606C, 0, 32), MAP(0x60FD, 0, 32), MAP(0x503F, 0, 16),
};
#define RX_N (sizeof(s_rx_map) / sizeof(s_rx_map[0]))
#define TX_N_FULL (sizeof(s_tx_map) / sizeof(s_tx_map[0]))

typedef struct {
    bool prepared;
    bool want_vendor_error;
    ecat_drive_info_t info;
} drive_t;

static drive_t s_drv[ECAT_TOPO_MAX_SLAVES + 1]; /* by SOEM slave number */

static drive_t *find(uint16_t slave)
{
    return (slave > 0 && slave <= ECAT_TOPO_MAX_SLAVES && s_drv[slave].prepared) ? &s_drv[slave] : NULL;
}

static void flush_sdo_errors(ecx_contextt *ctx, bool log)
{
    ec_errort e;
    while (ecx_poperror(ctx, &e)) {
        if (log && e.Etype == EC_ERR_TYPE_SDO_ERROR) {
            ESP_LOGW(TAG, "slave %u: SDO 0x%04X:%u abort 0x%08" PRIX32 " %s", e.Slave, e.Index, e.SubIdx,
                     (uint32_t)e.AbortCode, ec_sdoerror2string((uint32)e.AbortCode));
        } else if (log) {
            ESP_LOGW(TAG, "slave %u: 0x%04X:%u error type %d code 0x%08" PRIX32, e.Slave, e.Index, e.SubIdx,
                     (int)e.Etype, (uint32_t)e.AbortCode);
        }
    }
}

/* Map `n` entries into `pdo` and assign it alone to the sync manager object `assign`. */
static bool map_pdo(ecx_contextt *ctx, uint16_t slave, uint16_t assign, uint16_t pdo, const uint32_t *entries,
                    uint8_t n)
{
    bool ok = ecat_sdo_write_u8(ctx, slave, assign, 0, 0);
    ok &= ecat_sdo_write_u8(ctx, slave, pdo, 0, 0);
    for (uint8_t k = 0; ok && k < n; k++) {
        ok &= ecat_sdo_write_u32(ctx, slave, pdo, k + 1, entries[k]);
    }
    ok &= ecat_sdo_write_u8(ctx, slave, pdo, 0, n);
    ok &= ecat_sdo_write_u16(ctx, slave, assign, 1, pdo);
    ok &= ecat_sdo_write_u8(ctx, slave, assign, 0, 1);
    return ok;
}

static void read_pair(ecx_contextt *ctx, uint16_t slave, uint16_t index, uint32_t *a, uint32_t *b)
{
    if (!ecat_sdo_read_uint(ctx, slave, index, 1, a) || !ecat_sdo_read_uint(ctx, slave, index, 2, b)) {
        *a = 0;
        *b = 0;
    }
}

static int drive_po2so(ecx_contextt *ctx, uint16_t slave)
{
    drive_t *d = find(slave);
    if (d == NULL) {
        return 0;
    }
    ecat_drive_info_t *info = &d->info;
    memset(info, 0, sizeof(*info));
    info->hook_ran = true;
    flush_sdo_errors(ctx, false);

    bool rx_ok = map_pdo(ctx, slave, OBJ_RX_ASSIGN, OBJ_RX_PDO, s_rx_map, RX_N);
    bool tx_ok = false;
    if (d->want_vendor_error) {
        tx_ok = map_pdo(ctx, slave, OBJ_TX_ASSIGN, OBJ_TX_PDO, s_tx_map, TX_N_FULL);
        info->has_vendor_error = tx_ok;
    }
    if (!tx_ok) {
        flush_sdo_errors(ctx, d->want_vendor_error);
        tx_ok = map_pdo(ctx, slave, OBJ_TX_ASSIGN, OBJ_TX_PDO, s_tx_map, TX_N_FULL - 1);
    }
    info->remap_ok = rx_ok && tx_ok;
    flush_sdo_errors(ctx, !info->remap_ok);

    /* Optional objects: a drive without them simply leaves the fields at 0. */
    read_pair(ctx, slave, 0x608F, &info->enc_inc, &info->enc_rev);
    read_pair(ctx, slave, 0x6091, &info->gear_motor, &info->gear_shaft);
    read_pair(ctx, slave, 0x6092, &info->feed, &info->feed_rev);
    uint32_t method = 0;
    info->home_method_valid = ecat_sdo_read_uint(ctx, slave, 0x6098, 0, &method);
    info->home_method = (int8_t)(uint8_t)method;
    flush_sdo_errors(ctx, false);

    if (info->remap_ok) {
        ESP_LOGI(TAG, "slave %u: PDO remapped (PP/PV/HM%s); encoder %" PRIu32 "/%" PRIu32 ", gear %" PRIu32
                 ":%" PRIu32 ", feed %" PRIu32 "/%" PRIu32 ", homing method %s%d",
                 slave, info->has_vendor_error ? ", 0x503F" : "", info->enc_inc, info->enc_rev, info->gear_motor,
                 info->gear_shaft, info->feed, info->feed_rev, info->home_method_valid ? "" : "? ",
                 info->home_method);
    } else {
        ESP_LOGE(TAG, "slave %u: PDO remap refused (see SDO errors above): the axis stays unavailable", slave);
    }
    return 1;
}

void ecat_drive_reset(void)
{
    memset(s_drv, 0, sizeof(s_drv));
}

esp_err_t ecat_drive_prepare(ecx_contextt *ctx, uint16_t slave, const ecat_device_info_t *dev)
{
    if (slave == 0 || slave > ECAT_TOPO_MAX_SLAVES || slave > ctx->slavecount || dev == NULL ||
        !ecat_kind_is_drive(dev->kind)) {
        return ESP_ERR_INVALID_ARG;
    }
    drive_t *d = &s_drv[slave];
    memset(d, 0, sizeof(*d));
    d->prepared = true;
    /* 0x503F (servo error code) is a Leadshine servo object; the CL3B steppers do not have it. */
    d->want_vendor_error = dev->vendor == 0x4321 && dev->kind == ECAT_KIND_SERVO;
    ctx->slavelist[slave].PO2SOconfig = drive_po2so;
    return ESP_OK;
}

const ecat_drive_info_t *ecat_drive_info(uint16_t slave)
{
    const drive_t *d = find(slave);
    return d ? &d->info : NULL;
}

bool ecat_drive_layout_ok(const ecx_contextt *ctx, uint16_t slave)
{
    const drive_t *d = find(slave);
    if (d == NULL || !d->info.remap_ok) {
        return false;
    }
    const ec_slavet *sl = &ctx->slavelist[slave];
    const uint16_t in_bits = d->info.has_vendor_error ? ECAT_DRIVE_IN_BITS_503F : ECAT_DRIVE_IN_BITS;
    if (sl->Obits != ECAT_DRIVE_OUT_BITS || sl->Ibits != in_bits || sl->outputs == NULL || sl->inputs == NULL ||
        sl->Ostartbit != 0 || sl->Istartbit != 0) {
        ESP_LOGE(TAG, "slave %u: mapped %u out / %u in bits, expected %u / %u", slave, sl->Obits, sl->Ibits,
                 ECAT_DRIVE_OUT_BITS, in_bits);
        return false;
    }
    return true;
}

uint32_t ecat_drive_units_per_rev(const ecat_drive_info_t *d)
{
    if (d == NULL) {
        return 0;
    }
    if (d->feed && d->feed_rev) {
        /* units per motor rev = feed per shaft rev x shaft revs per motor rev */
        uint64_t num = (uint64_t)d->feed * (d->gear_motor && d->gear_shaft ? d->gear_shaft : 1);
        uint64_t den = (uint64_t)d->feed_rev * (d->gear_motor && d->gear_shaft ? d->gear_motor : 1);
        uint64_t v = (num + den / 2) / den;
        return v > 0 && v <= UINT32_MAX ? (uint32_t)v : 0;
    }
    if (d->enc_inc && d->enc_rev) {
        return d->enc_inc / d->enc_rev;
    }
    return 0;
}
