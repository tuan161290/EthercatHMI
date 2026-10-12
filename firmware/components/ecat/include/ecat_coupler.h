/*
 * Modular IO couplers (Modular Device Profile), e.g. Leadshine R1EC.
 *
 * The coupler does not describe its modules' process data to the master: the master must
 * download the module list (0xF030) and the PDO assignment, and size SM2/SM3 itself from
 * the ESI (docs/SLAVES_LEADSHINE.md section 4.4). This module does that in the slave's
 * PRE-OP -> SAFE-OP hook and records the byte layout of every slot.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ecat_catalog.h"
#include "ecat_topology.h"
#include "esp_err.h"
#include "soem/soem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t slave;                    /*!< SOEM slave number, 0 = unused entry */
    const ecat_device_info_t *dev;
    ecat_rack_t rack;                  /*!< Expected modules (from the accepted topology) */
    bool hook_ran;
    bool rack_ok;                      /*!< 0xF050 equals the expected modules */
    bool f030_written;                 /*!< 0xF030 differed and was written */
    bool f030_ok;                      /*!< 0xF030 holds the expected list after the hook */
    bool assign_ok;                    /*!< 0x1C12/0x1C13 written without error */
    uint16_t init_errors;              /*!< Module InitCmds that failed */
    uint16_t in_bits;                  /*!< Whole SM3 image, Device Status included */
    uint16_t out_bits;                 /*!< Whole SM2 image */
    uint16_t slot_in_bit[ECAT_TOPO_MAX_MODULES];  /*!< Bit offset of each slot in the slave's inputs */
    uint16_t slot_out_bit[ECAT_TOPO_MAX_MODULES]; /*!< Bit offset of each slot in the slave's outputs */
} ecat_coupler_t;

/**
 * Read the modules of a coupler: 0xF050 (detected list) and 0xF000:1 (slot distance).
 * Problems are appended to `issues` for topology slave index `topo_index`.
 */
void ecat_coupler_read_rack(ecx_contextt *ctx, uint16_t slave, const ecat_device_info_t *dev, ecat_rack_t *rack,
                            ecat_issues_t *issues, uint16_t topo_index);

/** Forget every prepared coupler and every PDO layout. Call before a new configuration. */
void ecat_coupler_reset(void);

/**
 * Register the PO2SOconfig hook of a coupler with the modules it must have.
 * Call after ecx_config_init() and before ecx_config_map_group().
 */
esp_err_t ecat_coupler_prepare(ecx_contextt *ctx, uint16_t slave, const ecat_device_info_t *dev,
                               const ecat_rack_t *rack);

/** @return the coupler state, or NULL if `slave` was not prepared */
const ecat_coupler_t *ecat_coupler_get(uint16_t slave);

/** @return number of prepared couplers; `slaves` receives their slave numbers */
int ecat_coupler_list(uint16_t *slaves, int max);

/** @return true if the hook ran, the coupler holds the expected modules and module list,
 *  and the PDO assignment was written */
bool ecat_coupler_ready(uint16_t slave);

/**
 * Mark a coupler as not configured, so ecat_coupler_ready() is false until its hook runs
 * again. Call before SOEM reconfigures or recovers the slave.
 */
void ecat_coupler_invalidate(uint16_t slave);

/**
 * Compare the SM2/SM3 lengths in the ESC with the layout (some couplers resize them
 * after 0xF030 is written). Call once the slave is in SAFE-OP.
 */
bool ecat_coupler_check_sm(ecx_contextt *ctx, uint16_t slave);

/** @return the coupler's Device Status word (0 = no fault), read from the process image */
uint32_t ecat_coupler_device_status(const ecx_contextt *ctx, uint16_t slave);

#ifdef __cplusplus
}
#endif
