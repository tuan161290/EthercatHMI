/*
 * EtherCAT master bring-up application for the JC1060P470C_I_W_Y.
 *
 * Start-up follows the topology rule of docs/ETHERCAT_PLAN.md section 3.6:
 *   scan -> compare with the saved topology -> OP only if nothing blocking differs;
 *   otherwise hold the bus in PRE-OP and wait for console commands (scan / accept / recheck).
 *
 * Core 1: EMAC RX task + "ecat" task (scan, configuration, cyclic process data).
 * Core 0: "ecat_mon" (statistics, HMI status), "ecat_chk" (slave recovery), console REPL, LVGL HMI.
 *
 * s_bus_lock: the ecat task holds it while it scans, configures or changes the bus, and releases
 * it for the cyclic loop and while the PRE-OP hold waits for a command. ecat_chk, ecat_mon and
 * the SDO jobs of the HMI (ecat_sdo_service.h) touch the slave list only while holding it, so
 * they never act on a bus being rescanned.
 *
 * Based on SOEM samples/simple_ng.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_alarms.h"
#include "app_console.h"
#include "board_eth.h"
#include "ecat_axis.h"
#include "ecat_busview.h"
#include "ecat_coupler.h"
#include "ecat_drive.h"
#include "ecat_io.h"
#include "ecat_sdo_service.h"
#include "ecat_topology.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hmi.h"
#include "machine_alarm.h"
#include "machine_clock.h"
#include "machine_params.h"
#include "machine_store.h"
#include "net.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "soem/soem.h"
#include "soem_esp.h"

static const char *TAG = "ecat";

#define ECAT_GROUP          0
#define ECAT_IOMAP_SIZE     4096
#define EMAC_RX_TASK_PRIO   (CONFIG_SOEM_RT_TASK_PRIO + 1) /* must preempt the cycle task on core 1 */
#define ECAT_TASK_STACK     16384  /* SOEM CoE/mailbox code keeps ~1.5 KB buffers on the stack */
#define ECAT_CHECK_STACK    8192
#define LEAVE_OP_TIMEOUT_US 2000000 /* axes stop and switch off before the bus leaves OP */

typedef struct {
    uint32_t cycles;
    uint32_t wkc_errors;
    uint32_t overruns;          /* cycles that started more than one period late */
    int32_t expected_wkc;
    int32_t last_wkc;
    uint32_t rtt_min_us;
    uint32_t rtt_max_us;
    uint64_t rtt_sum_us;
    uint32_t wake_late_max_us;  /* worst wake-up delay after the deadline */
} cycle_stats_t;

/* In PSRAM (54 + 4 KB of internal RAM freed, 2026-10-31): the machine runs Profile Position only,
 * so a cycle may be late by the cache misses this adds. Both cores see the same data: the P4 has
 * an ICache per core but one L1 DCache (IDF hal/esp32p4/include/hal/cache_ll.h). No DMA reads
 * these buffers: the EMAC driver copies each frame into its own descriptors' buffers (IDF
 * esp_eth/src/mac/esp_eth_mac_esp_dma.c, emac_esp_dma_transmit_frame). */
static EXT_RAM_BSS_ATTR ecx_contextt s_ctx;
static EXT_RAM_BSS_ATTR uint8_t s_iomap[ECAT_IOMAP_SIZE] __attribute__((aligned(4)));
static volatile bool s_in_op;
static volatile bool s_holding;   /* in hold_loop(): bus held in PRE-OP, waiting for commands */
static volatile bool s_need_check;
static volatile bool s_leave_op;
static cycle_stats_t s_stats;
static QueueHandle_t s_cmd_q;
static SemaphoreHandle_t s_bus_lock;

/* Recovery in OP (ecat_chk): slaves SOEM reconfigured or recovered must pass the identity check
 * again before OP; slaves that fail it stay in PRE-OP until 'stop' + 'recheck'. */
static bool s_reverify[EC_MAXSLAVE];
static bool s_blocked[EC_MAXSLAVE];
static volatile uint16_t s_n_blocked;

/* Topology state. Written only by the ecat task while the bus is not in OP. */
static ecat_topology_t s_saved;
static ecat_topo_meta_t s_saved_meta;
static bool s_have_saved;
static ecat_topology_t s_bus;
static ecat_issues_t s_bus_issues;
static ecat_compare_t s_cmp;
static bool s_cmp_valid;

/* HMI view of the topology (Settings > EtherCAT) and the outcome of the last request. */
static char s_view_msg[ECAT_BUSVIEW_MSG_LEN];
static volatile app_cmd_t s_after_stop = (app_cmd_t)-1; /* HMI asked to scan/recheck while in OP */
static const char *volatile s_accept_by = "console";    /* recorded with a saved topology */

static void publish_view(ecat_bus_phase_t phase, bool new_scan)
{
    const ecat_busview_src_t src = {
        .phase = (uint8_t)phase,
        .have_saved = s_have_saved,
        .meta = &s_saved_meta,
        .saved = &s_saved,
        .bus = &s_bus,
        .issues = &s_bus_issues,
        .cmp_valid = s_cmp_valid,
        .cmp = &s_cmp,
        .n_blocked = s_n_blocked,
        .new_scan = new_scan,
        .message = s_view_msg,
    };
    ecat_busview_publish(&src);
}

