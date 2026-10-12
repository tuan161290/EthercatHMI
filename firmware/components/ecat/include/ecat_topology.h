/*
 * EtherCAT topology: Scan -> Accept -> Save (docs/ETHERCAT_PLAN.md section 3.6).
 *
 * The bus is scanned into an ecat_topology_t. An ENGINEER accepts a scan, which is then
 * saved as the expected topology. At every start the bus is compared with the saved
 * topology and the master only goes to OP when nothing blocking differs.
 * Slaves are identified by cable position (user's decision, 2026-10-10): the n-th drive along the
 * chain is AXn, whatever station alias it has. Two drives of the same model swapped in the chain
 * cannot be told apart; a different model, a missing or an extra slave can.
 *
 * Everything in this header except ecat_topology_scan() is plain data processing with
 * no SOEM or hardware access, so it can be checked with ecat_topology_selftest().
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ecat_catalog.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ECAT_TOPO_MAX_SLAVES  32
#define ECAT_TOPO_MAX_RACKS   4
#define ECAT_TOPO_MAX_MODULES 32
#define ECAT_TOPO_NAME_LEN    8
#define ECAT_TOPO_MAX_ISSUES  16
#define ECAT_TOPO_NO_RACK     0xFF
#define ECAT_TOPO_NONE        0xFFFF

/** Modules plugged into a modular coupler, slot 0 first. */
typedef struct {
    uint8_t n_modules;
    uint32_t ident[ECAT_TOPO_MAX_MODULES];
} ecat_rack_t;

typedef struct {
    uint16_t position;       /*!< 1-based cable position (SOEM slave number) at scan time */
    uint16_t alias;          /*!< Station alias from register 0x0012, shown only (not used to match) */
    uint32_t vendor;
    uint32_t product;
    uint32_t revision;
    uint8_t kind;            /*!< ecat_kind_t */
    uint8_t rack;            /*!< Index into rack[], ECAT_TOPO_NO_RACK if not a modular coupler */
    uint8_t unused;          /*!< Was the alias source; kept so saved topologies keep their layout */
    uint8_t reserved;
    char name[ECAT_TOPO_NAME_LEN]; /*!< "AX3", "RIO1", "DEV9" */
} ecat_topo_slave_t;

typedef struct {
    uint16_t n_slaves;
    uint8_t n_racks;
    ecat_topo_slave_t slave[ECAT_TOPO_MAX_SLAVES];
    ecat_rack_t rack[ECAT_TOPO_MAX_RACKS];
} ecat_topology_t;

/* ---- Problems found in a scan ---- */

typedef enum {
    ECAT_ISSUE_UNKNOWN_MODULE,    /*!< Coupler module not in the catalog (blocking) */
    ECAT_ISSUE_RACK_UNREADABLE,   /*!< 0xF050 could not be read (blocking) */
    ECAT_ISSUE_RACK_GEOMETRY,     /*!< 0xF000 slot distance differs from the ESI (blocking) */
    ECAT_ISSUE_TOO_MANY,          /*!< More slaves, racks or modules than the topology holds (blocking) */
    ECAT_ISSUE_UNKNOWN_DEVICE,    /*!< Slave not in the catalog: kept, data not used (warning) */
} ecat_issue_code_t;

typedef struct {
    uint8_t code;     /*!< ecat_issue_code_t */
    uint16_t slave;   /*!< Index into ecat_topology_t.slave[] */
    uint32_t detail;  /*!< Module ident or count, depending on the code */
} ecat_issue_t;

typedef struct {
    uint8_t n;
    uint8_t n_blocking;
    ecat_issue_t item[ECAT_TOPO_MAX_ISSUES];
} ecat_issues_t;

/* ---- Comparison with the saved topology ---- */

#define ECAT_DIFF_REVISION 0x02 /*!< Different revision: warning, accepted */
#define ECAT_DIFF_MODEL    0x04 /*!< Different vendor/product: blocking */
#define ECAT_DIFF_RACK     0x08 /*!< Different coupler modules: blocking */
#define ECAT_DIFF_MISSING  0x10 /*!< Not on the bus: blocking */
#define ECAT_DIFF_BLOCKING (ECAT_DIFF_MODEL | ECAT_DIFF_RACK | ECAT_DIFF_MISSING)

