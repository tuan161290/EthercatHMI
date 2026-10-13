/*
 * Serial console commands (esp_console REPL on the primary console, core 0).
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_attr.h"
#include "app_console.h"
#include "camera.h"
#include "ecat_axis.h"
#include "ecat_busview.h"
#include "ecat_sdo_service.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "soem/soem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hmi.h"
#include "esp_timer.h"
#include "machine_alarm.h"
#include "machine_clock.h"
#include "machine_store.h"
#include "net.h"
#include "sdkconfig.h"

static const char *TAG = "console";

static int cmd_topo(int argc, char **argv)
{
    return app_ecat_request(APP_CMD_SHOW) ? 0 : 1;
}

/* ---- Memory (also on Settings > System info) ---- */

static void mem_line(const char *name, uint32_t caps)
{
    const double k = 1024.0;
    printf("%-10s %9.1f %9.1f %9.1f %9.1f\n", name, heap_caps_get_free_size(caps) / k,
           heap_caps_get_minimum_free_size(caps) / k, heap_caps_get_largest_free_block(caps) / k,
           heap_caps_get_total_size(caps) / k);
}

static int cmd_mem(int argc, char **argv)
{
    /* Tasks of this firmware and of ESP-IDF / esp_hosted that matter for the RAM budget. */
    static const char *const tasks[] = {"ecat",      "emac_rx",    "ecat_chk",  "ecat_mon",  "ecat_sdo",
                                        "alarms",    "taskLVGL",   "net",       "tiT",       "sys_evt",
                                        "esp_timer", "console_repl", "sdio_read", "sdio_write", "sdio_process_rx",
                                        "sdio_rx_buf", "rpc_rx",   "rpc_tx",    "ipc0",      "ipc1",
                                        "camera",    "cam_http0",  "cam_http1", "cam_http2", "httpd",
                                        "isp_task"};
    printf("KB         %9s %9s %9s %9s\n", "free", "lowest", "largest", "total");
    mem_line("Internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    mem_line("PSRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    printf("Stack never used (bytes; small = close to overflow):\n");
    int col = 0;
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(tasks[i]);
        if (h == NULL) {
            continue;
        }
        printf("  %-16s %6u%s", tasks[i], (unsigned)uxTaskGetStackHighWaterMark(h), ++col % 3 == 0 ? "\n" : "");
    }
    printf("%s", col % 3 ? "\n" : "");
    return 0;
}

#if CONFIG_APP_HMI
/* CPU load and LVGL frames per second, once a second (as on the title bar). */
static int cmd_cpu(int argc, char **argv)
{
    const int seconds = argc > 1 ? atoi(argv[1]) : 1;
    for (int i = 0; i < seconds; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        hmi_cpu_t c;
        if (!hmi_cpu_get(&c)) {
            printf("No measurement yet\n");
            return 1;
        }
        printf("CPU core 0 %3u %%, core 1 %3u %%; LVGL %2u frames/s (0 = nothing changed on the screen)\n",
               c.load[0], c.load[1], c.fps);
    }
    return 0;
}
#endif

/* ---- Machine data store ---- */

#define STORE_TEST_SAVES 10
#define STORE_TEST_LEN   1024

static int cmd_store(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "test") == 0) {
        /* Ten 1 KB saves, timed; the cycle statistics show what they cost the EtherCAT cycle. */
        uint8_t *buf = heap_caps_malloc(STORE_TEST_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            printf("Out of memory\n");
            return 1;
        }
        int64_t worst = 0, total = 0;
        for (int i = 0; i < STORE_TEST_SAVES; i++) {
            memset(buf, i, STORE_TEST_LEN);
            const int64_t t0 = esp_timer_get_time();
            const esp_err_t err = machine_store_save("test", buf, STORE_TEST_LEN);
            const int64_t dt = esp_timer_get_time() - t0;
            if (err != ESP_OK) {
                printf("Save %d failed: %s\n", i + 1, esp_err_to_name(err));
                break;
            }
            total += dt;
            worst = dt > worst ? dt : worst;
        }
        (void)machine_store_remove("test");
        heap_caps_free(buf);
        printf("%d saves of %d bytes: %lld ms in all, worst %lld ms. See 'overruns' and 'wake late' in the "
               "next cycle line.\n",
               STORE_TEST_SAVES, STORE_TEST_LEN, total / 1000, worst / 1000);
    } else if (argc == 2 && strcmp(argv[1], "compact") == 0) {
        const esp_err_t err = machine_store_compact();
        if (err == ESP_ERR_INVALID_STATE) {
            printf("Not while EtherCAT is in OP: compaction erases flash ('stop' first)\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("Compaction failed: %s\n", esp_err_to_name(err));
            return 1;
        }
    } else if (argc != 1) {
        printf("Use: store | store test | store compact\n");
        return 1;
    }
    machine_store_info_t in;
    machine_store_info(&in);
    if (!in.ok) {
        printf("No 'mstore' partition: machine data are not saved\n");
        return 1;
    }
    printf("Store: half %u, generation %lu, %lu of %lu bytes used, %u key(s), %u waiting in RAM\n", in.half,
           (unsigned long)in.gen, (unsigned long)in.used, (unsigned long)in.size, in.keys, in.waiting);
    char key[16];
    size_t len;
    for (int i = 0; machine_store_key_at(i, key, sizeof(key), &len); i++) {
        printf("  %-15s %u bytes%s\n", key, (unsigned)len, machine_store_key_pending(key) ? " (waiting)" : "");
    }
    return 0;
}