static void view_message(const char *msg)
{
    strlcpy(s_view_msg, msg, sizeof(s_view_msg));
}

static const char *state_name(uint16_t state)
{
    switch (state & 0x0f) {
    case EC_STATE_INIT:        return "INIT";
    case EC_STATE_PRE_OP:      return "PRE_OP";
    case EC_STATE_BOOT:        return "BOOT";
    case EC_STATE_SAFE_OP:     return "SAFE_OP";
    case EC_STATE_OPERATIONAL: return "OP";
    default:                   return "NONE";
    }
}

static void print_slaves(void)
{
    ESP_LOGI(TAG, "%d slave(s):", s_ctx.slavecount);
    for (int i = 1; i <= s_ctx.slavecount; i++) {
        const ec_slavet *sl = &s_ctx.slavelist[i];
        ESP_LOGI(TAG, "  #%d %-24s man 0x%08" PRIx32 " id 0x%08" PRIx32 " rev 0x%08" PRIx32
                 " alias %u addr 0x%04x state %s%s out %ub in %ub DC %s mbx 0x%04x",
                 i, sl->name, sl->eep_man, sl->eep_id, sl->eep_rev, sl->aliasadr, sl->configadr,
                 state_name(sl->state), (sl->state & EC_STATE_ERROR) ? "+ERR" : "",
                 sl->Obits, sl->Ibits, sl->hasdc ? "yes" : "no", sl->mbx_proto);
    }
}

/* ---- Topology: Scan -> Accept -> Save ---- */

static void scan_bus(void)
{
    int found;
    publish_view(ECAT_BUS_PHASE_STARTING, false);
    ecat_coupler_reset();
    ecat_drive_reset();
    while ((found = ecx_config_init(&s_ctx)) <= 0) {
        soem_esp_stats_t nic;
        soem_esp_get_stats(&nic);
        if (found == EC_SLAVECOUNTEXCEEDED) {
            ESP_LOGE(TAG, "too many slaves: this build handles at most %d (CONFIG_SOEM_MAXSLAVE - 1)",
                     EC_MAXSLAVE - 1);
        } else {
            ESP_LOGW(TAG, "no slaves found, retrying (TX %" PRIu32 " RX %" PRIu32 " frames)",
                     nic.tx_frames, nic.rx_frames);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    print_slaves();

    ecat_topology_scan(&s_ctx, &s_bus, &s_bus_issues);
    s_cmp_valid = false;
    if (s_have_saved) {
        ecat_topology_compare(&s_saved, &s_bus, &s_cmp);
        s_cmp_valid = true;
        ecat_topology_assign_names(&s_bus, &s_saved, &s_cmp);
    } else {
        ecat_topology_assign_names(&s_bus, NULL, NULL);
    }
    publish_view(ECAT_BUS_PHASE_STARTING, true);
}

static void show_topology(void)
{
    if (s_have_saved) {
        char when[32] = "time unknown";
        if (s_saved_meta.saved_at) {
            time_t t = (time_t)s_saved_meta.saved_at;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        }
        char title[64];
        snprintf(title, sizeof(title), "Saved topology (%s, by %s)", when, s_saved_meta.saved_by);
        ecat_topology_log(title, &s_saved);
    } else {
        ESP_LOGW(TAG, "No saved topology.");
    }
    ecat_topology_log("Bus (last scan, names proposed)", &s_bus);
    ecat_issues_log(&s_bus, &s_bus_issues);
    if (s_cmp_valid) {
        ecat_compare_log(&s_saved, &s_bus, &s_cmp);
    }
}

static bool accept_scan(const char *who)
{
    if (s_bus.n_slaves == 0) {
        ESP_LOGE(TAG, "Nothing scanned yet.");
        view_message("Nothing scanned yet.");
        return false;
    }
    if (s_bus_issues.n_blocking) {
        ESP_LOGE(TAG, "Cannot accept: %u blocking issue(s) in the scan.", s_bus_issues.n_blocking);
        ecat_issues_log(&s_bus, &s_bus_issues);
        snprintf(s_view_msg, sizeof(s_view_msg), "Not saved: %u blocking issue(s) in the scan.",
                 s_bus_issues.n_blocking);
        return false;
    }

    ecat_topo_meta_t meta = { 0 };
    time_t now = time(NULL);
    meta.saved_at = now > 1700000000 ? (int64_t)now : 0; /* 0 until the RTC/NTP sets the clock */
    strlcpy(meta.saved_by, who, sizeof(meta.saved_by));

    esp_err_t err = ecat_topology_save(&s_bus, &meta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving the topology failed: %s", esp_err_to_name(err));
        snprintf(s_view_msg, sizeof(s_view_msg), "Saving failed: %s", esp_err_to_name(err));
        return false;
    }
    s_saved = s_bus;
    s_saved_meta = meta;
    s_have_saved = true;
    ESP_LOGW(TAG, "Topology saved: %u slave(s). The bus restarts; home every axis before running.",
             (unsigned)s_saved.n_slaves);
    snprintf(s_view_msg, sizeof(s_view_msg), "Topology saved: %u slave(s). Home every axis before running.",
             (unsigned)s_saved.n_slaves);
    return true;
}

static void forget_topology(void)
{
    esp_err_t err = ecat_topology_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erasing the topology failed: %s", esp_err_to_name(err));
        return;
    }
    s_have_saved = false;
    s_cmp_valid = false;
    ESP_LOGW(TAG, "Saved topology erased. 'scan' then 'accept' to save a new one.");
    view_message("Saved topology erased.");
}

static bool topology_allows_op(void)
{
    show_topology();
    if (!s_have_saved) {
#if CONFIG_APP_TOPOLOGY_AUTO_ACCEPT
        if (s_bus_issues.n_blocking == 0) {
            ESP_LOGW(TAG, "APP_TOPOLOGY_AUTO_ACCEPT: saving this first scan without confirmation (bench use only)");
            return accept_scan("auto");
        }
#endif
        ESP_LOGW(TAG, "Check the scan above, then type 'accept' to save it as the expected topology.");
        return false;
    }
    return s_cmp.ok && s_bus_issues.n_blocking == 0;
}

/* ---- Bring-up ---- */

/* One send/receive of the process image. Returns the working counter. */
static int process_data_roundtrip(uint32_t *rtt_us)
{
    int64_t t0 = esp_timer_get_time();
    ecx_send_processdata(&s_ctx);
    int wkc = ecx_receive_processdata(&s_ctx, EC_TIMEOUTRET);
    *rtt_us = (uint32_t)(esp_timer_get_time() - t0);
    return wkc;
}

static void hold_in_preop(void)
{
    s_ctx.slavelist[0].state = EC_STATE_PRE_OP;
    ecx_writestate(&s_ctx, 0);
}

static void prepare_couplers(void)
{
    for (uint16_t i = 0; i < s_bus.n_slaves; i++) {
        const ecat_topo_slave_t *s = &s_bus.slave[i];
        if (s->kind != ECAT_KIND_COUPLER || s->rack == ECAT_TOPO_NO_RACK) {
            continue;
        }
        const ecat_device_info_t *dev = ecat_catalog_device(s->vendor, s->product);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ecat_coupler_prepare(&s_ctx, s->position, dev, &s_bus.rack[s->rack]));
    }
}

