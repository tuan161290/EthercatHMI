/*
 * Modular IO coupler configuration in the PRE-OP -> SAFE-OP hook.
 *
 * Steps follow what a configuration tool derives from the ESI <Slots> section, and the
 * lessons of the user's earlier PC project (docs/REFERENCE_PGM.md section 4):
 *   1. 0xF050 (detected modules) must equal the accepted rack.
 *   2. 0xF030 (configured modules) is written only if it differs: the R1EC takes the list
 *      once per power cycle (unverified on hardware, see docs/SLAVES_LEADSHINE.md 4.4).
 *   3. Module InitCmds from the ESI (input filters, output state on link loss).
 *   4. PDO assignment 0x1C12/0x1C13: coupler Device Status first, then the slots in order.
 *   5. The SM2/SM3 sizes come from the ESI (soem_esp_set_pdo_layout()), because the
 *      coupler does not serve its PDO mapping objects.
 */

#include <inttypes.h>
#include <string.h>
#include "ecat_coupler.h"
#include "ecat_sdo.h"
#include "esp_log.h"
#include "soem_esp.h"

static const char *TAG = "ecat_cpl";

#define OBJ_F030_CONFIGURED 0xF030
#define OBJ_F050_DETECTED   0xF050
#define OBJ_F000_MDP        0xF000
#define OBJ_RX_ASSIGN       0x1C12
#define OBJ_TX_ASSIGN       0x1C13

static ecat_coupler_t s_cpl[ECAT_TOPO_MAX_RACKS];

static ecat_coupler_t *find(uint16_t slave)
{
    for (int i = 0; i < ECAT_TOPO_MAX_RACKS; i++) {
        if (s_cpl[i].slave == slave && slave != 0) {
            return &s_cpl[i];
        }
    }
    return NULL;
}

/* Pop SOEM's error list (SDO abort codes). Logged when a step failed, so the log names the
 * cause; dropped otherwise, because some refusals are expected (see f030_write()). */
static void flush_sdo_errors(ecx_contextt *ctx, bool log)
{
    ec_errort e;
    while (ecx_poperror(ctx, &e)) {
        if (!log) {
            continue;
        }
        if (e.Etype == EC_ERR_TYPE_SDO_ERROR) {
            ESP_LOGW(TAG, "slave %u: SDO 0x%04X:%u abort 0x%08" PRIX32 " %s", e.Slave, e.Index, e.SubIdx,
                     (uint32_t)e.AbortCode, ec_sdoerror2string((uint32)e.AbortCode));
        } else {
            ESP_LOGW(TAG, "slave %u: 0x%04X:%u error type %d code 0x%08" PRIX32, e.Slave, e.Index, e.SubIdx,
                     (int)e.Etype, (uint32_t)e.AbortCode);
        }
    }
}

/* The R1EC allows its mailbox 10 s (ESI RequestTimeout/ResponseTimeout, R1EC-v1.4.0.0.xml:31-35),
 * SOEM's default is 0.7 s (EC_TIMEOUTRXM). Only the rack reads wait longer. */
#define RACK_SDO_TIMEOUT_US 3000000

/* One entry of the module list. A failure is logged with the slave's recorded state and SOEM's
 * error list (SDO abort code, mailbox error), so the console tells why. */
static bool rack_read(ecx_contextt *ctx, uint16_t slave, uint16_t index, uint8_t sub, uint32_t *v)
{
    int len = sizeof(*v);
    *v = 0;
    if (ecx_SDOread(ctx, slave, index, sub, FALSE, &len, v, RACK_SDO_TIMEOUT_US) > 0) {
        return true;
    }
    ESP_LOGW(TAG, "slave %u: SDO read 0x%04X:%u failed (recorded state 0x%02X)", slave, index, sub,
             ctx->slavelist[slave].state);
    flush_sdo_errors(ctx, true);
    return false;
}