/* ---- SDO reads for diagnostics (through the SDO worker: PRE-OP, SAFE-OP or OP) ---- */

#define SDO_WAIT_MS 10000

/* The job names the slave by position and identity; the identity comes from the last scan. */
static bool sdo_target(uint16_t pos, ecat_sdo_job_t *job)
{
    ecat_busview_t *bv = heap_caps_malloc(sizeof(*bv), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bv == NULL) {
        printf("Out of memory\n");
        return false;
    }
    ecat_busview_read(bv);
    bool found = false;
    for (int i = 0; i < bv->bus.n_slaves && !found; i++) {
        const ecat_topo_slave_t *s = &bv->bus.slave[i];
        if (s->position == pos) {
            memset(job, 0, sizeof(*job));
            job->slave = pos;
            job->vendor = s->vendor;
            job->product = s->product;
            found = true;
        }
    }
    heap_caps_free(bv);
    if (!found) {
        printf("No slave %u in the last scan ('topo' lists them)\n", pos);
    }
    return found;
}

static bool sdo_run(ecat_sdo_job_t *job)
{
    uint32_t ticket;
    if (!ecat_sdo_submit(job, &ticket)) {
        printf("The SDO worker is busy (Settings > Servo?): try again\n");
        return false;
    }
    for (int t = 0; t < SDO_WAIT_MS; t += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
        const ecat_sdo_state_t st = ecat_sdo_poll(ticket, job);
        if (st == ECAT_SDO_DONE) {
            return true;
        }
        if (st != ECAT_SDO_BUSY) {
            printf("Refused: bus busy, slave missing, a different device or not in PRE-OP/SAFE-OP/OP\n");
            return false;
        }
    }
    printf("No result within %d s\n", SDO_WAIT_MS / 1000);
    return false;
}

static void sdo_print_item(const ecat_sdo_item_t *it)
{
    if (it->ok) {
        printf("0x%04X:%u = %lu (0x%08lX)\n", it->index, it->sub, (unsigned long)(uint32_t)it->value,
               (unsigned long)(uint32_t)it->value);
    } else if (it->abort) {
        printf("0x%04X:%u failed: abort 0x%08lX %s\n", it->index, it->sub, (unsigned long)it->abort,
               ec_sdoerror2string(it->abort));
    } else {
        printf("0x%04X:%u failed: no answer\n", it->index, it->sub);
    }
}

/* Sub 0 (the count), then entries 1..count of an array or record object, on one line. */
static void sdo_dump_array(uint16_t pos, uint16_t index, uint8_t max_entries)
{
    ecat_sdo_job_t job;
    if (!sdo_target(pos, &job)) {
        return;
    }
    job.n = 1;
    job.item[0] = (ecat_sdo_item_t){.index = index, .sub = 0, .size = 4};
    if (!sdo_run(&job)) {
        return;
    }
    if (!job.item[0].ok) {
        sdo_print_item(&job.item[0]);
        return;
    }
    uint32_t n = (uint32_t)job.item[0].value & 0xFF;
    printf("0x%04X: %lu entr%s", index, (unsigned long)n, n == 1 ? "y" : "ies");
    if (n > max_entries) {
        n = max_entries;
    }
    job.n = (uint8_t)n;
    for (uint8_t k = 0; k < n; k++) {
        job.item[k] = (ecat_sdo_item_t){.index = index, .sub = (uint8_t)(k + 1), .size = 4};
    }
    if (n && sdo_run(&job)) {
        for (uint8_t k = 0; k < n; k++) {
            if (job.item[k].ok) {
                printf(" %lX", (unsigned long)(uint32_t)job.item[k].value);
            } else {
                printf(" [%u: abort 0x%08lX]", k + 1, (unsigned long)job.item[k].abort);
            }
        }
    }
    printf("\n");
}

static int cmd_sdo(int argc, char **argv)
{
    char *end = NULL;
    const unsigned long pos = argc >= 3 ? strtoul(argv[1], &end, 10) : 0;
    if (argc < 3 || pos < 1 || pos > 0xFFFF || *end != '\0') {
        printf("Use: sdo <pos> <index hex> [sub]   e.g. 'sdo 10 F050 0'\n"
               "     sdo <pos> rack                  modular coupler: 0xF000, 0xF050, 0xF030, 0x1C00, 0x1C12, 0x1C13\n");
        return 1;
    }
    if (strcmp(argv[2], "rack") == 0) {
        static const uint16_t objs[] = {0xF000, 0xF050, 0xF030, 0x1C00, 0x1C12, 0x1C13};
        for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
            sdo_dump_array((uint16_t)pos, objs[i], ECAT_SDO_JOB_MAX);
        }
        return 0;
    }
    const unsigned long index = strtoul(argv[2], &end, 16);
    unsigned long sub = 0;
    if (*end == '\0' && argc >= 4) {
        sub = strtoul(argv[3], &end, 0);
    }
    if (*end != '\0' || index > 0xFFFF || sub > 0xFF) {
        printf("Index is hex (F050 or 0xF050), sub is a number\n");
        return 1;
    }
    ecat_sdo_job_t job;
    if (!sdo_target((uint16_t)pos, &job)) {
        return 1;
    }
    job.n = 1;
    job.item[0] = (ecat_sdo_item_t){.index = (uint16_t)index, .sub = (uint8_t)sub, .size = 4};
    if (!sdo_run(&job)) {
        return 1;
    }
    sdo_print_item(&job.item[0]);
    return job.item[0].ok ? 0 : 1;
}