/* CiA402 drives: PDO remap for PP/PV/HM in their PRE-OP -> SAFE-OP hook (ecat_drive.h). */
static void prepare_drives(void)
{
    for (uint16_t i = 0; i < s_bus.n_slaves; i++) {
        const ecat_topo_slave_t *s = &s_bus.slave[i];
        if (ecat_kind_is_drive((ecat_kind_t)s->kind)) {
            const ecat_device_info_t *dev = ecat_catalog_device(s->vendor, s->product);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ecat_drive_prepare(&s_ctx, s->position, dev));
        }
    }
}

static bool couplers_ok(bool in_safe_op)
{
    uint16_t list[ECAT_TOPO_MAX_RACKS];
    int n = ecat_coupler_list(list, ECAT_TOPO_MAX_RACKS);
    for (int k = 0; k < n; k++) {
        if (!ecat_coupler_ready(list[k])) {
            ESP_LOGE(TAG, "IO coupler at slave %u is not configured (see ecat_cpl messages above)", list[k]);
            return false;
        }
        if (in_safe_op && !ecat_coupler_check_sm(&s_ctx, list[k])) {
            return false;
        }
    }
    return true;
}

static void log_slaves_not_in(uint16_t want)
{
    ecx_readstate(&s_ctx);
    for (int i = 1; i <= s_ctx.slavecount; i++) {
        const ec_slavet *sl = &s_ctx.slavelist[i];
        if (sl->state != want) {
            ESP_LOGE(TAG, "slave %d not %s: state 0x%04x AL 0x%04x (%s)", i, state_name(want), sl->state,
                     sl->ALstatuscode, ec_ALstatuscode2string(sl->ALstatuscode));
        }
    }
}

