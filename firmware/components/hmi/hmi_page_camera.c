/*
 * Settings > Camera (no mockup; built like Settings > Network): the camera switch, its state, the
 * picture size and the stream addresses on the left, how the server records it on the right.
 *
 * The camera runs in its own task (components/camera); this page only switches it (camera_set_on,
 * returns at once) and follows camera_status() and the Wi-Fi address in its poll.
 */

#include <stdio.h>
#include <string.h>
#include "camera.h"
#include "esp_attr.h"
#include "hmi_internal.h"
#include "net.h"

#define POLL_MS 500
#define RIGHT_W 330
#define LABEL_W 120
#define DASH    "\xE2\x80\x94"

/* Same order as camera_res_t */
static const char s_res_opts[] = "1920 x 1080\n1280 x 720\n1024 x 576\n720 x 480 (sides cut)\n640 x 360";

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *sw, *tag, *detail, *res_val;
    lv_obj_t *stream_val, *snap_val, *picture_val, *rate_val, *data_val;
    lv_obj_t *server_line;
    lv_timer_t *poll;
    camera_status_t st;
} s_cam;

/* One-line label that ends in "..." when it does not fit. */
static lv_obj_t *dots_label(lv_obj_t *parent, const char *text, int px, hmi_weight_t w, uint32_t color)
{
    lv_obj_t *l = hmi_label(parent, text, px, w, color);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(hmi_font(px, w)));
    return l;
}

static lv_obj_t *info_row(lv_obj_t *card, const char *name)
{
    lv_obj_t *r = hmi_box(card, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), 36);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_divider_top(r);
    lv_obj_set_width(hmi_label(r, name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT), LABEL_W);
    lv_obj_t *v = dots_label(r, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_flex_grow(v, 1);
    return v;
}

static void toggle_cb(lv_event_t *e)
{
    (void)e;
    const bool on = !s_cam.st.on;
    s_cam.st.on = on; /* shown at once; the poll follows the camera task */
    hmi_switch_set(s_cam.sw, on);
    (void)camera_set_on(on);
}

static void res_show(void)
{
    uint16_t w, h;
    camera_res_size(s_cam.st.res, &w, &h);
    char t[24];
    snprintf(t, sizeof(t), "%u x %u", w, h);
    hmi_set_text(s_cam.res_val, t);
}

static void res_set_cb(int index, void *user)
{
    (void)user;
    if (index < 0 || index >= CAMERA_RES_COUNT) {
        return;
    }
    (void)camera_set_res((camera_res_t)index);
    if (s_cam.res_val) { /* the page may have closed meanwhile */
        s_cam.st.res = (camera_res_t)index;
        res_show();
    }
}

static void res_cb(lv_event_t *e)
{
    (void)e;
    hmi_choice("Picture size", s_res_opts, s_cam.st.res, res_set_cb, NULL);
}