void ecat_coupler_read_rack(ecx_contextt *ctx, uint16_t slave, const ecat_device_info_t *dev, ecat_rack_t *rack,
                            ecat_issues_t *issues, uint16_t topo_index)
{
    uint32_t n = 0;

    memset(rack, 0, sizeof(*rack));
    flush_sdo_errors(ctx, false); /* a failure below logs only its own errors */
    if (!rack_read(ctx, slave, OBJ_F050_DETECTED, 0, &n)) {
        ecat_issues_add(issues, ECAT_ISSUE_RACK_UNREADABLE, topo_index, 0);
        return;
    }
    uint32_t max = dev->max_modules < ECAT_TOPO_MAX_MODULES ? dev->max_modules : ECAT_TOPO_MAX_MODULES;
    if (n > max) {
        ecat_issues_add(issues, ECAT_ISSUE_TOO_MANY, topo_index, n);
        n = max;
    }
    for (uint8_t m = 0; m < n; m++) {
        uint32_t ident = 0;
        if (!rack_read(ctx, slave, OBJ_F050_DETECTED, m + 1, &ident)) {
            ecat_issues_add(issues, ECAT_ISSUE_RACK_UNREADABLE, topo_index, m + 1);
            return;
        }
        rack->ident[m] = ident;
        rack->n_modules = m + 1;
    }

    /* 0xF000:1 = index distance between slots. Optional object: only a readable, different
     * value is a problem. */
    uint32_t distance = 0;
    if (dev->slot_index_inc && ecat_sdo_read_uint(ctx, slave, OBJ_F000_MDP, 1, &distance) &&
        distance != dev->slot_index_inc) {
        ecat_issues_add(issues, ECAT_ISSUE_RACK_GEOMETRY, topo_index, distance);
    }
}

void ecat_coupler_reset(void)
{
    memset(s_cpl, 0, sizeof(s_cpl));
    soem_esp_clear_pdo_layouts();
}

/*
 * 0xF030:0 is read-only in the ESI; only the 16 entries are writable (R1EC-v1.4.0.0.xml:511-545).
 * Whether the coupler keeps a count in sub 0 or ends the list at the first 0 entry is not known
 * (unverified on hardware), so the count is neither required nor trusted: entries 1..n must
 * hold the rack and, if the list has room, entry n+1 must be 0 or absent.
 */
static bool f030_matches(ecx_contextt *ctx, const ecat_coupler_t *c)
{
    const ecat_rack_t *rack = &c->rack;
    for (uint8_t m = 0; m < rack->n_modules; m++) {
        uint32_t ident = 0;
        if (!ecat_sdo_read_uint(ctx, c->slave, OBJ_F030_CONFIGURED, m + 1, &ident) || ident != rack->ident[m]) {
            return false;
        }
    }
    uint32_t next = 0;
    if (rack->n_modules < c->dev->max_modules &&
        ecat_sdo_read_uint(ctx, c->slave, OBJ_F030_CONFIGURED, rack->n_modules + 1, &next) && next != 0) {
        return false; /* a longer list from an earlier configuration */
    }
    return true;
}

/* @return false if an entry could not be written. Sub 0 is written as CiA 301 asks (0 first,
 * the count last) but a refusal is expected from the ESI and ignored. */
static bool f030_write(ecx_contextt *ctx, const ecat_coupler_t *c)
{
    const ecat_rack_t *rack = &c->rack;
    (void)ecat_sdo_write_u8(ctx, c->slave, OBJ_F030_CONFIGURED, 0, 0);
    bool ok = true;
    for (uint8_t m = 0; m < rack->n_modules; m++) {
        ok &= ecat_sdo_write_u32(ctx, c->slave, OBJ_F030_CONFIGURED, m + 1, rack->ident[m]);
    }
    if (rack->n_modules < c->dev->max_modules) {
        ok &= ecat_sdo_write_u32(ctx, c->slave, OBJ_F030_CONFIGURED, rack->n_modules + 1, 0);
    }
    (void)ecat_sdo_write_u8(ctx, c->slave, OBJ_F030_CONFIGURED, 0, rack->n_modules);
    return ok;
}

/* Write an assignment object: sub0 = 0, the entries, then sub0 = count. */
static bool write_assign(ecx_contextt *ctx, uint16_t slave, uint16_t obj, const uint16_t *pdo, uint8_t n)
{
    bool ok = ecat_sdo_write_u8(ctx, slave, obj, 0, 0);
    for (uint8_t k = 0; k < n; k++) {
        ok &= ecat_sdo_write_u16(ctx, slave, obj, k + 1, pdo[k]);
    }
    if (n) {
        ok &= ecat_sdo_write_u8(ctx, slave, obj, 0, n);
    }
    return ok;
}