static bool bring_up_to_op(void)
{
    ec_groupt *grp = &s_ctx.grouplist[ECAT_GROUP];
    /* Longest PRE-OP -> SAFE-OP / SAFE-OP -> OP time any catalog ESI allows (R1EC: 10 s). */
    const int safeop_op_us = (int)ecat_catalog_safeop_op_timeout_ms * 1000;
    uint32_t rtt;

    /* Zero the image so a new mapping never starts with output bytes of the previous one. */
    memset(s_iomap, 0, sizeof(s_iomap));
    prepare_couplers();
    prepare_drives();
    int used = ecx_config_map_group(&s_ctx, s_iomap, ECAT_GROUP);
    ESP_LOGI(TAG, "mapped %" PRIu32 " output + %" PRIu32 " input bytes in %d segment(s), IOmap %d bytes",
             grp->Obytes, grp->Ibytes, grp->nsegments, used);
    if (used > ECAT_IOMAP_SIZE) {
        /* Mapping only computes offsets; nothing has been exchanged through the image yet. */
        ESP_LOGE(TAG, "IOmap too small (%d bytes)", ECAT_IOMAP_SIZE);
        hold_in_preop();
        return false;
    }
    if (!couplers_ok(false)) {
        hold_in_preop();
        return false;
    }

    ecx_configdc(&s_ctx);
    if (ecx_statecheck(&s_ctx, 0, EC_STATE_SAFE_OP, safeop_op_us) != EC_STATE_SAFE_OP) {
        log_slaves_not_in(EC_STATE_SAFE_OP);
        hold_in_preop();
        return false;
    }
    if (!couplers_ok(true)) {
        hold_in_preop();
        return false;
    }
    s_stats.expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;

    /* Outputs must be valid before slaves accept OP. The IOmap is all zero here:
     * drives get controlword 0 (no motion) and coupler outputs stay off. */
    process_data_roundtrip(&rtt);
    s_ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&s_ctx, 0);
    for (int waited = 0; waited < safeop_op_us; waited += 50000) {
        process_data_roundtrip(&rtt);
        ecx_statecheck(&s_ctx, 0, EC_STATE_OPERATIONAL, 50000);
        if (s_ctx.slavelist[0].state == EC_STATE_OPERATIONAL) {
            ESP_LOGI(TAG, "all slaves OPERATIONAL, expected WKC %" PRId32, s_stats.expected_wkc);
            ecat_axes_bind(&s_ctx, &s_bus); /* names come from the accepted topology */
            ecat_io_bind(&s_ctx, &s_bus);
            return true;
        }
    }

    log_slaves_not_in(EC_STATE_OPERATIONAL);
    hold_in_preop();
    return false;
}

/* Runs until a 'stop' command. */
static void cyclic_loop(void)
{
    const int64_t period = CONFIG_APP_ECAT_CYCLE_US;
    ec_timet ts;
    int64_t next = esp_timer_get_time() + period;

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.expected_wkc = s_ctx.grouplist[ECAT_GROUP].outputsWKC * 2 + s_ctx.grouplist[ECAT_GROUP].inputsWKC;
    s_stats.rtt_min_us = UINT32_MAX;
    memset(s_reverify, 0, sizeof(s_reverify));
    memset(s_blocked, 0, sizeof(s_blocked));
    s_n_blocked = 0;
    s_in_op = true;       /* from here the console refuses commands that need the hold loop */
    xQueueReset(s_cmd_q); /* commands typed during start-up are stale now */
    publish_view(ECAT_BUS_PHASE_OP, false);
    xSemaphoreGive(s_bus_lock);
    bool leaving = false;
    int64_t leave_by = 0;
    for (;;) {
        ts.tv_sec = next / 1000000;
        ts.tv_nsec = (next % 1000000) * 1000;
        osal_monotonic_sleep(&ts);

        int64_t now = esp_timer_get_time();
        uint32_t late = (uint32_t)(now - next);
        if (late > s_stats.wake_late_max_us) {
            s_stats.wake_late_max_us = late;
        }

        uint32_t rtt;
        int wkc = process_data_roundtrip(&rtt);
        s_stats.cycles++;
        s_stats.last_wkc = wkc;
        s_stats.rtt_sum_us += rtt;
        if (rtt < s_stats.rtt_min_us) {
            s_stats.rtt_min_us = rtt;
        }
        if (rtt > s_stats.rtt_max_us) {
            s_stats.rtt_max_us = rtt;
        }
        if (wkc < s_stats.expected_wkc) {
            s_stats.wkc_errors++;
            s_need_check = true;
        }

        /* Leaving OP: the axes stop and switch off first, while the process data still runs. */
        if (s_leave_op && !leaving) {
            leaving = true;
            leave_by = now + LEAVE_OP_TIMEOUT_US;
            ecat_axes_shutdown();
        }
        ecat_axes_cycle(wkc >= s_stats.expected_wkc, now);
        ecat_io_cycle(wkc >= s_stats.expected_wkc);
        if (leaving && (ecat_axes_all_off() || now > leave_by)) {
            break;
        }

        next += period;
        now = esp_timer_get_time();
        if (now > next) {
            /* Missed at least one deadline: resynchronise instead of bursting. */
            s_stats.overruns++;
            next = now + period;
        }
    }
    /* Wait for a running ecat_chk pass to finish before the bus changes under it. */
    xSemaphoreTake(s_bus_lock, portMAX_DELAY);
    ecat_axes_unbind();
    ecat_io_unbind();
    hold_in_preop();
    xQueueReset(s_cmd_q); /* a command queued just as OP started is stale too */
    s_in_op = false;
    s_leave_op = false;
    ESP_LOGW(TAG, "Left OP on request.");
    app_cmd_t next_cmd = s_after_stop;
    s_after_stop = (app_cmd_t)-1;
    if ((int)next_cmd >= 0) {
        (void)xQueueSend(s_cmd_q, &next_cmd, 0); /* the HMI asked for a scan or re-check */
    }
}