typedef struct {
    uint16_t bus[ECAT_TOPO_MAX_SLAVES];  /*!< Per saved slave: bus slave at the same position, or ECAT_TOPO_NONE */
    uint8_t diff[ECAT_TOPO_MAX_SLAVES];  /*!< Per saved slave: ECAT_DIFF_* bits */
    uint16_t n_extra;
    uint16_t extra[ECAT_TOPO_MAX_SLAVES]; /*!< Bus slaves that match no saved slave (blocking) */
    uint16_t n_blocking;
    uint16_t n_warnings;
    bool ok;                              /*!< True if the master may go to OP */
} ecat_compare_t;

/** Who saved the topology and when. */
typedef struct {
    int64_t saved_at;  /*!< Unix time, 0 if the clock was not set */
    char saved_by[12];
} ecat_topo_meta_t;

/* ---- Pure functions ---- */

/** Add an issue (no-op when the list is full). */
void ecat_issues_add(ecat_issues_t *issues, ecat_issue_code_t code, uint16_t slave, uint32_t detail);
bool ecat_issue_is_blocking(ecat_issue_code_t code);
const char *ecat_issue_text(ecat_issue_code_t code);

/**
 * Check a scan before it may be accepted: unknown devices and modules, coupler racks.
 * Appends to `issues` (which may already hold problems found while scanning).
 */
void ecat_topology_validate(const ecat_topology_t *scan, ecat_issues_t *issues);

/**
 * Match every saved slave with a bus slave and classify the differences.
 * Every saved slave is compared with the bus slave at the same cable position.
 */
void ecat_topology_compare(const ecat_topology_t *saved, const ecat_topology_t *bus, ecat_compare_t *out);

/**
 * Give every scanned slave a name: the saved name of the slave it matches, if any,
 * otherwise "AX<n>" for the n-th drive in cable order, "RIO<n>" for the n-th IO slave and
 * "DEV<position>" for unknown devices.
 * `saved` and `cmp` may be NULL (first acceptance).
 */
void ecat_topology_assign_names(ecat_topology_t *scan, const ecat_topology_t *saved, const ecat_compare_t *cmp);

/** Short text for the most important bit of a diff ("MATCH", "MISSING", ...). */
const char *ecat_diff_text(uint8_t diff);

/** Catalog name of a slave, or "unknown". */
const char *ecat_topology_device_name(const ecat_topo_slave_t *s);

/** Log a topology, the issues of a scan, and a comparison (ESP_LOGI). */
void ecat_topology_log(const char *title, const ecat_topology_t *t);
void ecat_issues_log(const ecat_topology_t *t, const ecat_issues_t *issues);
void ecat_compare_log(const ecat_topology_t *saved, const ecat_topology_t *bus, const ecat_compare_t *cmp);

/** Run the built-in checks of compare/validate/names. @return number of failed checks */
int ecat_topology_selftest(void);

/* ---- Persistent storage (NVS namespace "ecat") ---- */

/** @return ESP_ERR_NOT_FOUND if nothing valid is saved */
esp_err_t ecat_topology_load(ecat_topology_t *out, ecat_topo_meta_t *meta);
esp_err_t ecat_topology_save(const ecat_topology_t *topo, const ecat_topo_meta_t *meta);
esp_err_t ecat_topology_erase(void);

/* ---- Bus access ---- */

struct ecx_context;

/**
 * Build a topology from the slaves found by ecx_config_init() (slaves in PRE-OP).
 * Reads the station alias from register 0x0012 for display only;
 * reads the module list (0xF050) of modular couplers. Then validates the result.
 */
esp_err_t ecat_topology_scan(struct ecx_context *ctx, ecat_topology_t *out, ecat_issues_t *issues);

/**
 * Check again that the slave at expected->position is the accepted model, after SOEM has
 * recovered or reconfigured it in OP: vendor and product are read again from its SII EEPROM.
 * (Two drives of the same model cannot be told apart.) Modular couplers are checked with
 * ecat_coupler_ready() instead.
 */
bool ecat_topology_verify_slave(struct ecx_context *ctx, const ecat_topo_slave_t *expected);

#ifdef __cplusplus
}
#endif