static int cmd_scan(int argc, char **argv)
{
    return app_ecat_request(APP_CMD_SCAN) ? 0 : 1;
}

static int cmd_accept(int argc, char **argv)
{
    return app_ecat_request(APP_CMD_ACCEPT) ? 0 : 1;
}

static int cmd_recheck(int argc, char **argv)
{
    return app_ecat_request(APP_CMD_RECHECK) ? 0 : 1;
}

static int cmd_forget(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        printf("This erases the saved topology; the bus then stays in PRE-OP until a new one is accepted.\n"
               "Type 'forget yes' to confirm.\n");
        return 1;
    }
    return app_ecat_request(APP_CMD_FORGET) ? 0 : 1;
}

static int cmd_stop(int argc, char **argv)
{
    return app_ecat_request(APP_CMD_STOP) ? 0 : 1;
}

static int cmd_selftest(int argc, char **argv)
{
    return app_ecat_request(APP_CMD_SELFTEST) ? 0 : 1;
}

static int cmd_panel(int argc, char **argv)
{
    return app_panel_request(argc >= 2 ? argv[1] : NULL) ? 0 : 1;
}

/* ---- Axes (ecat_axis.h): servo on/off, reset, stop, home, moves and jog for bench tests ---- */

#define AXIS_DEFAULT_RPM      60.0
#define AXIS_MAX_RPM          3000.0
#define AXIS_ASSUMED_UNITS    10000u /* units per rev when the drive does not tell (unverified) */
#define AXIS_JOG_REFRESH_MS   100
#define AXIS_JOG_MAX_MS       10000

static EXT_RAM_BSS_ATTR ecat_axes_status_t s_axes; /* console task only; too big for its stack */

static uint32_t axis_units_per_rev(const ecat_axis_status_t *a)
{
    return a->units_per_rev ? a->units_per_rev : AXIS_ASSUMED_UNITS;
}

static uint32_t rpm_to_units(const ecat_axis_status_t *a, double rpm)
{
    double v = rpm / 60.0 * (double)axis_units_per_rev(a);
    return v < 1.0 ? 1u : v > 2.0e9 ? 2000000000u : (uint32_t)v;
}

/* Position units, or revolutions with an 'r' suffix ("2.5r"). */
static bool parse_position(const ecat_axis_status_t *a, const char *s, int32_t *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s) {
        return false;
    }
    if (*end == 'r' || *end == 'R') {
        v *= (double)axis_units_per_rev(a);
        end++;
    }
    if (*end != '\0' || v > 2.0e9 || v < -2.0e9) {
        return false;
    }
    *out = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
    return true;
}

static bool parse_rpm(const char *s, double *rpm)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || v <= 0.0 || v > AXIS_MAX_RPM) {
        return false;
    }
    *rpm = v;
    return true;
}