/* Bus not in OP: serve console commands until the bus should be configured again. */
static void hold_loop(void)
{
    ESP_LOGW(TAG, "Bus held in PRE-OP. Commands: topo, scan, accept, recheck, forget yes, selftest, help");
    s_holding = true;
    for (;;) {
        publish_view(ECAT_BUS_PHASE_HELD, false);
        app_cmd_t cmd;
        /* While waiting, SDO jobs of the HMI (Settings > Servo) may use the bus. */
        xSemaphoreGive(s_bus_lock);
        const BaseType_t got = xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY);
        xSemaphoreTake(s_bus_lock, portMAX_DELAY);
        if (got != pdTRUE) {
            continue;
        }
        view_message("");
        switch (cmd) {
        case APP_CMD_SHOW:
            show_topology();
            break;
        case APP_CMD_SCAN:
            s_holding = false;
            scan_bus();
            s_holding = true;
            show_topology();
            ESP_LOGI(TAG, "Type 'accept' to save this scan, or 'recheck' to compare again.");
            break;
        case APP_CMD_ACCEPT:
            if (accept_scan(s_accept_by)) {
                s_holding = false;
                return;
            }
            break;
        case APP_CMD_RECHECK:
            s_holding = false;
            return;
        case APP_CMD_FORGET:
            forget_topology();
            break;
        default:
            break;
        }
    }
}

static void ecat_task(void *arg)
{
    esp_eth_handle_t eth;
    bool full_100m = false;

    xSemaphoreTake(s_bus_lock, portMAX_DELAY); /* released only while cyclic_loop() runs */
    ESP_ERROR_CHECK(board_eth_init(EMAC_RX_TASK_PRIO, &eth));
    ESP_ERROR_CHECK(soem_esp_attach(eth));
    ESP_ERROR_CHECK(esp_eth_start(eth));
    while (board_eth_wait_link(eth, 5000, &full_100m) == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "no Ethernet link yet, plug in the first EtherCAT slave (IN port)");
    }
    if (!full_100m) {
        ESP_LOGW(TAG, "link is not 100M full duplex: EtherCAT will not work reliably");
    }

    if (!ecx_init(&s_ctx, "eth0")) {
        ESP_LOGE(TAG, "ecx_init failed");
        vTaskDelete(NULL);
    }

    esp_err_t err = ecat_topology_load(&s_saved, &s_saved_meta);
    s_have_saved = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "loading the saved topology failed: %s", esp_err_to_name(err));
    }

    for (;;) {
        scan_bus();
        if (topology_allows_op()) {
#if CONFIG_APP_ECAT_GO_OP
            publish_view(ECAT_BUS_PHASE_STARTING, false);
            if (bring_up_to_op()) {
                cyclic_loop();
            } else {
                view_message("The bus did not reach OP: see the console log.");
            }
#else
            ESP_LOGI(TAG, "Topology OK. APP_ECAT_GO_OP is disabled: staying in PRE-OP.");
#endif
        }
        hold_loop();
    }
}

/* Called from the console task (core 0). */
bool app_ecat_request(app_cmd_t cmd)
{
    switch (cmd) {
    case APP_CMD_SELFTEST: {
        bool ok = ecat_topology_selftest() == 0;
        ok &= ecat_axis_selftest() <= 0; /* -1: skipped because the bus is in OP */
        return ok;
    }
    case APP_CMD_STOP:
        if (!s_in_op) {
            printf("The bus is not in OP.\n");
            return false;
        }
        s_leave_op = true;
        return true;
    case APP_CMD_SHOW:
        if (s_in_op) {
            show_topology(); /* nothing writes the topology while in OP */
            return true;
        }
        break;
    default:
        if (s_in_op) {
            printf("The bus is in OP: stop the machine and type 'stop' first.\n");
            return false;
        }
        break;
    }
    s_accept_by = "console";
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        printf("Busy, try again.\n");
        return false;
    }
    return true;
}

#if CONFIG_APP_HMI
/* From the HMI (LVGL task, core 0). Scan and Re-check also work in OP: the axes stop, the bus
 * leaves OP, then the request runs. Accept needs a scan, so it never comes while in OP. */
static bool hmi_bus_request(hmi_bus_req_t req)
{
    app_cmd_t cmd;
    switch (req) {
    case HMI_BUS_REQ_SCAN:
        cmd = APP_CMD_SCAN;
        break;
    case HMI_BUS_REQ_RECHECK:
        cmd = APP_CMD_RECHECK;
        break;
    case HMI_BUS_REQ_ACCEPT:
        cmd = APP_CMD_ACCEPT;
        break;
    default:
        return false;
    }
    static const char *const names[] = {[HMI_BUS_REQ_SCAN] = "scan", [HMI_BUS_REQ_ACCEPT] = "accept",
                                        [HMI_BUS_REQ_RECHECK] = "re-check"};
    ESP_LOGI(TAG, "HMI request: %s", names[req]);
    if (s_in_op) {
        if (cmd == APP_CMD_ACCEPT) {
            return false;
        }
        s_after_stop = cmd;
        s_leave_op = true;
        return true;
    }
    s_accept_by = "HMI";
    return xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE;
}
#endif

