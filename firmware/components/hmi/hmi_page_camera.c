/*
 * Settings > Camera (no mockup; built like Settings > Network): the camera switch, its state, the
 * picture size and the local stream addresses on the left; on the right the push to the server
 * (go2rtc on the NAS, reached through DDNS): switch, state, address and login.
 *
 * The camera runs in its own tasks (components/camera); this page only changes settings (they return
 * at once) and follows camera_status(), camera_push_get() and the Wi-Fi address in its poll. The
 * address, user and password are typed on the on-screen keyboard (hmi_text_input).
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
#define PUSH_LABEL_W 84
#define DASH    "\xE2\x80\x94"

/* Same order as camera_res_t */
static const char s_res_opts[] = "1920 x 1080\n1280 x 720\n1024 x 576\n720 x 480 (sides cut)\n640 x 360";

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *sw, *tag, *detail, *res_val;
    lv_obj_t *stream_val, *snap_val, *picture_val, *rate_val, *data_val;
    lv_obj_t *push_sw, *push_tag, *push_detail, *push_msg, *url_val, *user_val, *pass_val;
    lv_timer_t *poll;
    camera_status_t st;
    camera_push_cfg_t push;
    char error[96]; /* last refused entry, shown until the state changes */
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

/* ---- Push card ---- */

static void push_toggle_cb(lv_event_t *e)
{
    (void)e;
    const bool on = !s_cam.push.on;
    s_cam.push.on = on;
    hmi_switch_set(s_cam.push_sw, on);
    (void)camera_push_set_on(on);
}

static void push_result(esp_err_t err)
{
    snprintf(s_cam.error, sizeof(s_cam.error), "%s",
             err == ESP_OK                ? ""
             : err == ESP_ERR_INVALID_ARG ? "Refused: the address must start with http:// or https://, and fit."
                                          : esp_err_to_name(err));
}

static void url_set_cb(const char *text, void *user)
{
    (void)user;
    if (text) {
        push_result(camera_push_set(text, NULL, NULL));
    }
}

static void user_set_cb(const char *text, void *user)
{
    (void)user;
    if (text) {
        push_result(camera_push_set(NULL, text, NULL));
    }
}

static void pass_set_cb(const char *text, void *user)
{
    (void)user;
    if (text) {
        push_result(camera_push_set(NULL, NULL, text));
    }
}

static void url_cb(lv_event_t *e)
{
    (void)e;
    hmi_text_input("Server address",
                   "Push address of go2rtc on the server, e.g. https://home.example.net:8555/api/stream.mjpeg?dst=ehmi",
                   s_cam.push.url, false, 0, CAMERA_PUSH_URL_MAX, url_set_cb, NULL);
}

static void user_cb(lv_event_t *e)
{
    (void)e;
    hmi_text_input("User", "Login of that go2rtc (api: username). Leave it empty if it has none.", s_cam.push.user,
                   false, 0, CAMERA_PUSH_USER_MAX, user_set_cb, NULL);
}

static void pass_cb(lv_event_t *e)
{
    (void)e;
    hmi_text_input("Password", "Password of that login (api: password). Leave it empty if it has none.", "", true, 0,
                   CAMERA_PUSH_PASS_MAX, pass_set_cb, NULL);
}

