/*
 * Topology view for the HMI (Settings > EtherCAT): the saved topology, the last scan, its issues
 * and the comparison, published by the ecat task whenever they change (never in the cyclic
 * loop). Seqlock: the writer is the ecat task, readers run on core 0.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ecat_topology.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ECAT_BUS_PHASE_STARTING, /*!< Waiting for the link, scanning or configuring */
    ECAT_BUS_PHASE_HELD,     /*!< PRE-OP, waiting for Scan / Accept / Re-check */
    ECAT_BUS_PHASE_OP,       /*!< Cyclic process data */
} ecat_bus_phase_t;

#define ECAT_BUSVIEW_MSG_LEN 96

typedef struct {
    uint32_t gen;      /*!< +1 on every publish */
    uint32_t scan_gen; /*!< +1 after every bus scan */
    uint8_t phase;     /*!< ecat_bus_phase_t */
    bool have_saved;
    bool cmp_valid;
    uint16_t n_blocked;
    ecat_topo_meta_t meta;
    ecat_topology_t saved;
    ecat_topology_t bus;
    ecat_issues_t issues;
    ecat_compare_t cmp;
    char message[ECAT_BUSVIEW_MSG_LEN]; /*!< Outcome of the last request, "" if none */
} ecat_busview_t;

/** What the writer publishes; NULL pointers publish zeroed parts. */
typedef struct {
    uint8_t phase;
    bool have_saved;
    const ecat_topo_meta_t *meta;
    const ecat_topology_t *saved;
    const ecat_topology_t *bus;
    const ecat_issues_t *issues;
    bool cmp_valid;
    const ecat_compare_t *cmp;
    uint16_t n_blocked;
    bool new_scan;     /*!< `bus` comes from a scan made since the last publish */
    const char *message;
} ecat_busview_src_t;

/** Writer (ecat task, outside the cyclic loop). */
void ecat_busview_publish(const ecat_busview_src_t *src);

/** @return the generation of the published view (cheap: check before ecat_busview_read()) */
uint32_t ecat_busview_gen(void);

/** Copy the published view (about 3 KB). */
void ecat_busview_read(ecat_busview_t *out);

#ifdef __cplusplus
}
#endif