static const ecat_topo_slave_t *bus_slave_at(uint16_t position)
{
    for (uint16_t k = 0; k < s_bus.n_slaves; k++) {
        if (s_bus.slave[k].position == position) {
            return &s_bus.slave[k];
        }
    }
    return NULL;
}

/*
 * A slave SOEM reconfigured in OP may only return to OP if it is still the accepted one
 * (ETHERCAT_PLAN 3.6). ecx_reconfig_slave() requests SAFE-OP even when the coupler hook
 * refused the rack, so the coupler state is checked here; other slaves by their SII identity.
 */
static bool recovered_slave_ok(uint16_t i)
{
    const ecat_topo_slave_t *s = bus_slave_at(i);
    if (s == NULL) {
        return false;
    }
    if (ecat_coupler_get(i) != NULL) {
        return ecat_coupler_ready(i) && ecat_coupler_check_sm(&s_ctx, i);
    }
    return ecat_topology_verify_slave(&s_ctx, s);
}

static void block_slave(int i)
{
    const ecat_topo_slave_t *s = bus_slave_at((uint16_t)i);
    s_blocked[i] = true;
    s_n_blocked++;
    ecat_axes_block_slave((uint16_t)i);
    s_ctx.slavelist[i].state = EC_STATE_PRE_OP;
    ecx_writestate(&s_ctx, i);
    ESP_LOGE(TAG, "slave %d (%s) is not the accepted device after reconfiguration: held in PRE-OP", i,
             s ? s->name : "?");
}

/*
 * One supervision pass, after SOEM's simple_ng "ecatcheck", with two differences:
 *  - a reconfigured slave must pass recovered_slave_ok() before it gets OP again;
 *  - a lost slave is never recovered with ecx_recover_slave(). A slave that answers again at
 *    its station address kept its configuration (a cable was unplugged, not the power), so it
 *    comes back through the paths above. A slave that lost power has station address 0, and
 *    ecx_recover_slave() would accept any device of the same model at that cable position
 *    (it compares only register 0x0012 and the SII identity). Such a slave stays lost until
 *    'stop' + 'recheck' runs the full scan and comparison: no hot plug (ETHERCAT_PLAN 3.6).
 *    (In simple_ng the recover call is unreachable anyway: it sits in the branch for slaves
 *    already back in OP.)
 */
static void check_slaves(void)
{
    s_need_check = false;
    s_ctx.grouplist[ECAT_GROUP].docheckstate = FALSE;
    ecx_readstate(&s_ctx);
    for (int i = 1; i <= s_ctx.slavecount; i++) {
        ec_slavet *sl = &s_ctx.slavelist[i];
        if (sl->group != ECAT_GROUP || s_blocked[i]) {
            continue;
        }
        if (sl->state != EC_STATE_OPERATIONAL) {
            s_ctx.grouplist[ECAT_GROUP].docheckstate = TRUE;
            if (sl->state == EC_STATE_SAFE_OP + EC_STATE_ERROR) {
                ESP_LOGW(TAG, "slave %d SAFE_OP+ERROR, ack", i);
                sl->state = EC_STATE_SAFE_OP + EC_STATE_ACK;
                ecx_writestate(&s_ctx, i);
            } else if (sl->state == EC_STATE_SAFE_OP) {
                if (s_reverify[i] && !recovered_slave_ok((uint16_t)i)) {
                    block_slave(i);
                    continue;
                }
                s_reverify[i] = false;
                ESP_LOGW(TAG, "slave %d SAFE_OP -> OP", i);
                sl->state = EC_STATE_OPERATIONAL;
                ecx_writestate(&s_ctx, i);
            } else if (sl->state > EC_STATE_NONE) {
                s_reverify[i] = true;
                ecat_coupler_invalidate((uint16_t)i); /* its hook must run and succeed again */
                if (ecx_reconfig_slave(&s_ctx, i, EC_TIMEOUTRET)) {
                    sl->islost = FALSE;
                    ESP_LOGW(TAG, "slave %d reconfigured", i);
                }
            } else if (!sl->islost) {
                ecx_statecheck(&s_ctx, i, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
                if (sl->state == EC_STATE_NONE) {
                    sl->islost = TRUE;
                    ESP_LOGE(TAG, "slave %d lost (if it lost power: 'stop' + 'recheck' once it is back)", i);
                }
            }
        } else if (sl->islost) {
            sl->islost = FALSE;
            ESP_LOGW(TAG, "slave %d found", i);
        }
    }
}

/* Slave supervision while in OP. Runs on core 0. */
static void ecat_check_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!s_in_op || !(s_need_check || s_ctx.grouplist[ECAT_GROUP].docheckstate)) {
            continue;
        }
        if (xSemaphoreTake(s_bus_lock, 0) != pdTRUE) {
            continue; /* the ecat task is leaving OP, or the monitor is reading */
        }
        if (s_in_op) {
            check_slaves();
        }
        xSemaphoreGive(s_bus_lock);
    }
}

/* Hex dump of up to `max` bytes, for the coupler line of the monitor. */
static void hex_bytes(char *out, size_t len, const uint8_t *p, uint32_t n, uint32_t max)
{
    size_t used = 0;
    out[0] = '\0';
    for (uint32_t i = 0; i < n && i < max && used + 4 < len; i++) {
        used += (size_t)snprintf(out + used, len - used, "%02X ", p[i]);
    }
}