/* Axis by 1-based number or by name ("AX3"). */
static int find_axis(const char *arg)
{
    char *end;
    long n = strtol(arg, &end, 10);
    if (*arg != '\0' && *end == '\0') {
        return (n >= 1 && n <= s_axes.n_axes) ? (int)n - 1 : -1;
    }
    for (int i = 0; i < s_axes.n_axes; i++) {
        if (strcasecmp(arg, s_axes.axis[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

static void axis_print(int i)
{
    const ecat_axis_status_t *a = &s_axes.axis[i];
    printf("%d %-6s slave %u | %s | %s%s%s | pos %ld (%.3f rev) vel %ld | SW 0x%04X CW 0x%04X mode %d | "
           "error 0x%04X/0x%04X | %s\n",
           i + 1, a->name, a->slave, a->available ? ecat_cia_state_text(a->cia_state) : "UNAVAILABLE",
           ecat_axis_job_text(a->job), a->homed ? ", homed" : "", a->units_per_rev ? "" : ", units/rev unknown",
           (long)a->position, (double)a->position / (double)axis_units_per_rev(a), (long)a->velocity, a->statusword,
           a->controlword, a->mode_display, a->error_code, a->vendor_error, ecat_axis_error_text(a->error));
}

static void axis_usage(void)
{
    printf("axis                          list the axes\n"
           "axis <n|name> on|off|reset    servo on / servo off / fault reset\n"
           "axis <n|name> stop|qstop      halt / quick stop (then servo off)\n"
           "axis <n|name> home            homing with the drive's method (0x6098)\n"
           "axis <n|name> move <pos> [rpm]  absolute move; <pos> in units, or revs with 'r' (2.5r)\n"
           "axis <n|name> rel <dist> [rpm]  relative move from the actual position\n"
           "axis <n|name> jog +|- [rpm] [ms]  continuous jog, default %.0f rpm for 1000 ms\n",
           AXIS_DEFAULT_RPM);
}

static int cmd_axis(int argc, char **argv)
{
    ecat_axes_read(&s_axes);
    if (argc == 1) {
        if (!s_axes.in_op) {
            printf("The bus is not in OP: no axes. Scan and accept the topology first ('topo').\n");
            return 1;
        }
        if (s_axes.n_axes == 0) {
            printf("No drives in the accepted topology.\n");
        }
        for (int i = 0; i < s_axes.n_axes; i++) {
            axis_print(i);
        }
        return 0;
    }
    if (argc < 3) {
        axis_usage();
        return 1;
    }
    const int ax = find_axis(argv[1]);
    if (ax < 0) {
        printf("No axis '%s'. Type 'axis' for the list.\n", argv[1]);
        return 1;
    }
    const ecat_axis_status_t *a = &s_axes.axis[ax];
    ecat_axis_cmd_t c = {.axis = (uint8_t)ax};
    const char *op = argv[2];
    double rpm = AXIS_DEFAULT_RPM;
    bool ok = true;

    if (strcmp(op, "on") == 0) {
        c.code = ECAT_AXIS_ENABLE;
    } else if (strcmp(op, "off") == 0) {
        c.code = ECAT_AXIS_DISABLE;
    } else if (strcmp(op, "reset") == 0) {
        c.code = ECAT_AXIS_RESET;
    } else if (strcmp(op, "stop") == 0) {
        c.code = ECAT_AXIS_STOP;
    } else if (strcmp(op, "qstop") == 0) {
        c.code = ECAT_AXIS_QUICK_STOP;
    } else if (strcmp(op, "home") == 0) {
        c.code = ECAT_AXIS_HOME;
    } else if ((strcmp(op, "move") == 0 || strcmp(op, "rel") == 0) && (argc == 4 || argc == 5)) {
        c.code = op[0] == 'm' ? ECAT_AXIS_MOVE_ABS : ECAT_AXIS_MOVE_REL;
        ok = parse_position(a, argv[3], &c.position) && (argc == 4 || parse_rpm(argv[4], &rpm));
        c.velocity = rpm_to_units(a, rpm);
    } else if (strcmp(op, "jog") == 0 && argc >= 4 && argc <= 6) {
        int ms = 1000;
        ok = (strcmp(argv[3], "+") == 0 || strcmp(argv[3], "-") == 0) && (argc < 5 || parse_rpm(argv[4], &rpm));
        if (ok && argc == 6) {
            ms = atoi(argv[5]);
            ok = ms > 0 && ms <= AXIS_JOG_MAX_MS;
        }
        if (!ok) {
            axis_usage();
            return 1;
        }
        c.code = ECAT_AXIS_JOG;
        c.dir = argv[3][0] == '+' ? 1 : -1;
        c.velocity = rpm_to_units(a, rpm);
        /* Repeat like a held jog button; the axis stops by itself if the refresh stops. */
        for (int t = 0; t < ms; t += AXIS_JOG_REFRESH_MS) {
            (void)ecat_axis_command(&c);
            vTaskDelay(pdMS_TO_TICKS(AXIS_JOG_REFRESH_MS));
        }
        c.dir = 0;
    } else {
        ok = false;
    }
    if (!ok) {
        axis_usage();
        return 1;
    }
    if (!a->units_per_rev && (c.code == ECAT_AXIS_JOG || c.code == ECAT_AXIS_MOVE_ABS || c.code == ECAT_AXIS_MOVE_REL)) {
        printf("Units per revolution unknown: assuming %u (0x608F/0x6092 not readable).\n", AXIS_ASSUMED_UNITS);
    }
    if (!ecat_axis_command(&c)) {
        printf("Command queue full, try again.\n");
        return 1;
    }
    vTaskDelay(pdMS_TO_TICKS(150)); /* let the cyclic task take it */
    ecat_axes_read(&s_axes);
    if (ax < s_axes.n_axes) {
        axis_print(ax);
    }
    return 0;
}

/* ---- Two-axis path in CSP (ecat_path_*), distances in motor revolutions ---- */

#define PATH_DEFAULT_RPS     1.0   /* path speed, rev/s */
#define PATH_DEFAULT_RAMP_MS 200.0 /* standstill to that speed */

static bool parse_num(const char *s, double *out)
{
    char *end;
    const double v = strtod(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static void path_print(void)
{
    const ecat_path_status_t *p = &s_axes.path;
    static const char *const kinds[] = {"line", "arc cw", "arc ccw"};
    const char *x = p->axis[0] < s_axes.n_axes ? s_axes.axis[p->axis[0]].name : "?";
    const char *y = p->axis[1] < s_axes.n_axes ? s_axes.axis[p->axis[1]].name : "?";
    const char *who = p->err_axis == 0 ? x : p->err_axis == 1 ? y : "";
    printf("path %lu, %s %s/%s: %s%s, %.4f of %.4f rev, %.3f rev/s, contour %.5f rev (max %.5f), lag max %.5f rev, "
           "%lu ms\n",
           (unsigned long)p->gen, p->kind < 3 ? kinds[p->kind] : "?", x, y, ecat_path_phase_text(p->phase),
           p->phase == ECAT_PATH_IDLE && p->done ? ", done" : "", (double)p->s, (double)p->length,
           (double)p->speed, (double)p->contour, (double)p->contour_max, (double)p->lag_max,
           (unsigned long)p->time_ms);
    if (p->error) {
        printf("  %s%s%s\n", who, *who ? ": " : "", ecat_axis_error_text(p->error));
    }
}

static void path_usage(void)
{
    printf("path                                  status of the last path\n"
           "path stop                             decelerate along the path and stop\n"
           "path line <X> <Y> <dx> <dy> [rev/s] [ramp ms]\n"
           "path cw|ccw <X> <Y> <i> <j> [deg] [rev/s] [ramp ms]   arc around the centre (i, j); 360 deg = circle\n"
           "  Distances in motor revolutions from where the axes are; both servos on. Default %.1f rev/s, %.0f ms.\n"
           "  Max speed of Settings > Servo is NOT applied here (bench tests only).\n",
           PATH_DEFAULT_RPS, PATH_DEFAULT_RAMP_MS);
}

static int cmd_path(int argc, char **argv)
{
    ecat_axes_read(&s_axes);
    if (argc == 1) {
        if (s_axes.path.gen == 0) {
            printf("No path since the bus went to OP.\n");
        } else {
            path_print();
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        ecat_path_stop();
        return 0;
    }
    const bool line = strcmp(argv[1], "line") == 0;
    const bool arc = strcmp(argv[1], "cw") == 0 || strcmp(argv[1], "ccw") == 0;
    if ((!line && !arc) || argc < 6) {
        path_usage();
        return 1;
    }
    if (!s_axes.in_op) {
        printf("The bus is not in OP: no axes.\n");
        return 1;
    }
    const int x = find_axis(argv[2]);
    const int y = find_axis(argv[3]);
    if (x < 0 || y < 0) {
        printf("No axis '%s'. Type 'axis' for the list.\n", x < 0 ? argv[2] : argv[3]);
        return 1;
    }
    double a, b, angle = 360.0, speed = PATH_DEFAULT_RPS, ramp = PATH_DEFAULT_RAMP_MS;
    int n = 6;
    bool ok = parse_num(argv[4], &a) && parse_num(argv[5], &b);
    if (ok && arc && argc > n) {
        ok = parse_num(argv[n++], &angle) && angle > 0.0 && angle <= 360.0;
    }
    if (ok && argc > n) {
        ok = parse_num(argv[n++], &speed) && speed > 0.0;
    }
    if (ok && argc > n) {
        ok = parse_num(argv[n++], &ramp) && ramp >= 1.0;
    }
    if (!ok || argc > n) {
        path_usage();
        return 1;
    }

    ecat_path_cmd_t c = {
        .axis = {(uint8_t)x, (uint8_t)y},
        .kind = line ? ECAT_PATH_LINE : argv[1][1] == 'w' ? ECAT_PATH_ARC_CW : ECAT_PATH_ARC_CCW,
        .k = {(float)axis_units_per_rev(&s_axes.axis[x]), (float)axis_units_per_rev(&s_axes.axis[y])},
        .speed = (float)speed,
        .accel = (float)(speed * 1000.0 / ramp),
    };
    if (line) {
        c.end[0] = (float)a;
        c.end[1] = (float)b;
    } else {
        c.center[0] = (float)a;
        c.center[1] = (float)b;
        if (angle < 360.0) { /* else end = start: a full circle */
            const double r = hypot(a, b);
            const double th = atan2(-b, -a) + (c.kind == ECAT_PATH_ARC_CCW ? 1.0 : -1.0) * angle * M_PI / 180.0;
            c.end[0] = (float)(a + r * cos(th));
            c.end[1] = (float)(b + r * sin(th));
        }
    }
    float len;
    const char *why = ecat_path_check(&c, &len);
    if (why) {
        printf("%s\n", why);
        return 1;
    }
    if (!ecat_path_start(&c)) {
        printf("Command queue full, try again.\n");
        return 1;
    }
    printf("Path of %.4f rev at %.3f rev/s sent. 'path' shows the result, 'path stop' stops it.\n", (double)len,
           speed);
    return 0;
}

#if CONFIG_APP_HMI
static bool parse_pct(const char *s, uint8_t *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < 0 || v > 100) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static bool parse_timeout(const char *s, uint16_t *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < 0 || v > HMI_SCREEN_TIMEOUT_MAX_S) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

static void display_print(void)
{
    static const char *const screen[] = {"on", "dimmed", "off"};
    hmi_display_t d;
    hmi_display_get(&d);
    printf("Brightness %u %%, colour balance R %u G %u B %u %%%s\n", d.brightness, d.red, d.green, d.blue,
           hmi_display_save_pending() ? " (not saved yet: EtherCAT is in OP)" : "");
    printf("Screen %s; dims after %u s, off after %u s without a touch (0 = never)\n", screen[hmi_screen_get()],
           d.dim_s, d.off_s);
}

static int cmd_display(int argc, char **argv)
{
    hmi_display_t d;
    hmi_display_get(&d);
    bool ok;
    if (argc == 1) {
        display_print();
        return 0;
    } else if (argc == 2 && strcmp(argv[1], "wake") == 0) {
        hmi_screen_wake();
        display_print();
        return 0;
    } else if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        const hmi_display_t cur = d;
        hmi_display_defaults(&d);
        d.dim_s = cur.dim_s; /* as the Reset button: brightness and colour balance only */
        d.off_s = cur.off_s;
        ok = true;
    } else if (argc == 3 && strcmp(argv[1], "bright") == 0) {
        ok = parse_pct(argv[2], &d.brightness);
    } else if (argc == 5 && strcmp(argv[1], "rgb") == 0) {
        ok = parse_pct(argv[2], &d.red) && parse_pct(argv[3], &d.green) && parse_pct(argv[4], &d.blue);
    } else if (argc == 3 && strcmp(argv[1], "dim") == 0) {
        ok = parse_timeout(argv[2], &d.dim_s);
    } else if (argc == 3 && strcmp(argv[1], "off") == 0) {
        ok = parse_timeout(argv[2], &d.off_s);
    } else {
        ok = false;
    }
    if (!ok) {
        printf("Use: display | display bright <%d-100> | display rgb <r> <g> <b> (each %d-100) | display reset\n"
               "     display dim <s> | display off <s> (0-%d, 0 = never) | display wake\n",
               HMI_BRIGHTNESS_MIN, HMI_GAIN_MIN, HMI_SCREEN_TIMEOUT_MAX_S);
        return 1;
    }
    esp_err_t err = hmi_display_set(&d, true);
    if (err != ESP_OK) {
        printf("Failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    display_print();
    return 0;
}

static int cmd_touchlog(int argc, char **argv)
{
    hmi_touchlog_print();
    return 0;
}

static int cmd_screenshot(int argc, char **argv)
{
    hmi_screenshot_print();
    return 0;
}

static int cmd_page(int argc, char **argv)
{
    if (argc != 2 || !hmi_open_page(argv[1])) {
        printf("Use: page main|manual|teach|interp|io|alarms|settings|time|servo|ethercat|system|network|info\n");
        return 1;
    }
    return 0;
}
#endif

/* ---- Clock (RX8025T) ---- */

static void clock_print(void)
{
    char tz[16];
    machine_clock_tz_text(machine_clock_tz_minutes(), tz, sizeof(tz));
    if (!machine_clock_valid()) {
        printf("Clock not set (%s, RTC %s)\n", tz, machine_clock_rtc_present() ? "present" : "not found");
        return;
    }
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char t[32];
    strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", &tm);
    printf("%s %s, RTC %s\n", t, tz, machine_clock_rtc_present() ? "present" : "not found");
}

static int cmd_clock(int argc, char **argv)
{
    if (argc == 1) {
        clock_print();
        return 0;
    }
    struct tm tm = {0};
    if (argc == 3 && sscanf(argv[1], "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3 &&
        sscanf(argv[2], "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 2) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        const esp_err_t err = machine_clock_set_local(&tm);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            printf("Failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        clock_print();
        return 0;
    }
    printf("Use: clock | clock YYYY-MM-DD HH:MM[:SS] (local time)\n");
    return 1;
}

/* ---- Wi-Fi and internet time (components/net) ---- */

static void wifi_print(void)
{
    net_status_t st;
    net_status(&st);
    printf("Wi-Fi %s: %s", st.wifi_on ? "on" : "off", net_state_text(st.state));
    if (st.ssid[0]) {
        printf(" | network \"%s\"", st.ssid);
    }
    if (st.state == NET_WIFI_CONNECTED) {
        printf(" | IP %u.%u.%u.%u | %d dBm", st.ip[0], st.ip[1], st.ip[2], st.ip[3], st.rssi);
    }
    if (st.reason[0]) {
        printf(" | %s", st.reason);
    }
    printf("\nInternet time %s (%s)", st.ntp_on ? "on" : "off", st.ntp_server);
    if (st.last_sync) {
        struct tm l;
        localtime_r(&st.last_sync, &l);
        char t[32];
        strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", &l);
        printf(" | last sync %s", t);
    }
    if (st.ntp_error[0]) {
        printf(" | %s", st.ntp_error);
    }
    printf("\n");
}

static int wifi_scan_print(void)
{
    net_status_t st;
    net_status(&st);
    const uint32_t gen = st.scan_gen;
    esp_err_t err = net_scan();
    if (err != ESP_OK) {
        printf("Scan refused: %s (is Wi-Fi on?)\n", esp_err_to_name(err));
        return 1;
    }
    for (int i = 0; i < 100 && st.scan_gen == gen; i++) { /* up to 10 s */
        vTaskDelay(pdMS_TO_TICKS(100));
        net_status(&st);
    }
    if (st.scan_gen == gen) {
        printf("No scan result within 10 s.\n");
        return 1;
    }
    static net_ap_t aps[NET_SCAN_MAX]; /* console task only */
    const int n = net_scan_results(aps, NET_SCAN_MAX);
    printf("%d network(s):\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %4d dBm  %-7s %s\n", aps[i].rssi, aps[i].secured ? "secured" : "open", aps[i].ssid);
    }
    return 0;
}

/* Core and priority of the network and camera tasks: esp_hosted's and esp_video's must be on core 0
 * (components/net, board_camera.c). */
static void wifi_tasks_print(void)
{
    static const char *const names[] = {"sdio_read", "sdio_write", "sdio_process_rx", "sdio_rx_buf", "rpc_rx",
                                        "rpc_tx", "rpc_supp_cb", "tiT", "sys_evt", "net", "isp_task", "camera",
                                        "cam_http0", "httpd", "ecat", "emac_rx"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(names[i]);
        if (h == NULL) {
            printf("  %-16s not running\n", names[i]);
            continue;
        }
        const BaseType_t core = xTaskGetCoreID(h);
        printf("  %-16s prio %2u  core %s\n", names[i], (unsigned)uxTaskPriorityGet(h),
               core == tskNO_AFFINITY ? "any (!)" : core == 0 ? "0" : "1");
    }
}

static int cmd_wifi(int argc, char **argv)
{
    esp_err_t err = ESP_OK;
    if (argc == 1) {
        wifi_print();
        return 0;
    } else if (argc == 2 && strcmp(argv[1], "tasks") == 0) {
        wifi_tasks_print();
        return 0;
    } else if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        return wifi_scan_print();
    } else if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        err = net_set_wifi(strcmp(argv[1], "on") == 0);
    } else if (argc == 2 && strcmp(argv[1], "forget") == 0) {
        err = net_forget();
    } else if ((argc == 3 || argc == 4) && strcmp(argv[1], "join") == 0) {
        err = net_join(argv[2], argc == 4 ? argv[3] : NULL);
    } else {
        printf("Use: wifi | wifi on | wifi off | wifi scan | wifi join <ssid> [password] | wifi forget | wifi tasks\n"
               "     (quote a name with spaces: wifi join \"My net\" secret123)\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("Failed: %s\n", err == ESP_ERR_INVALID_ARG ? "name 1-32 characters, password 8-63 or none"
                                                          : esp_err_to_name(err));
        return 1;
    }
    printf("OK; type 'wifi' to follow the state.\n");
    return 0;
}

static int cmd_ntp(int argc, char **argv)
{
    net_status_t st;
    net_status(&st);
    esp_err_t err = ESP_OK;
    if (argc == 1) {
        wifi_print();
        return 0;
    } else if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        err = net_set_ntp(strcmp(argv[1], "on") == 0, NULL);
    } else if (argc == 3 && strcmp(argv[1], "server") == 0) {
        err = net_set_ntp(st.ntp_on, argv[2]);
    } else if (argc == 2 && strcmp(argv[1], "sync") == 0) {
        err = net_ntp_sync_now();
    } else {
        printf("Use: ntp | ntp on | ntp off | ntp server <name> | ntp sync\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("Failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("OK; type 'ntp' to follow the state.\n");
    return 0;
}

/* ---- Camera (components/camera) ---- */

static int cmd_cam(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        (void)camera_set_on(strcmp(argv[1], "on") == 0);
        printf("OK; type 'cam' to follow the state.\n");
        return 0;
    } else if (argc != 1) {
        printf("Use: cam | cam on | cam off\n");
        return 1;
    }
    camera_status_t c;
    camera_status(&c);
    printf("Camera %s: %s", c.on ? "on" : "off", camera_state_text(c.state));
    if (c.reason[0]) {
        printf(" | %s", c.reason);
    }
    if (c.sensor[0]) {
        printf(" | %s %ux%u", c.sensor, c.width, c.height);
    }
    printf("\n");
    net_status_t n;
    net_status(&n);
    if (c.state == CAMERA_READY || c.state == CAMERA_STREAMING) {
        printf("Stream http://%u.%u.%u.%u:%u/stream | snapshot /snapshot.jpg\n", n.ip[0], n.ip[1], n.ip[2], n.ip[3],
               c.port);
    }
    printf("Viewers %u | %u.%u frames/s (at most %u) | %" PRIu32 " kbit/s | last frame %" PRIu32 " KB | %" PRIu32
           " frames since start-up\n",
           c.viewers, c.fps_x10 / 10, c.fps_x10 % 10, c.max_fps, c.kbit_s, c.frame_bytes / 1024, c.frames);
    return 0;
}

/* ---- Alarms ---- */

static int cmd_alarms(int argc, char **argv)
{
    static machine_alarm_t list[MACHINE_ALARM_ACTIVE_MAX]; /* console task only */
    if (argc == 2 && strcmp(argv[1], "ack") == 0) {
        machine_alarm_ack_all();
    } else if (argc != 1) {
        printf("Use: alarms | alarms ack\n");
        return 1;
    }
    const int n = machine_alarm_list(list, MACHINE_ALARM_ACTIVE_MAX);
    if (n == 0) {
        printf("No alarms.\n");
    }
    static const char *const st[] = {"ACTIVE", "ACK", "CLEARED"};
    for (int i = 0; i < n; i++) {
        const machine_alarm_t *a = &list[i];
        printf("%-5s %-7s %-12s %s | %s\n", a->code, a->sev == MACHINE_ALARM_ERROR ? "ERROR" : "WARNING",
               st[machine_alarm_state(a)], a->title, a->source);
    }
    return 0;
}

esp_err_t app_console_start(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "topo", .help = "Show the saved topology, the last bus scan and the comparison", .func = cmd_topo },
        { .command = "mem", .help = "RAM: free, lowest since start-up, largest block, total; stack reserve of the tasks",
          .func = cmd_mem },
        { .command = "store", .help = "Machine data store: keys and space; 'store test' times ten 1 KB saves; "
                                      "'store compact' (not in OP)",
          .func = cmd_store },
        { .command = "sdo", .help = "Read a CoE object: 'sdo 10 F050 0' (slave position, hex index, sub); "
                                    "'sdo 10 rack' dumps a modular coupler's module lists and PDO assignment",
          .func = cmd_sdo },
        { .command = "scan", .help = "Scan the bus again (not in OP)", .func = cmd_scan },
        { .command = "accept", .help = "Save the last scan as the expected topology", .func = cmd_accept },
        { .command = "recheck", .help = "Scan, compare with the saved topology, go to OP if it matches", .func = cmd_recheck },
        { .command = "forget", .help = "Erase the saved topology ('forget yes')", .func = cmd_forget },
        { .command = "stop", .help = "Leave OP and hold the bus in PRE-OP", .func = cmd_stop },
        { .command = "selftest", .help = "Run the topology logic self-test (no hardware needed)", .func = cmd_selftest },
        { .command = "panel", .help = "Show or store the LCD panel variant: 'panel', 'panel new', 'panel old' (applies after reset)",
          .func = cmd_panel },
        { .command = "axis", .help = "Axes in OP: 'axis' lists them; 'axis 1 on|off|reset|stop|qstop|home', "
                                     "'axis 1 move|rel <pos> [rpm]', 'axis 1 jog +|- [rpm] [ms]'",
          .func = cmd_axis },
        { .command = "path", .help = "Two-axis path in CSP: 'path' status, 'path stop', "
                                     "'path line <X> <Y> <dx> <dy> [rev/s] [ms]', "
                                     "'path cw|ccw <X> <Y> <i> <j> [deg] [rev/s] [ms]' (revolutions)",
          .func = cmd_path },
#if CONFIG_APP_HMI
        { .command = "display", .help = "Show or set brightness and colour balance (%) and the screen timeouts (s): "
                                        "'display', 'display bright 60', 'display rgb 100 95 90', 'display reset', "
                                        "'display dim 120', 'display off 600', 'display wake'. Applies at once and is "
                                        "saved",
          .func = cmd_display },
        { .command = "cpu", .help = "CPU load of both cores and LVGL frames/s over the last second; 'cpu 10' prints "
                                    "10 lines, one a second",
          .func = cmd_cpu },
        { .command = "page", .help = "Show an HMI page: 'page io', 'page teach', 'page servo', ... (bench tests)",
          .func = cmd_page },
        { .command = "touchlog", .help = "Print the last 32 touches: point, down/up and the widget hit",
          .func = cmd_touchlog },
        { .command = "screenshot", .help = "Print the screen as text lines; run 'python tools/screenshot.py COMx' "
                                           "on the PC to get a PNG",
          .func = cmd_screenshot },
#endif
        { .command = "clock", .help = "Show the clock, or set it: 'clock 2026-10-10 14:30' (local time, also the RTC)",
          .func = cmd_clock },
        { .command = "wifi", .help = "Wi-Fi through the ESP32-C6: 'wifi', 'wifi on|off', 'wifi scan', "
                                     "'wifi join <ssid> [password]', 'wifi forget'",
          .func = cmd_wifi },
        { .command = "ntp", .help = "Internet time: 'ntp', 'ntp on|off', 'ntp server pool.ntp.org', 'ntp sync'",
          .func = cmd_ntp },
        { .command = "cam", .help = "Camera stream over Wi-Fi: 'cam' shows the state, address and frame rate; "
                                    "'cam on|off' (saved, as Settings > Camera)",
          .func = cmd_cam },
        { .command = "alarms", .help = "List the current alarms; 'alarms ack' acknowledges all", .func = cmd_alarms },
    };

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "ecat>";
    cfg.max_cmdline_length = 128;
    cfg.task_stack_size = 6144;
    cfg.task_core_id = 0; /* core 1 belongs to EtherCAT */

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t dev = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&dev, &cfg, &repl), TAG, "repl");
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t dev = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev, &cfg, &repl), TAG, "repl");
#else
#error "No primary console with input: enable UART or USB-Serial-JTAG console"
#endif

    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    return esp_console_start_repl(repl);
}