static int coupler_po2so(ecx_contextt *ctx, uint16 slave)
{
    ecat_coupler_t *c = find(slave);
    if (c == NULL) {
        return 0;
    }
    const ecat_device_info_t *dev = c->dev;
    c->hook_ran = true;
    c->rack_ok = c->f030_ok = c->f030_written = c->assign_ok = false;
    flush_sdo_errors(ctx, false); /* report only what this hook causes */

    /* 1. The modules really plugged in must be the accepted ones. */
    ecat_rack_t detected;
    ecat_issues_t issues = { 0 };
    ecat_coupler_read_rack(ctx, slave, dev, &detected, &issues, 0);
    c->rack_ok = issues.n_blocking == 0 && detected.n_modules == c->rack.n_modules &&
                 memcmp(detected.ident, c->rack.ident, detected.n_modules * sizeof(detected.ident[0])) == 0;
    if (!c->rack_ok) {
        ESP_LOGE(TAG, "slave %u: detected modules differ from the accepted rack, not configured", slave);
        flush_sdo_errors(ctx, true);
        return 0;
    }

    /* 2. Configured module list. */
    bool f030_write_ok = true;
    c->f030_ok = f030_matches(ctx, c);
    if (!c->f030_ok) {
        f030_write_ok = f030_write(ctx, c);
        c->f030_written = true;
        c->f030_ok = f030_matches(ctx, c);
    }
    uint32_t f030_sub0 = 0xFF; /* logged only: tells on day 1 whether the coupler keeps a count */
    (void)ecat_sdo_read_uint(ctx, slave, OBJ_F030_CONFIGURED, 0, &f030_sub0);

    /* 3 + 4. InitCmds and PDO assignment, slot by slot. */
    uint16_t rx[ECAT_TOPO_MAX_MODULES * ECAT_MODULE_MAX_PDO];
    uint16_t tx[ECAT_TOPO_MAX_MODULES * ECAT_MODULE_MAX_PDO + 1];
    uint8_t n_rx = 0, n_tx = 0;
    uint16_t in_bit = dev->status_bits, out_bit = 0;

    if (dev->status_pdo) {
        tx[n_tx++] = dev->status_pdo;
    }
    c->init_errors = 0;
    for (uint8_t m = 0; m < c->rack.n_modules; m++) {
        const ecat_module_info_t *mi = ecat_catalog_module(dev->product, c->rack.ident[m]);
        if (mi == NULL) {
            /* Cannot happen for an accepted rack: unknown modules block acceptance. */
            ESP_LOGE(TAG, "slave %u slot %u: module 0x%08" PRIX32 " not in the catalog", slave, m, c->rack.ident[m]);
            c->rack_ok = false;
            return 0;
        }
        for (uint8_t k = 0; k < mi->n_init; k++) {
            const ecat_init_cmd_t *ic = &mi->init[k];
            uint16_t idx = ic->index + m * dev->slot_index_inc;
            if (!ecat_sdo_write(ctx, slave, idx, ic->subindex, &ic->value, ic->size)) {
                c->init_errors++;
            }
        }
        for (uint8_t k = 0; k < mi->n_rx && n_rx < sizeof(rx) / sizeof(rx[0]); k++) {
            rx[n_rx++] = mi->rx_pdo[k] + m * dev->slot_pdo_inc;
        }
        for (uint8_t k = 0; k < mi->n_tx && n_tx < sizeof(tx) / sizeof(tx[0]); k++) {
            tx[n_tx++] = mi->tx_pdo[k] + m * dev->slot_pdo_inc;
        }
        c->slot_in_bit[m] = in_bit;
        c->slot_out_bit[m] = out_bit;
        in_bit += mi->in_bits;
        out_bit += mi->out_bits;
    }
    c->assign_ok = write_assign(ctx, slave, OBJ_RX_ASSIGN, rx, n_rx) & write_assign(ctx, slave, OBJ_TX_ASSIGN, tx, n_tx);
    c->in_bits = in_bit;
    c->out_bits = out_bit;

    /* 5. Sizes from the ESI. */
    soem_esp_pdo_layout_t layout = {
        .out_bits = out_bit,
        .in_bits = in_bit,
        .sm2_addr = dev->sm_out.addr,
        .sm2_control = dev->sm_out.control,
        .sm3_addr = dev->sm_in.addr,
        .sm3_control = dev->sm_in.control,
    };
    soem_esp_set_pdo_layout(slave, &layout);

    bool problem = !c->f030_ok || !f030_write_ok || c->init_errors || !c->assign_ok;
    ESP_LOGI(TAG, "slave %u %s: %u module(s), 0xF030 %s (sub 0 = %" PRIu32 "), InitCmd errors %u, PDO assign %s, "
             "in %u bits, out %u bits",
             slave, dev->name, c->rack.n_modules,
             c->f030_ok ? (c->f030_written ? "written" : "already set")
                        : (f030_write_ok ? "NOT TAKEN (power-cycle the coupler)" : "WRITE REFUSED"),
             f030_sub0, c->init_errors, c->assign_ok ? "ok" : "FAILED", in_bit, out_bit);
    flush_sdo_errors(ctx, problem);
    return 1;
}