/* Name and a value that opens the keyboard. @return the value label */
static lv_obj_t *push_field(lv_obj_t *card, const char *name, lv_event_cb_t cb)
{
    lv_obj_t *r = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(hmi_label(r, name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT), PUSH_LABEL_W);
    lv_obj_t *b = hmi_control(r, 0, HMI_TOUCH_H);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_t *v = dots_label(b, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_flex_grow(v, 1);
    hmi_icon(b, HMI_ICON_CHEVRON_RIGHT, 16, HMI_TEXT_2);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return v;
}

static void push_card(lv_obj_t *body)
{
    lv_obj_t *card = hmi_card(body, 10, 16, 10);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_size(card, RIGHT_W, LV_PCT(100));

    lv_obj_t *top = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_size(top, LV_PCT(100), 40);
    lv_obj_add_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(top, push_toggle_cb, LV_EVENT_CLICKED, NULL);
    hmi_icon(top, HMI_ICON_UPLOAD, 20, HMI_TEXT);
    hmi_label(top, "Send to server", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_spacer(top);
    s_cam.push_sw = hmi_switch(top, s_cam.push.on);

    lv_obj_t *status = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_cam.push_tag = hmi_tag(status, true);
    s_cam.push_detail = dots_label(status, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_flex_grow(s_cam.push_detail, 1);
    s_cam.push_msg = hmi_label(card, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(s_cam.push_msg, LV_PCT(100));

    s_cam.url_val = push_field(card, "Server", url_cb);
    s_cam.user_val = push_field(card, "User", user_cb);
    s_cam.pass_val = push_field(card, "Password", pass_cb);

    hmi_spacer(card);
    lv_obj_t *n = hmi_label(card,
                            "Sends the picture to go2rtc on the NAS over the Internet (DDNS), which records it. "
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
}

static void push_show(void)
{
    const camera_push_cfg_t *c = &s_cam.push;
    const camera_status_t *st = &s_cam.st;
    hmi_switch_set(s_cam.push_sw, c->on);
    hmi_set_text(s_cam.url_val, c->url[0] ? c->url : "Not set");
    hmi_set_text(s_cam.user_val, c->user[0] ? c->user : "None");
    hmi_set_text(s_cam.pass_val, c->has_password ? "Set" : "None");

    char d[32] = "";
    uint32_t bg = HMI_WHITE, fg = HMI_TEXT_2;
    switch (st->push) {
    case CAMERA_PUSH_OFF:
        break;
    case CAMERA_PUSH_WAITING:
    case CAMERA_PUSH_CONNECTING:
        bg = HMI_CAUTION_BG;
        fg = HMI_CAUTION;
        break;
    case CAMERA_PUSH_SENDING:
        snprintf(d, sizeof(d), "%lu.%lu Mbit/s", (unsigned long)(st->push_kbit_s / 1000),
                 (unsigned long)(st->push_kbit_s % 1000 / 100));
        bg = HMI_SUCCESS_BG;
        fg = HMI_SUCCESS;
        break;
    case CAMERA_PUSH_RETRY:
        bg = HMI_CRITICAL_BG;
        fg = HMI_CRITICAL;
        break;
    }
    hmi_tag_set(s_cam.push_tag, camera_push_state_text(st->push), bg, fg);
    hmi_set_text(s_cam.push_detail, d);

    char m[CAMERA_REASON_MAX + 40];
    if (s_cam.error[0]) {
        snprintf(m, sizeof(m), "%s", s_cam.error);
    } else if (st->push == CAMERA_PUSH_RETRY) {
        snprintf(m, sizeof(m), "%s. Trying again.", st->push_reason);
    } else {
        snprintf(m, sizeof(m), "%s", st->push_reason);
    }
    hmi_set_text(s_cam.push_msg, m);
    lv_obj_set_style_text_color(s_cam.push_msg, HMI_C(s_cam.error[0] || st->push == CAMERA_PUSH_RETRY ? HMI_CRITICAL
                                                                                                      : HMI_TEXT_2),
                                0);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    const camera_push_state_t was = s_cam.st.push;
    camera_status(&s_cam.st);
    camera_push_get(&s_cam.push);
    if (t && s_cam.st.push != was) {
        s_cam.error[0] = '\0'; /* a new state replaces an old refusal */
    }
    net_status_t nw;
    net_status(&nw);
    hmi_switch_set(s_cam.sw, s_cam.st.on);
    res_show();
    status_show(&nw);
    values_show(&nw);
    push_show();
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
    camera_push_get(&s_cam.push);

    lv_obj_t *body = hmi_box(root, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_add_event_cb(body, deleted_cb, LV_EVENT_DELETE, NULL);
    camera_card(body);
    push_card(body);
    poll_cb(NULL);
    s_cam.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
