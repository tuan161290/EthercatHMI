/*
 * Build a topology from the slaves found by ecx_config_init().
 */

#include <inttypes.h>
#include <string.h>
#include "ecat_coupler.h"
#include "ecat_topology.h"
#include "esp_log.h"

static const char *TAG = "ecat_scan";

bool ecat_topology_verify_slave(struct ecx_context *ctx, const ecat_topo_slave_t *expected)
{
    /* SII vendor and product words (SOEM ec_type.h ECT_SII_MANUF/ECT_SII_ID), read from the device
     * now in this position, not the copy SOEM kept from the start-up scan. */
    const uint32_t vendor = ecx_readeeprom(ctx, expected->position, ECT_SII_MANUF, EC_TIMEOUTEEP);
    const uint32_t product = ecx_readeeprom(ctx, expected->position, ECT_SII_ID, EC_TIMEOUTEEP);
    (void)ecx_eeprom2pdi(ctx, expected->position); /* as ecx_config_init() leaves it */
    if (vendor != expected->vendor || product != expected->product) {
        ESP_LOGE(TAG, "slave %u (%s): device is now 0x%08" PRIX32 "/0x%08" PRIX32 ", expected 0x%08" PRIX32
                 "/0x%08" PRIX32, expected->position, expected->name, vendor, product, expected->vendor,
                 expected->product);
        return false;
    }
    return true;
}

esp_err_t ecat_topology_scan(struct ecx_context *ctx, ecat_topology_t *out, ecat_issues_t *issues)
{
    memset(out, 0, sizeof(*out));
    memset(issues, 0, sizeof(*issues));

    /* ecx_config_init() only requests PRE-OP; the mailbox (SDO) works once it is reached.
     * The R1EC allows itself 3 s (its ESI), longer than SOEM's default. */
    int timeout = (int)ecat_catalog_preop_timeout_ms * 1000;
    if (timeout < EC_TIMEOUTSTATE) {
        timeout = EC_TIMEOUTSTATE;
    }
    if (ecx_statecheck(ctx, 0, EC_STATE_PRE_OP, timeout) != EC_STATE_PRE_OP) {
        ESP_LOGW(TAG, "not every slave reached PRE-OP: SDO reads may fail");
    }
    /* ecx_statecheck() of slave 0 records the state in slavelist[0] only, and SOEM sends no
     * mailbox to a slave whose own recorded state is below PRE-OP (ec_main.c ecx_mbxsend()).
     * Without this every coupler read failed at once on the first R1EC scan (2026-10-11). */
    (void)ecx_readstate(ctx);

    int count = ctx->slavecount;
    if (count > ECAT_TOPO_MAX_SLAVES) {
        ecat_issues_add(issues, ECAT_ISSUE_TOO_MANY, 0, (uint32_t)count);
        count = ECAT_TOPO_MAX_SLAVES;
    }

    for (int i = 1; i <= count; i++) {
        const ec_slavet *sl = &ctx->slavelist[i];
        uint16_t idx = out->n_slaves;
        ecat_topo_slave_t *s = &out->slave[idx];

        s->position = (uint16_t)i;
        s->alias = sl->aliasadr;
        s->vendor = sl->eep_man;
        s->product = sl->eep_id;
        s->revision = sl->eep_rev;
        s->rack = ECAT_TOPO_NO_RACK;

        const ecat_device_info_t *dev = ecat_catalog_device(s->vendor, s->product);
        s->kind = dev ? dev->kind : ECAT_KIND_UNKNOWN;

        if (dev != NULL && dev->kind == ECAT_KIND_COUPLER) {
            if (out->n_racks >= ECAT_TOPO_MAX_RACKS) {
                ecat_issues_add(issues, ECAT_ISSUE_TOO_MANY, idx, out->n_racks + 1);
            } else {
                s->rack = out->n_racks++;
                if ((sl->state & 0x0f) < EC_STATE_PRE_OP) {
                    /* A slow coupler: give it its own PRE-OP timeout (this also records its state). */
                    (void)ecx_statecheck(ctx, (uint16_t)i, EC_STATE_PRE_OP, timeout);
                }
                ecat_coupler_read_rack(ctx, (uint16_t)i, dev, &out->rack[s->rack], issues, idx);
            }
        }
        out->n_slaves++;
    }

    ecat_topology_validate(out, issues);
    ESP_LOGI(TAG, "%u slave(s) scanned, %u issue(s), %u blocking", (unsigned)out->n_slaves, (unsigned)issues->n,
             (unsigned)issues->n_blocking);
    return ESP_OK;
}