/* Name and a combo (as Dim after on the Display page). */
static void res_field(lv_obj_t *card)
{
    lv_obj_t *r = hmi_box(card, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_style_pad_bottom(r, 4, 0);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(hmi_label(r, "Picture size", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT), LABEL_W);
    lv_obj_t *b = hmi_control(r, 0, HMI_TOUCH_H);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(b, 12, 0);
    s_cam.res_val = hmi_label(b, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    hmi_icon(b, HMI_ICON_CHEVRON_DOWN, 16, HMI_TEXT_2);
    lv_obj_add_event_cb(b, res_cb, LV_EVENT_CLICKED, NULL);
}

/* ---- Camera card ---- */

static void camera_card(lv_obj_t *body)
{
    lv_obj_t *card = hmi_card(body, 10, 16, 8);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);

    lv_obj_t *top = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_size(top, LV_PCT(100), 40);
    lv_obj_add_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(top, toggle_cb, LV_EVENT_CLICKED, NULL);
    hmi_icon(top, HMI_ICON_CAMERA, 20, HMI_TEXT);
    hmi_label(top, "Camera", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_spacer(top);
    s_cam.sw = hmi_switch(top, s_cam.st.on);

    lv_obj_t *status = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_set_style_pad_bottom(status, 6, 0);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_cam.tag = hmi_tag(status, true);
    s_cam.detail = dots_label(status, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_flex_grow(s_cam.detail, 1);

    res_field(card);

    s_cam.stream_val = info_row(card, "Stream");
    s_cam.snap_val = info_row(card, "Snapshot");
    s_cam.picture_val = info_row(card, "Picture");
    s_cam.rate_val = info_row(card, "Frame rate");
    s_cam.data_val = info_row(card, "Data");

    hmi_spacer(card);
    lv_obj_t *note = hmi_label(card,
                               "The sensor only runs while someone watches. A smaller picture needs less Wi-Fi; "
                               "choosing a size restarts the stream.",
                               HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(note, LV_PCT(100));
}

/* ---- Server card ---- */

static void server_card(lv_obj_t *body)
{
    lv_obj_t *card = hmi_card(body, 10, 16, 10);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_size(card, RIGHT_W, LV_PCT(100));

    lv_obj_t *top = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_size(top, LV_PCT(100), 40);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_icon(top, HMI_ICON_MONITOR, 20, HMI_TEXT);
    hmi_label(top, "Recording on the server", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);

    lv_obj_t *t = hmi_label(card,
                            "go2rtc on the TrueNAS server takes the stream, records it and serves it to every "
                            "viewer, so the HMI sends it only once. Its stream line:",
                            HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(t, LV_PCT(100));

    lv_obj_t *box = hmi_box(card, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, HMI_TINT_OPA, 0);
    s_cam.server_line = hmi_label(box, "", HMI_FS_SMALL, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_set_width(s_cam.server_line, LV_PCT(100));
    lv_label_set_long_mode(s_cam.server_line, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *n = hmi_label(card,
                            "Give the HMI a fixed address in the router (DHCP reservation), so the line stays valid. "
                            "Setup: docs/CAMERA.md.",
                            HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(n, LV_PCT(100));
}

/* ---- Poll ---- */

static void status_show(const net_status_t *nw)
{
    const camera_status_t *st = &s_cam.st;
    char d[96];
    uint32_t bg = HMI_WHITE, fg = HMI_TEXT_2;
    switch (st->state) {
    case CAMERA_OFF:
        snprintf(d, sizeof(d), "Switch the camera on to send its picture.");
        break;
    case CAMERA_STARTING:
        snprintf(d, sizeof(d), "Finding the camera...");
        bg = HMI_CAUTION_BG;
        fg = HMI_CAUTION;
        break;
    case CAMERA_FAULT:
        snprintf(d, sizeof(d), "%s", st->reason);
        bg = HMI_CRITICAL_BG;
        fg = HMI_CRITICAL;
        break;
    case CAMERA_WAIT_WIFI:
        snprintf(d, sizeof(d), "%s",
                 nw->wifi_on ? "Wi-Fi is not connected yet." : "Switch Wi-Fi on in Settings \xE2\x80\xBA Network.");
        bg = HMI_CAUTION_BG;
        fg = HMI_CAUTION;
        break;
    case CAMERA_READY:
        snprintf(d, sizeof(d), "Waiting for a viewer; the sensor is stopped.");
        bg = HMI_SUCCESS_BG;
        fg = HMI_SUCCESS;
        break;
    case CAMERA_STREAMING:
        snprintf(d, sizeof(d), "%u viewer%s", st->viewers, st->viewers == 1 ? "" : "s");
        bg = HMI_SUCCESS_BG;
        fg = HMI_SUCCESS;
        break;
    }
    hmi_tag_set(s_cam.tag, camera_state_text(st->state), bg, fg);
    hmi_set_text(s_cam.detail, d);
}

static void values_show(const net_status_t *nw)
{
    const camera_status_t *st = &s_cam.st;
    const bool serving = st->state == CAMERA_READY || st->state == CAMERA_STREAMING;
    char host[32], t[96];
    if (nw->state == NET_WIFI_CONNECTED) {
        if (st->port == 80) {
            snprintf(host, sizeof(host), "%u.%u.%u.%u", nw->ip[0], nw->ip[1], nw->ip[2], nw->ip[3]);
        } else {
            snprintf(host, sizeof(host), "%u.%u.%u.%u:%u", nw->ip[0], nw->ip[1], nw->ip[2], nw->ip[3], st->port);
        }
    } else {
        snprintf(host, sizeof(host), "<HMI address>");
    }

    snprintf(t, sizeof(t), "http://%s/stream", host);
    hmi_set_text(s_cam.stream_val, serving ? t : DASH);
    snprintf(t, sizeof(t), "http://%s/snapshot.jpg", host);
    hmi_set_text(s_cam.snap_val, serving ? t : DASH);

    if (st->sensor[0]) {
        snprintf(t, sizeof(t), "%s \xC2\xB7 JPEG quality %d", st->sensor, CONFIG_CAMERA_JPEG_QUALITY);
    } else {
        snprintf(t, sizeof(t), DASH);
    }
    hmi_set_text(s_cam.picture_val, t);

    if (st->state == CAMERA_STREAMING) {
        snprintf(t, sizeof(t), "%u.%u frames/s (at most %u)", st->fps_x10 / 10, st->fps_x10 % 10, st->max_fps);
    } else {
        snprintf(t, sizeof(t), "At most %u frames/s", st->max_fps);
    }
    hmi_set_text(s_cam.rate_val, t);

    if (st->state == CAMERA_STREAMING) {
        snprintf(t, sizeof(t), "%lu.%lu Mbit/s \xC2\xB7 %lu KB a frame", (unsigned long)(st->kbit_s / 1000),
                 (unsigned long)(st->kbit_s % 1000 / 100), (unsigned long)(st->frame_bytes / 1024));
    } else {
        snprintf(t, sizeof(t), DASH);
    }
    hmi_set_text(s_cam.data_val, t);

    snprintf(t, sizeof(t), "ehmi: http://%s/stream", host);
    hmi_set_text(s_cam.server_line, t);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    camera_status(&s_cam.st);
    net_status_t nw;
    net_status(&nw);
    hmi_switch_set(s_cam.sw, s_cam.st.on);
    res_show();
    status_show(&nw);
    values_show(&nw);
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_cam.poll) {
        lv_timer_delete(s_cam.poll);
    }
    memset(&s_cam, 0, sizeof(s_cam));
}

void hmi_camera_create(lv_obj_t *root, lv_obj_t *head)
{
    (void)head;
    memset(&s_cam, 0, sizeof(s_cam));
    camera_status(&s_cam.st);

    lv_obj_t *body = hmi_box(root, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_add_event_cb(body, deleted_cb, LV_EVENT_DELETE, NULL);
    camera_card(body);
    server_card(body);
    poll_cb(NULL);
    s_cam.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