static void log_couplers(void)
{
    uint16_t list[ECAT_TOPO_MAX_RACKS];
    int n = ecat_coupler_list(list, ECAT_TOPO_MAX_RACKS);
    for (int k = 0; k < n; k++) {
        const ec_slavet *sl = &s_ctx.slavelist[list[k]];
        const ecat_coupler_t *c = ecat_coupler_get(list[k]);
        if (c == NULL) {
            continue;
        }
        uint32_t status = ecat_coupler_device_status(&s_ctx, list[k]);
        uint32_t skip = c->dev->status_bits / 8;
        char in[52], out[52];
        hex_bytes(in, sizeof(in), sl->inputs ? sl->inputs + skip : NULL, sl->Ibytes > skip ? sl->Ibytes - skip : 0, 16);
        hex_bytes(out, sizeof(out), sl->outputs, sl->Obytes, 16);
        if (status) {
            ESP_LOGW(TAG, "coupler slave %u: DEVICE STATUS 0x%08" PRIX32 " (module fault?) | in %s| out %s", list[k], status,
                     in, out);
        } else {
            ESP_LOGI(TAG, "coupler slave %u: status ok | in %s| out %s", list[k], in, out);
        }
    }
}

#if CONFIG_APP_HMI
/* EtherCAT chip in the HMI title bar. This task is the only writer of the snapshot. */
static void publish_bus_status(bool link_up, const cycle_stats_t *win)
{
    static uint32_t last_wkc_errors;
    uint32_t wkc_errors = s_stats.wkc_errors; /* reset to 0 when OP starts */
    bool new_errors = wkc_errors != last_wkc_errors && wkc_errors != 0;
    last_wkc_errors = wkc_errors;

    hmi_bus_status_t st = {
        .slaves = (uint16_t)(s_ctx.slavecount > 0 ? s_ctx.slavecount : 0),
        .cycle_us = CONFIG_APP_ECAT_CYCLE_US,
    };
    if (win) {
        st.jitter_us = win->wake_late_max_us;
        st.rtt_avg_us = win->cycles ? (uint32_t)(win->rtt_sum_us / win->cycles) : 0;
        st.rtt_max_us = win->rtt_max_us;
        st.overruns = win->overruns;
    }
    if (!link_up) {
        st.state = HMI_BUS_NO_LINK;
    } else if (s_in_op) {
        st.state = (new_errors || s_n_blocked) ? HMI_BUS_OP_FAULT : HMI_BUS_OP;
    } else if (s_holding) {
        st.state = HMI_BUS_HELD;
    } else {
        st.state = HMI_BUS_STARTING;
    }
    hmi_publish_bus_status(&st);
}
#endif

static void ecat_monitor_task(void *arg)
{
    bool link_was_up = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_ECAT_MONITOR_PERIOD_MS));
        soem_esp_stats_t nic;
        soem_esp_get_stats(&nic);
        bool link_up = board_eth_link_is_up();
        /* Timing of the last period; torn reads only affect one report. */
        const bool in_op = s_in_op;
        cycle_stats_t st = {0};
        if (in_op) {
            st = s_stats;
            s_stats.rtt_min_us = UINT32_MAX;
            s_stats.rtt_max_us = 0;
            s_stats.wake_late_max_us = 0;
        }
#if CONFIG_APP_HMI
        publish_bus_status(link_up, in_op ? &st : NULL);
#endif
        if (!in_op) {
            /* Keep the console quiet while it waits for commands; report link changes only. */
            if (link_up != link_was_up) {
                ESP_LOGI(TAG, "link %s | TX %" PRIu32 " RX %" PRIu32 " dropped %" PRIu32 " other %" PRIu32,
                         link_up ? "up" : "down", nic.tx_frames, nic.rx_frames, nic.rx_dropped, nic.rx_non_ecat);
            }
            link_was_up = link_up;
            continue;
        }
        link_was_up = link_up;
        uint32_t avg = st.cycles ? (uint32_t)(st.rtt_sum_us / st.cycles) : 0;
        ESP_LOGI(TAG, "cycles %" PRIu32 " | WKC %" PRId32 "/%" PRId32 " err %" PRIu32
                 " | RTT us min %" PRIu32 " avg %" PRIu32 " max %" PRIu32
                 " | wake late max %" PRIu32 " us | overruns %" PRIu32 " | RX drop %" PRIu32,
                 st.cycles, st.last_wkc, st.expected_wkc, st.wkc_errors,
                 st.rtt_min_us == UINT32_MAX ? 0 : st.rtt_min_us, avg, st.rtt_max_us,
                 st.wake_late_max_us, st.overruns, nic.rx_dropped);
        if (s_n_blocked) {
            ESP_LOGE(TAG, "%u slave(s) held in PRE-OP after a failed identity check: stop the machine, "
                     "then 'stop' and 'recheck'", (unsigned)s_n_blocked);
        }
        if (xSemaphoreTake(s_bus_lock, 0) == pdTRUE) {
            if (s_in_op) {
                log_couplers();
            }
            xSemaphoreGive(s_bus_lock);
        }
    }
}

