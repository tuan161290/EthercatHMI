/*
 * HMI simulator: the machine as the pages see it. The bench bus of 2026-10-11 (7x L5N,
 * 2x CL3B-EC507, one R1EC with 2x SC-3200-1 + SC-0032-N-1) in OP, AX1 enabled and homed, a few
 * alarms, Wi-Fi on "OpenWrt". Catalog, topology rules, parameters and alarms are the firmware's
 * own code (sim_build.py REAL); the machine store keeps values in RAM.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "ecat_axis.h"
#include "ecat_busview.h"
#include "ecat_catalog.h"
#include "ecat_drive.h"
#include "ecat_io.h"
#include "ecat_sdo_service.h"
#include "hmi.h"
#include "machine_alarm.h"
#include "machine_clock.h"
#include "machine_params.h"
#include "machine_store.h"
#include "camera.h"
#include "net.h"

/* ---- Machine store: RAM only ---- */

#define STORE_KEYS 32

static struct {
    char key[16];
    void *data;
    size_t len;
} s_store[STORE_KEYS];

esp_err_t machine_store_load(const char *key, void *buf, size_t len)
{
    for (int i = 0; i < STORE_KEYS; i++) {
        if (s_store[i].data && strcmp(s_store[i].key, key) == 0) {
            if (s_store[i].len != len) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(buf, s_store[i].data, len);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t machine_store_save(const char *key, const void *buf, size_t len)
{
    int slot = -1;
    for (int i = 0; i < STORE_KEYS; i++) {
        if (s_store[i].data && strcmp(s_store[i].key, key) == 0) {
            free(s_store[i].data);
            s_store[i].data = NULL;
        }
        if (s_store[i].data == NULL && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(s_store[slot].key, key, sizeof(s_store[slot].key));
    s_store[slot].data = malloc(len);
    memcpy(s_store[slot].data, buf, len);
    s_store[slot].len = len;
    return ESP_OK;
}

bool machine_store_pending(void)
{
    return false;
}

void machine_store_info(machine_store_info_t *out)
{
    int keys = 0;
    uint32_t used = 64;
    for (int i = 0; i < STORE_KEYS; i++) {
        if (s_store[i].data) {
            keys++;
            used += (uint32_t)(36 + (s_store[i].len + 15) / 16 * 16);
        }
    }
    *out = (machine_store_info_t){.ok = true, .used = used, .size = 256 * 1024, .keys = (uint8_t)keys};
}

bool machine_store_key_pending(const char *key)
{
    (void)key;
    return false;
}

/* ---- Clock ---- */

static int s_tz = 7 * 60;

bool machine_clock_valid(void)
{
    return true;
}

bool machine_clock_rtc_present(void)
{
    return true;
}

esp_err_t machine_clock_set_local(const struct tm *local)
{
    (void)local;
    return ESP_OK;
}

esp_err_t machine_clock_save_to_rtc(void)
{
    return ESP_OK;
}

int machine_clock_tz_minutes(void)
{
    return s_tz;
}

esp_err_t machine_clock_set_tz(int minutes)
{
    s_tz = minutes;
    return ESP_OK;
}

void machine_clock_tz_text(int minutes, char *out, size_t len)
{
    const int a = abs(minutes);
    snprintf(out, len, "UTC%c%02d:%02d", minutes >= 0 ? '+' : '-', a / 60, a % 60);
}

/* ---- EtherCAT ---- */

#define VENDOR_LEADSHINE 0x00004321u
#define PRODUCT_L5N      0x000000F0u
#define PRODUCT_CL3B     0x00000800u
#define PRODUCT_R1EC     0x61400015u

static const uint32_t s_rack[] = {0x71101045u, 0x71101045u, 0x71101405u};
static ecat_topology_t s_topo;
static ecat_io_layout_t s_layout;

static void topo_build(void)
{
    ecat_topology_t *t = &s_topo;
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 10; i++) {
        ecat_topo_slave_t *s = &t->slave[i];
        s->position = (uint16_t)(i + 1);
        s->vendor = VENDOR_LEADSHINE;
        s->product = i < 7 ? PRODUCT_L5N : i < 9 ? PRODUCT_CL3B : PRODUCT_R1EC;
        s->revision = i < 9 ? 1 : 0x22092010u;
        s->kind = i < 7 ? ECAT_KIND_SERVO : i < 9 ? ECAT_KIND_STEPPER : ECAT_KIND_COUPLER;
        s->rack = i < 9 ? ECAT_TOPO_NO_RACK : 0;
    }
    t->n_slaves = 10;
    t->n_racks = 1;
    t->rack[0].n_modules = 3;
    memcpy(t->rack[0].ident, s_rack, sizeof(s_rack));
    ecat_topology_assign_names(t, NULL, NULL);

    /* IO layout as ecat_io.c builds it from the catalog; Device Status 0xF100:01 comes first */
    ecat_io_layout_t *l = &s_layout;
    memset(l, 0, sizeof(*l));
    l->gen = 1;
    l->in_op = true;
    l->n = 1;
    ecat_io_coupler_t *c = &l->c[0];
    strlcpy(c->name, t->slave[9].name, sizeof(c->name));
    c->slave = 10;
    uint16_t in_bit = 32, out_bit = 0;
    for (int m = 0; m < 3; m++) {
        const ecat_module_info_t *mi = ecat_catalog_module(PRODUCT_R1EC, s_rack[m]);
        ecat_io_module_t *im = &c->mod[m];
        im->ident = s_rack[m];
        strlcpy(im->name, mi ? mi->name : "?", sizeof(im->name));
        im->slot = (uint8_t)m;
        im->di = mi ? mi->di : 0;
        im->dout = mi ? mi->dout : 0;
        im->in_bits = mi ? mi->in_bits : 0;
        im->out_bits = mi ? mi->out_bits : 0;
        im->in_bit = in_bit;
        im->out_bit = out_bit;
        im->x_first = l->n_di;
        im->y_first = l->n_do;
        in_bit += im->in_bits;
        out_bit += im->out_bits;
        l->n_di += im->di;
        l->n_do += im->dout;
    }
    c->n_modules = 3;
    c->in_bytes = (uint16_t)((in_bit + 7) / 8);
    c->out_bytes = (uint16_t)((out_bit + 7) / 8);
}

uint32_t ecat_busview_gen(void)
{
    return 1;
}

void ecat_busview_read(ecat_busview_t *out)
{
    memset(out, 0, sizeof(*out));
    out->gen = 1;
    out->scan_gen = 1;
    out->phase = ECAT_BUS_PHASE_OP;
    out->have_saved = true;
    out->cmp_valid = true;
    out->meta.saved_at = (int64_t)time(NULL) - 3 * 3600;
    strlcpy(out->meta.saved_by, "HMI", sizeof(out->meta.saved_by));
    out->saved = s_topo;
    out->bus = s_topo;
    ecat_topology_compare(&out->saved, &out->bus, &out->cmp);
}

/* ---- Axes: AX1 and AX2 servo on; a two-axis path runs at its speed (no ramps) with a small
 * made-up contour error, so the Interpolation page draws a trace. ---- */

static struct {
    ecat_path_status_t st;
    ecat_path_cmd_t cmd;
    float r, th0, sweep;
    uint32_t t0;
    int32_t pos[2]; /* AX1, AX2 */
} s_path = {.st = {.err_axis = 0xFF}, .pos = {-154220, -28790}};

void ecat_axes_read(ecat_axes_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->in_op = true;
    out->bus_ok = true;
    out->n_axes = 9;
    if (s_path.st.phase != ECAT_PATH_IDLE) {
        ecat_path_status_t *p = &s_path.st;
        const float t = (float)lv_tick_elaps(s_path.t0) / 1000.0f;
        float s = s_path.cmd.speed * t;
        s = s < p->length ? s : p->length;
        float x, y;
        const float wobble = 0.004f * sinf(s * 3.0f); /* 4 um of contour error */
        if (s_path.cmd.kind == ECAT_PATH_LINE) {
            const float f = s / p->length;
            x = s_path.cmd.end[0] * f - wobble * s_path.cmd.end[1] / p->length;
            y = s_path.cmd.end[1] * f + wobble * s_path.cmd.end[0] / p->length;
        } else {
            const float th = s_path.th0 + s_path.sweep * s / p->length;
            x = s_path.cmd.center[0] + (s_path.r + wobble) * cosf(th);
            y = s_path.cmd.center[1] + (s_path.r + wobble) * sinf(th);
        }
        for (int k = 0; k < 2; k++) {
            s_path.pos[k] = p->start[k] + (int32_t)lroundf((k ? y : x) * s_path.cmd.k[k]);
        }
        p->s = s;
        p->speed = s < p->length ? s_path.cmd.speed : 0.0f;
        p->contour = fabsf(wobble);
        p->contour_max = fmaxf(p->contour_max, p->contour);
        p->lag_max = s_path.cmd.speed * 0.003f;
        p->time_ms = (uint32_t)(t * 1000.0f);
        if (s >= p->length) {
            p->phase = ECAT_PATH_IDLE;
            p->done = true;
        }
    }
    out->path = s_path.st;
    for (int i = 0; i < 9; i++) {
        ecat_axis_status_t *a = &out->axis[i];
        const bool on = i < 2;
        strlcpy(a->name, s_topo.slave[i].name, sizeof(a->name));
        a->slave = (uint16_t)(i + 1);
        a->kind = s_topo.slave[i].kind;
        a->available = true;
        a->enabled = on;
        a->homed = i == 0;
        a->cia_state = on ? ECAT_CIA_OP_ENABLED : ECAT_CIA_READY;
        a->job = on && s_path.st.phase != ECAT_PATH_IDLE ? ECAT_JOB_PATH : ECAT_JOB_IDLE;
        a->mode_display = 1;
        a->statusword = on ? 0x1637 : 0x0631;
        a->controlword = on ? 0x000F : 0x0006;
        a->position = on ? s_path.pos[i] : (int32_t)(i * 125430 - 154220);
        a->units_per_rev = 10000;
        a->home_method = 23;
        a->home_method_valid = true;
    }
}

const char *ecat_path_check(const ecat_path_cmd_t *p, float *length)
{
    /* The geometry of ecat_axis.c, without its tolerance checks. */
    float len;
    if (p->axis[0] == p->axis[1]) {
        return "Pick two different axes";
    }
    if (p->kind == ECAT_PATH_LINE) {
        len = hypotf(p->end[0], p->end[1]);
    } else {
        const float r = hypotf(p->center[0], p->center[1]);
        float sweep = atan2f(p->end[1] - p->center[1], p->end[0] - p->center[0]) - atan2f(-p->center[1], -p->center[0]);
        const bool full = hypotf(p->end[0], p->end[1]) * p->k[0] < 1.0f;
        if (p->kind == ECAT_PATH_ARC_CCW) {
            while (sweep <= 0.0f) {
                sweep += 6.2831853f;
            }
            sweep = full ? 6.2831853f : sweep;
        } else {
            while (sweep >= 0.0f) {
                sweep -= 6.2831853f;
            }
            sweep = full ? -6.2831853f : sweep;
        }
        len = fabsf(sweep) * r;
    }
    if (length) {
        *length = len;
    }
    return len > 0.0f ? NULL : "The path is shorter than one position unit";
}

bool ecat_path_start(const ecat_path_cmd_t *p)
{
    float len;
    if (ecat_path_check(p, &len) || s_path.st.phase != ECAT_PATH_IDLE) {
        return true;
    }
    const uint32_t gen = s_path.st.gen + 1;
    s_path.cmd = *p;
    memset(&s_path.st, 0, sizeof(s_path.st));
    s_path.st.gen = gen;
    s_path.st.phase = ECAT_PATH_RUNNING;
    s_path.st.kind = p->kind;
    s_path.st.err_axis = 0xFF;
    s_path.st.length = len;
    for (int k = 0; k < 2; k++) {
        s_path.st.axis[k] = p->axis[k];
        s_path.st.k[k] = p->k[k];
        s_path.st.start[k] = s_path.pos[k];
    }
    s_path.r = hypotf(p->center[0], p->center[1]);
    s_path.th0 = atan2f(-p->center[1], -p->center[0]);
    s_path.sweep = (p->kind == ECAT_PATH_ARC_CCW ? 1.0f : -1.0f) * (s_path.r > 0.0f ? len / s_path.r : 0.0f);
    s_path.t0 = lv_tick_get();
    return true;
}

void ecat_path_stop(void)
{
    s_path.st.phase = ECAT_PATH_IDLE;
}

const char *ecat_path_phase_text(uint8_t phase)
{
    static const char *const t[] = {"Idle", "Switching to CSP", "Running", "Stopping", "Settling"};
    return phase < 5 ? t[phase] : "?";
}

const char *ecat_axis_error_text(uint8_t err)
{
    static const char *const t[] = {"No error",     "Not available",          "EtherCAT errors",
                                    "Drive fault",  "Enable timed out",       "Servo is off",
                                    "Busy",         "Mode change timed out",  "Set-point not acknowledged",
                                    "Homing error", "Homing timed out",       "Reset failed",
                                    "Jog dead-man time-out", "Left Operation enabled",
                                    "Path not valid", "Path stopped: the other axis stopped"};
    return err < sizeof(t) / sizeof(t[0]) ? t[err] : "?";
}

const char *ecat_cia_state_text(uint8_t state)
{
    static const char *const t[] = {"Not ready",         "Switch on disabled", "Ready to switch on", "Switched on",
                                    "Operation enabled", "Quick stop",         "Fault reaction",     "Fault"};
    return state < 8 ? t[state] : "?";
}

const char *ecat_axis_job_text(uint8_t job)
{
    static const char *const t[] = {"Idle",     "Enabling", "Resetting",  "Moving",       "Jogging",
                                    "Homing",   "Stopping", "Quick stop", "Interpolating"};
    return job < 9 ? t[job] : "?";
}

bool ecat_axis_command(const ecat_axis_cmd_t *cmd)
{
    (void)cmd;
    return true;
}

uint32_t ecat_drive_units_per_rev(const ecat_drive_info_t *d)
{
    (void)d;
    return 10000;
}

uint32_t ecat_io_layout_gen(void)
{
    return s_layout.gen;
}

void ecat_io_layout_read(ecat_io_layout_t *out)
{
    *out = s_layout;
}

void ecat_io_image_read(ecat_io_image_t *out)
{
    static const uint8_t in[] = {0, 0, 0, 0, 0xA5, 0x13, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00};
    memset(out, 0, sizeof(*out));
    out->gen = s_layout.gen;
    out->in_op = true;
    out->bus_ok = true;
    memcpy(out->in[0], in, sizeof(in));
    out->out[0][0] = 0x05;
}

bool ecat_io_force_mode(bool on)
{
    (void)on;
    return true;
}

bool ecat_io_force_bit(uint8_t coupler, uint16_t out_bit, bool value)
{
    (void)coupler;
    (void)out_bit;
    (void)value;
    return true;
}

static ecat_sdo_job_t s_job;

bool ecat_sdo_submit(const ecat_sdo_job_t *job, uint32_t *ticket)
{
    s_job = *job;
    for (int i = 0; i < s_job.n; i++) {
        ecat_sdo_item_t *it = &s_job.item[i];
        it->ok = true;
        if (!it->write) {
            it->value = it->index == 0x6092 && it->sub == 1 ? 10000 : 1000;
        }
    }
    *ticket = 1;
    return true;
}

ecat_sdo_state_t ecat_sdo_poll(uint32_t ticket, ecat_sdo_job_t *out)
{
    (void)ticket;
    *out = s_job;
    return ECAT_SDO_DONE;
}

bool hmi_bus_request(hmi_bus_req_t req)
{
    (void)req;
    return true;
}

bool hmi_bus_status_read(hmi_bus_status_t *out)
{
    *out = (hmi_bus_status_t){.state = HMI_BUS_OP, .slaves = 10, .cycle_us = 1000, .jitter_us = 1,
                              .rtt_avg_us = 125, .rtt_max_us = 141};
    return true;
}

bool hmi_cpu_get(hmi_cpu_t *out)
{
    *out = (hmi_cpu_t){.load = {23, 41}, .fps = 60};
    return true;
}

/* ---- Wi-Fi and NTP ---- */

static net_status_t s_net = {
    .wifi_on = true, .ssid = "OpenWrt", .ntp_on = true, .ntp_server = "pool.ntp.org",
    .state = NET_WIFI_CONNECTED, .rssi = -58, .ip = {1, 2, 1, 113}, .scan_gen = 1,
};
static const net_ap_t s_aps[] = {
    {"OpenWrt", -58, true}, {"Factory-Office", -66, true}, {"Line2-AP", -74, true}, {"Guest", -81, false},
};

void net_status(net_status_t *out)
{
    *out = s_net;
    out->last_sync = time(NULL) - 120;
}

int net_scan_results(net_ap_t *out, int max)
{
    const int n = (int)(sizeof(s_aps) / sizeof(s_aps[0])) < max ? (int)(sizeof(s_aps) / sizeof(s_aps[0])) : max;
    memcpy(out, s_aps, sizeof(s_aps[0]) * (size_t)n);
    return n;
}

esp_err_t net_set_wifi(bool on)
{
    s_net.wifi_on = on;
    return ESP_OK;
}

esp_err_t net_join(const char *ssid, const char *password)
{
    (void)password;
    strlcpy(s_net.ssid, ssid, sizeof(s_net.ssid));
    return ESP_OK;
}

esp_err_t net_forget(void)
{
    s_net.ssid[0] = '\0';
    return ESP_OK;
}

esp_err_t net_scan(void)
{
    return ESP_OK;
}

esp_err_t net_set_ntp(bool on, const char *server)
{
    s_net.ntp_on = on;
    if (server) {
        strlcpy(s_net.ntp_server, server, sizeof(s_net.ntp_server));
    }
    return ESP_OK;
}

esp_err_t net_ntp_sync_now(void)
{
    return ESP_OK;
}

const char *net_state_text(net_wifi_state_t s)
{
    static const char *const t[] = {"Off",        "Starting",  "Not available",          "Not connected",
                                    "Connecting", "Connected", "Not connected, retrying"};
    return (unsigned)s < 7 ? t[s] : "?";
}

/* ---- Camera ---- */

static camera_status_t s_cam = {
    .on = true, .res = CAMERA_RES_1280X720, .state = CAMERA_STREAMING, .sensor = "OV02C10", .width = 1280, .height = 720, .port = 80,
    .max_fps = 5, .viewers = 1, .fps_x10 = 50, .kbit_s = 1350, .frame_bytes = 33 * 1024, .frames = 18342,
};

void camera_status(camera_status_t *out)
{
    *out = s_cam;
}

esp_err_t camera_set_on(bool on)
{
    s_cam.on = on;
    s_cam.state = on ? CAMERA_STREAMING : CAMERA_OFF;
    return ESP_OK;
}

esp_err_t camera_set_res(camera_res_t res)
{
    s_cam.res = res;
    camera_res_size(res, &s_cam.width, &s_cam.height);
    return ESP_OK;
}

void camera_res_size(camera_res_t res, uint16_t *w, uint16_t *h)
{
    static const uint16_t size[][2] = {{1920, 1080}, {1280, 720}, {1024, 576}, {720, 480}, {640, 360}};
    *w = size[(unsigned)res < 5 ? res : 0][0];
    *h = size[(unsigned)res < 5 ? res : 0][1];
}

const char *camera_state_text(camera_state_t s)
{
    static const char *const t[] = {"Off", "Starting", "Not available", "Waiting for Wi-Fi", "Ready", "Streaming"};
    return (unsigned)s < 6 ? t[s] : "?";
}

/* ---- Start-up ---- */

void sim_fake_init(void)
{
    static const machine_alarm_spec_t a101 = {
        "A101", MACHINE_ALARM_ERROR, "Servo AX1 drive fault", "Slave 1 \xC2\xB7 AX1 \xC2\xB7 drive error 0x821B",
        "The drive reports error 0x821B (vendor code 0x081B).", "Remove the cause, then press Reset."};
    static const machine_alarm_spec_t a108 = {
        "A108", MACHINE_ALARM_ERROR, "Servo AX8 drive fault", "Slave 8 \xC2\xB7 AX8 \xC2\xB7 drive error 0x821B",
        "The drive reports error 0x821B.", "Remove the cause, then press Reset."};
    static const machine_alarm_spec_t w001 = {"W001", MACHINE_ALARM_WARNING, "Clock not set", "RTC RX8025T",
                                              "The RTC lost its time (backup cell empty or removed).",
                                              "Set the date and time in Settings \xE2\x80\xBA Time."};
    topo_build();
    machine_alarm_init();
    machine_timers_init();
    machine_alarm_update(&a101, true);
    machine_alarm_update(&a108, true);
    machine_alarm_update(&w001, true);
}
