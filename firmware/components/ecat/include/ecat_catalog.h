/*
 * Catalog of the EtherCAT devices the firmware knows, generated from ESI files
 * by tools/esi2catalog.py (ecat_catalog_gen.c).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECAT_MODULE_MAX_PDO 6

typedef enum {
    ECAT_KIND_UNKNOWN = 0, /*!< Not in the catalog: kept in the topology, its data is not used */
    ECAT_KIND_SERVO,       /*!< CiA402 servo drive */
    ECAT_KIND_STEPPER,     /*!< CiA402 closed-loop stepper drive */
    ECAT_KIND_IO,          /*!< IO slave with a fixed process image */
    ECAT_KIND_COUPLER,     /*!< Modular IO coupler (Modular Device Profile), e.g. Leadshine R1EC */
} ecat_kind_t;

/** One CoE write the ESI asks for in the PRE-OP -> SAFE-OP transition. */
typedef struct {
    uint16_t index;    /*!< Object index for slot 0; add slot * slot_index_inc */
    uint8_t subindex;
    uint8_t size;      /*!< Bytes, 1..4 */
    uint32_t value;    /*!< Little-endian value of the ESI <Data> */
} ecat_init_cmd_t;

typedef struct {
    uint16_t addr;     /*!< ESC start address */
    uint8_t control;   /*!< ESC control byte */
} ecat_sm_info_t;

typedef struct {
    uint32_t vendor;
    uint32_t product;
    const char *name;
    ecat_kind_t kind;
    uint16_t alias_index;    /*!< Object holding the station alias (DINT), 0 if none; unused since
                              *   slaves are identified by cable position */
    /* Modular couplers only */
    uint16_t slot_index_inc; /*!< Object index distance between slots */
    uint16_t slot_pdo_inc;   /*!< PDO index distance between slots */
    uint8_t max_modules;     /*!< Entries of the 0xF030/0xF050 module lists */
    uint16_t status_pdo;     /*!< Fixed TxPDO of the coupler itself, 0 if none */
    uint16_t status_bits;
    ecat_sm_info_t sm_out;   /*!< SM2, process data outputs */
    ecat_sm_info_t sm_in;    /*!< SM3, process data inputs */
    const char *source;      /*!< ESI file the record comes from */
} ecat_device_info_t;

typedef struct {
    uint32_t coupler_product; /*!< Coupler whose ESI defines the module */
    uint32_t ident;
    const char *name;
    uint8_t di;               /*!< Digital input channels (BOOL entries) */
    uint8_t dout;             /*!< Digital output channels */
    uint16_t in_bits;         /*!< Input process data with the default PDOs */
    uint16_t out_bits;
    uint8_t n_rx;
    uint8_t n_tx;
    uint16_t rx_pdo[ECAT_MODULE_MAX_PDO]; /*!< Default RxPDOs for slot 0 */
    uint16_t tx_pdo[ECAT_MODULE_MAX_PDO]; /*!< Default TxPDOs for slot 0 */
    uint8_t n_init;
    const ecat_init_cmd_t *init;
} ecat_module_info_t;

extern const ecat_device_info_t ecat_catalog_devices[];
extern const size_t ecat_catalog_device_count;
extern const ecat_module_info_t ecat_catalog_modules[];
extern const size_t ecat_catalog_module_count;

/* Longest AL state-change timeouts of the catalog devices (ESI <StateMachine><Timeout>). */
extern const uint32_t ecat_catalog_preop_timeout_ms;     /*!< INIT -> PRE-OP */
extern const uint32_t ecat_catalog_safeop_op_timeout_ms; /*!< PRE-OP -> SAFE-OP and SAFE-OP -> OP */

/** @return the catalog record, or NULL if the device is unknown */
const ecat_device_info_t *ecat_catalog_device(uint32_t vendor, uint32_t product);

/** @return the module record for a coupler, or NULL if unknown */
const ecat_module_info_t *ecat_catalog_module(uint32_t coupler_product, uint32_t ident);

/** @return true for servo and stepper drives (axes AX1.. in cable order) */
static inline int ecat_kind_is_drive(ecat_kind_t kind)
{
    return kind == ECAT_KIND_SERVO || kind == ECAT_KIND_STEPPER;
}

const char *ecat_kind_name(ecat_kind_t kind);

#ifdef __cplusplus
}
#endif