/* ---- LCD panel variant (New/Old), stored in NVS so it can be changed without rebuilding ---- */

#define NVS_BOARD_NS  "board"
#define NVS_PANEL_KEY "panel"

static board_lcd_panel_t s_panel_active;

static board_lcd_panel_t panel_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_BOARD_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, NVS_PANEL_KEY, &v);
        nvs_close(h);
    }
    return (v == BOARD_LCD_PANEL_NEW || v == BOARD_LCD_PANEL_OLD) ? (board_lcd_panel_t)v : board_lcd_panel_default();
}

/* Called from the console task (core 0). */
bool app_panel_request(const char *arg)
{
    if (arg == NULL) {
        printf("Panel in use: %s. Stored for the next start: %s. menuconfig default: %s.\n",
               board_lcd_panel_name(s_panel_active), board_lcd_panel_name(panel_load()),
               board_lcd_panel_name(board_lcd_panel_default()));
        return true;
    }
    board_lcd_panel_t p = strcmp(arg, "new") == 0 ? BOARD_LCD_PANEL_NEW
                          : strcmp(arg, "old") == 0 ? BOARD_LCD_PANEL_OLD
                                                    : (board_lcd_panel_t)0;
    if (p == 0) {
        printf("Use 'panel new' or 'panel old'.\n");
        return false;
    }
    if (s_in_op) {
        printf("The bus is in OP and flash is not written in OP: stop the machine and type 'stop' first.\n");
        return false;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_BOARD_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_PANEL_KEY, (uint8_t)p);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        printf("Saving failed: %s\n", esp_err_to_name(err));
        return false;
    }
    printf("Saved: %s panel. Press RESET or power-cycle the board to apply.\n", board_lcd_panel_name(p));
    return true;
}

/* Flash is erased only outside OP: an erase stalls both cores for longer than the drives' watchdog.
 * The machine store appends in OP (page programs only) and compacts when this allows it. */
static bool flash_write_allowed(void)
{
    return !s_in_op;
}

/* Called from the alarm supervisor (core 0). Torn reads only delay an alarm by one poll. */
void app_bus_info(app_bus_info_t *out)
{
    out->link_up = board_eth_link_is_up();
    out->in_op = s_in_op;
    out->holding = s_holding;
    out->n_blocked = s_n_blocked;
    out->wkc_errors = s_in_op ? s_stats.wkc_errors : 0;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_cmd_q = xQueueCreate(4, sizeof(app_cmd_t));
    s_bus_lock = xSemaphoreCreateMutex();
    configASSERT(s_cmd_q && s_bus_lock);
    ecat_axes_init(CONFIG_APP_ECAT_CYCLE_US);
    ecat_io_init();

    /* Machine data, clock and alarms (core 0). */
    ESP_ERROR_CHECK(machine_store_init(flash_write_allowed));
    ESP_ERROR_CHECK(machine_alarm_init());
    machine_timers_init();
    (void)machine_clock_init(); /* logs its own errors; the HMI shows "Clock not set" */

    s_panel_active = panel_load();
#if CONFIG_APP_HMI
    /* Before the EtherCAT tasks: the HMI's 96 KB draw buffer needs one contiguous DMA-capable
     * block of internal RAM, which their stacks would split. app_main runs on core 0, so the
     * display and touch interrupts land there. */
    const hmi_config_t hmi_cfg = {
        .panel = s_panel_active,
        .brightness = CONFIG_HMI_BRIGHTNESS,
        .bus_request = hmi_bus_request,
    };
    err = hmi_start(&hmi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HMI start failed (%s); EtherCAT keeps running", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "LCD panel: %s. Wrong colours, flicker or a black screen: type 'panel old' or 'panel new', "
             "then press RESET.", board_lcd_panel_name(s_panel_active));
#endif

    ESP_LOGI(TAG, "EtherCAT master test, cycle %d us, RT core %d", CONFIG_APP_ECAT_CYCLE_US, CONFIG_SOEM_RT_CORE);
    xTaskCreatePinnedToCore(ecat_task, "ecat", ECAT_TASK_STACK, NULL, CONFIG_SOEM_RT_TASK_PRIO, NULL,
                            CONFIG_SOEM_RT_CORE);
    xTaskCreatePinnedToCore(ecat_check_task, "ecat_chk", ECAT_CHECK_STACK, NULL, CONFIG_SOEM_WORKER_TASK_PRIO, NULL, 0);
    xTaskCreatePinnedToCore(ecat_monitor_task, "ecat_mon", 4096, NULL, 2, NULL, 0);
    ecat_sdo_service_start(&s_ctx, s_bus_lock);
    app_alarms_start();
    /* Wi-Fi (ESP32-C6) and NTP, core 0. After the HMI and EtherCAT have their internal RAM. */
    err = net_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network start failed (%s)", esp_err_to_name(err));
    }
#if CONFIG_APP_ECAT_CONSOLE
    ESP_ERROR_CHECK(app_console_start());
#endif
}
