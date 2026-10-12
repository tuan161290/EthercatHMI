/*
 * CiA402 drives (Leadshine L5N/L3N servo, CL3B-EC stepper): process data layout and the
 * PRE-OP -> SAFE-OP hook that maps it.
 *
 * The default PDOs carry only 6040/607A/60B8 (docs/SLAVES_LEADSHINE.md section 2), so the hook
 * remaps 0x1600 and 0x1A00 to one layout for every drive. Profile position (PP), profile velocity
 * (PV) and homing (HM) then run from the process data, with no SDO in OP (section 6). The ESI
 * allows every object below in the PDOs (PdoMapping flags) and 10 (L5N/L3N, DT1600/DT1A00) or
 * 8 (CL3B) entries per PDO.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ecat_catalog.h"
#include "esp_err.h"
#include "soem/soem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** RxPDO 0x1600 after the remap: 7 entries, 184 bits. */
typedef struct __attribute__((packed)) {
    uint16_t controlword;      /*!< 0x6040 */
    int32_t target_position;   /*!< 0x607A, PP */
    int32_t target_velocity;   /*!< 0x60FF, PV */
    uint32_t profile_velocity; /*!< 0x6081, PP */
    uint32_t profile_accel;    /*!< 0x6083, PP and PV */
    uint32_t profile_decel;    /*!< 0x6084, PP and PV */
    int8_t mode;               /*!< 0x6060 */
} ecat_drive_out_t;

/** TxPDO 0x1A00 after the remap: 6 entries (136 bits), 7 with 0x503F (152 bits). */
typedef struct __attribute__((packed)) {
    uint16_t statusword;     /*!< 0x6041 */
    uint16_t error_code;     /*!< 0x603F */
    int8_t mode_display;     /*!< 0x6061 */
    int32_t position;        /*!< 0x6064 */
    int32_t velocity;        /*!< 0x606C */
    uint32_t digital_inputs; /*!< 0x60FD */
    uint16_t vendor_error;   /*!< 0x503F, Leadshine servo only (L5N:6809); absent on CL3B */
} ecat_drive_in_t;

#define ECAT_DRIVE_OUT_BITS     184
#define ECAT_DRIVE_IN_BITS      136
#define ECAT_DRIVE_IN_BITS_503F 152

typedef struct {
    bool hook_ran;
    bool remap_ok;          /*!< 0x1C12/0x1600/0x1C13/0x1A00 written without an SDO error */
    bool has_vendor_error;  /*!< 0x503F is in the TxPDO */
    /* Read once in the hook; both parts 0 = not readable */
    uint32_t enc_inc;       /*!< 0x608F:1 encoder increments */
    uint32_t enc_rev;       /*!< 0x608F:2 motor revolutions */
    uint32_t gear_motor;    /*!< 0x6091:1 motor revolutions (not on CL3B) */
    uint32_t gear_shaft;    /*!< 0x6091:2 shaft revolutions */
    uint32_t feed;          /*!< 0x6092:1 feed (position units) */
    uint32_t feed_rev;      /*!< 0x6092:2 shaft revolutions */
    int8_t home_method;     /*!< 0x6098 */
    bool home_method_valid;
} ecat_drive_info_t;

/** Forget every prepared drive. Call before a new configuration. */
void ecat_drive_reset(void);

/**
 * Register the PO2SOconfig hook that remaps the drive's PDOs.
 * Call after ecx_config_init() and before ecx_config_map_group().
 */
esp_err_t ecat_drive_prepare(ecx_contextt *ctx, uint16_t slave, const ecat_device_info_t *dev);

/** @return what the hook found, or NULL if `slave` was not prepared */
const ecat_drive_info_t *ecat_drive_info(uint16_t slave);

/** @return true if SOEM mapped exactly the remapped layout (call after ecx_config_map_group()) */
bool ecat_drive_layout_ok(const ecx_contextt *ctx, uint16_t slave);

/**
 * Position units (0x6064) per motor revolution from 0x608F, 0x6091 and 0x6092
 * (CiA402 factor group): feed / gear, or the encoder resolution when the drive has no feed
 * constant. @return 0 if unknown (unverified on hardware for the Leadshine drives)
 */
uint32_t ecat_drive_units_per_rev(const ecat_drive_info_t *d);

#ifdef __cplusplus
}
#endif