esp_err_t ecat_coupler_prepare(ecx_contextt *ctx, uint16_t slave, const ecat_device_info_t *dev, const ecat_rack_t *rack)
{
    if (dev == NULL || rack == NULL || dev->kind != ECAT_KIND_COUPLER || slave == 0 || slave > ctx->slavecount) {
        return ESP_ERR_INVALID_ARG;
    }
    ecat_coupler_t *c = find(slave);
    for (int i = 0; c == NULL && i < ECAT_TOPO_MAX_RACKS; i++) {
        if (s_cpl[i].slave == 0) {
            c = &s_cpl[i];
        }
    }
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(c, 0, sizeof(*c));
    c->slave = slave;
    c->dev = dev;
    c->rack = *rack;
    ctx->slavelist[slave].PO2SOconfig = coupler_po2so;
    return ESP_OK;
}

const ecat_coupler_t *ecat_coupler_get(uint16_t slave)
{
    return find(slave);
}

int ecat_coupler_list(uint16_t *slaves, int max)
{
    int n = 0;
    for (int i = 0; i < ECAT_TOPO_MAX_RACKS && n < max; i++) {
        if (s_cpl[i].slave != 0) {
            slaves[n++] = s_cpl[i].slave;
        }
    }
    return n;
}

bool ecat_coupler_ready(uint16_t slave)
{
    const ecat_coupler_t *c = find(slave);
    return c != NULL && c->hook_ran && c->rack_ok && c->f030_ok && c->assign_ok;
}

void ecat_coupler_invalidate(uint16_t slave)
{
    ecat_coupler_t *c = find(slave);
    if (c != NULL) {
        c->hook_ran = false;
        c->rack_ok = c->f030_ok = c->assign_ok = false;
    }
}

bool ecat_coupler_check_sm(ecx_contextt *ctx, uint16_t slave)
{
    const ecat_coupler_t *c = find(slave);
    if (c == NULL) {
        return false;
    }
    bool ok = true;
    const struct {
        int sm;
        uint16_t bits;
    } want[] = { { 2, c->out_bits }, { 3, c->in_bits } };

    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        ec_smt reg;
        memset(&reg, 0, sizeof(reg));
        int wkc = ecx_FPRD(&ctx->port, ctx->slavelist[slave].configadr, (uint16)(ECT_REG_SM0 + want[i].sm * sizeof(ec_smt)),
                           sizeof(reg), &reg, EC_TIMEOUTRET3);
        uint16_t len = etohs(reg.SMlength);
        uint16_t expect = (uint16_t)((want[i].bits + 7) / 8);
        if (wkc <= 0 || len != expect) {
            ESP_LOGE(TAG, "slave %u: ESC SM%d length %u, expected %u (wkc %d)", slave, want[i].sm, len, expect, wkc);
            ok = false;
        }
    }
    return ok;
}

uint32_t ecat_coupler_device_status(const ecx_contextt *ctx, uint16_t slave)
{
    const ecat_coupler_t *c = find(slave);
    const ec_slavet *sl = &ctx->slavelist[slave];
    if (c == NULL || c->dev->status_bits != 32 || sl->inputs == NULL || sl->Ibytes < 4) {
        return 0;
    }
    return (uint32_t)sl->inputs[0] | ((uint32_t)sl->inputs[1] << 8) | ((uint32_t)sl->inputs[2] << 16) |
           ((uint32_t)sl->inputs[3] << 24);
}
