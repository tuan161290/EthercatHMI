/*
 * Settings > Network (no mockup; built like Settings > Time): Wi-Fi on the left, internet time
 * on the right.
 *
 * Wi-Fi goes through the ESP32-C6 (components/net). This page only posts requests (net.h, they
 * return at once) and follows net_status() in its poll: scanning, joining and the NTP sync run in
 * the net task. Opening the page with Wi-Fi on starts a scan. Tapping a network asks for its
 * password on the on-screen keyboard (hmi_text_input); an open network asks for a confirmation.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "hmi_internal.h"
#include "machine_clock.h"
#include "net.h"

#define POLL_MS  500
#define RIGHT_W  330
#define ROW_H    56
#define LABEL_W  92
#define SIG_W    132

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *wifi_sw, *wifi_tag, *wifi_detail;
    lv_obj_t *scan_btn, *scan_label, *list, *forget_btn, *message;
    lv_obj_t *ntp_sw, *server_val, *sync_val, *clock_val, *ntp_err;
    lv_timer_t *poll;
    net_status_t st;
    uint32_t list_gen; /* what the list was built for */
    char list_ssid[NET_SSID_MAX + 1];
    bool list_connected, list_wifi_on, list_built;
    bool scan_asked; /* scanned once since Wi-Fi came up on this visit */
    net_ap_t aps[NET_SCAN_MAX];
    char pending_ssid[NET_SSID_MAX + 1]; /* network of the open password / confirm dialog */
} s_nw;

static void message(const char *text, bool error)
{
    if (s_nw.message) {
        lv_label_set_text(s_nw.message, text);
        lv_obj_set_style_text_color(s_nw.message, HMI_C(error ? HMI_CRITICAL : HMI_TEXT_2), 0);
    }
}

static const char *signal_word(int rssi)
{
    return rssi >= -55 ? "Excellent" : rssi >= -67 ? "Good" : rssi >= -75 ? "Fair" : "Weak";
}

/* One-line label that ends in "..." when it does not fit. */
static lv_obj_t *dots_label(lv_obj_t *parent, const char *text, int px, hmi_weight_t w, uint32_t color)
{
    lv_obj_t *l = hmi_label(parent, text, px, w, color);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(hmi_font(px, w)));
    return l;
}

/* ---- Joining ---- */

static void join_result(esp_err_t err, const char *ssid)
{
    char t[96];
    if (err == ESP_OK) {
        snprintf(t, sizeof(t), "Connecting to %s...", ssid);
        message(t, false);
    } else if (err == ESP_ERR_INVALID_ARG) {
        message("The password must have 8 to 63 characters (or none for an open network).", true);
    } else {
        snprintf(t, sizeof(t), "Could not connect: %s", esp_err_to_name(err));
        message(t, true);
    }
}

static void password_cb(const char *text, void *user)
{
    (void)user;
    if (text) {
        join_result(net_join(s_nw.pending_ssid, text), s_nw.pending_ssid);
    }
}

static void open_confirm_cb(bool ok, void *user)
{
    (void)user;
    if (ok) {
        join_result(net_join(s_nw.pending_ssid, NULL), s_nw.pending_ssid);
    }
}

static void ask_password(const char *ssid)
{
    char title[64];
    snprintf(title, sizeof(title), "Connect to %s", ssid);
    hmi_text_input(title, "Password of the network (8 to 63 characters)", "", true, 8, NET_PASS_MAX, password_cb,
                   NULL);
}

static void row_cb(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0) { /* the saved network, not in the last scan */
        strlcpy(s_nw.pending_ssid, s_nw.st.ssid, sizeof(s_nw.pending_ssid));
        ask_password(s_nw.pending_ssid);
        return;
    }
    strlcpy(s_nw.pending_ssid, s_nw.aps[i].ssid, sizeof(s_nw.pending_ssid));
    if (s_nw.aps[i].secured) {
        ask_password(s_nw.pending_ssid);
    } else {
        char title[64];
        snprintf(title, sizeof(title), "Connect to %s?", s_nw.pending_ssid);
        hmi_confirm(title, "This network is open: it has no password and its traffic is not encrypted.", "Connect",
                    false, open_confirm_cb, NULL);
    }
}

static void hidden_password_cb(const char *text, void *user)
{
    (void)user;
    if (text) {
        join_result(net_join(s_nw.pending_ssid, text), s_nw.pending_ssid);
    }
}

static void hidden_ssid_cb(const char *text, void *user)
{
    (void)user;
    if (text == NULL) {
        return;
    }
    strlcpy(s_nw.pending_ssid, text, sizeof(s_nw.pending_ssid));
    char title[64];
    snprintf(title, sizeof(title), "Password for %s", s_nw.pending_ssid);
    hmi_text_input(title, "Leave it empty for an open network.", "", true, 0, NET_PASS_MAX, hidden_password_cb, NULL);
}

static void hidden_cb(lv_event_t *e)
{
    (void)e;
    hmi_text_input("Hidden network", "Name of the network (SSID), exactly as set in the access point.", "", false,
                   1, NET_SSID_MAX, hidden_ssid_cb, NULL);
}

static void forget_confirm_cb(bool ok, void *user)
{
    (void)user;
    if (ok) {
        (void)net_forget();
        message("Network forgotten.", false);
    }
}

static void forget_cb(lv_event_t *e)
{
    (void)e;
    if (s_nw.st.ssid[0] == '\0') {
        return;
    }
    char title[64];
    snprintf(title, sizeof(title), "Forget %s?", s_nw.st.ssid);
    hmi_confirm(title, "The HMI disconnects and does not join this network again until you choose it.", "Forget",
                true, forget_confirm_cb, NULL);
}

/* ---- Wi-Fi card ---- */

static void wifi_toggle_cb(lv_event_t *e)
{
    (void)e;
    const bool on = !s_nw.st.wifi_on;
    s_nw.st.wifi_on = on; /* shown at once; the poll follows the net task */
    hmi_switch_set(s_nw.wifi_sw, on);
    (void)net_set_wifi(on);
    message(on ? "Switching Wi-Fi on..." : "Wi-Fi switched off.", false);
}

static void scan_cb(lv_event_t *e)
{
    (void)e;
    if (net_scan() != ESP_OK) {
        message("Switch Wi-Fi on first.", true);
    }
}

static void list_row(int i, const char *ssid, bool secured, int rssi, bool in_range)
{
    const bool saved = strcmp(ssid, s_nw.st.ssid) == 0;
    lv_obj_t *row = hmi_box(s_nw.list, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, HMI_TINT_OPA, LV_STATE_PRESSED);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_divider_top(row);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    hmi_icon(row, HMI_ICON_WIFI, 20, in_range ? HMI_TEXT : HMI_TEXT_OFF);
    lv_obj_t *name = dots_label(row, ssid, HMI_FS_BODY, saved ? HMI_SEMIBOLD : HMI_REGULAR, HMI_TEXT);
    lv_obj_set_flex_grow(name, 1);
    if (saved) {
        const bool up = s_nw.st.state == NET_WIFI_CONNECTED;
        hmi_tag_set(hmi_tag(row, false), up ? "Connected" : "Saved", up ? HMI_SUCCESS_BG : HMI_WHITE,
                    up ? HMI_SUCCESS : HMI_TEXT_2);
    }
    char t[40];
    if (in_range) {
        snprintf(t, sizeof(t), "%s \xC2\xB7 %s", secured ? "Secured" : "Open", signal_word(rssi));
    } else {
        snprintf(t, sizeof(t), "Not in the last scan");
    }
    lv_obj_t *sig = hmi_label(row, t, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(sig, SIG_W);
    lv_obj_set_style_text_align(sig, LV_TEXT_ALIGN_RIGHT, 0);
}

static void list_build(void)
{
    lv_obj_clean(s_nw.list);
    const int n = net_scan_results(s_nw.aps, NET_SCAN_MAX);
    bool saved_listed = s_nw.st.ssid[0] == '\0';
    for (int i = 0; i < n; i++) {
        saved_listed |= strcmp(s_nw.aps[i].ssid, s_nw.st.ssid) == 0;
    }
    if (!saved_listed) {
        list_row(-1, s_nw.st.ssid, true, 0, false);
    }
    for (int i = 0; i < n; i++) {
        list_row(i, s_nw.aps[i].ssid, s_nw.aps[i].secured, s_nw.aps[i].rssi, true);
    }
    if (n == 0 && saved_listed) {
        lv_obj_t *l = hmi_label(s_nw.list,
                                !s_nw.st.wifi_on ? "Wi-Fi is off."
                                : s_nw.st.scanning ? "Looking for networks..."
                                                   : "No networks found. Tap Scan to look again.",
                                HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
        lv_obj_set_style_pad_all(l, 8, 0);
    }
    s_nw.list_gen = s_nw.st.scan_gen;
    strlcpy(s_nw.list_ssid, s_nw.st.ssid, sizeof(s_nw.list_ssid));
    s_nw.list_connected = s_nw.st.state == NET_WIFI_CONNECTED;
    s_nw.list_wifi_on = s_nw.st.wifi_on;
    s_nw.list_built = true;
}

static void wifi_status_show(void)
{
    const net_status_t *st = &s_nw.st;
    char d[128];
    uint32_t bg = HMI_WHITE, fg = HMI_TEXT_2;
    const char *tag = net_state_text(st->state);
    switch (st->state) {
    case NET_WIFI_OFF:
        snprintf(d, sizeof(d), "Switch Wi-Fi on to look for networks.");
        break;
    case NET_WIFI_STARTING:
        snprintf(d, sizeof(d), "Waiting for the Wi-Fi module (ESP32-C6)...");
        break;
    case NET_WIFI_FAILED:
        snprintf(d, sizeof(d), "%s", st->reason);
        bg = HMI_CRITICAL_BG;
        fg = HMI_CRITICAL;
        break;
    case NET_WIFI_IDLE:
        snprintf(d, sizeof(d), "Choose a network below.");
        break;
    case NET_WIFI_CONNECTING:
        snprintf(d, sizeof(d), "%s", st->ssid);
        bg = HMI_CAUTION_BG;
        fg = HMI_CAUTION;
        break;
    case NET_WIFI_CONNECTED:
        if (st->rssi) {
            snprintf(d, sizeof(d), "%s \xC2\xB7 %u.%u.%u.%u \xC2\xB7 %s (%d dBm)", st->ssid, st->ip[0], st->ip[1],
                     st->ip[2], st->ip[3], signal_word(st->rssi), st->rssi);
        } else {
            snprintf(d, sizeof(d), "%s \xC2\xB7 %u.%u.%u.%u", st->ssid, st->ip[0], st->ip[1], st->ip[2], st->ip[3]);
        }
        bg = HMI_SUCCESS_BG;
        fg = HMI_SUCCESS;
        break;
    case NET_WIFI_RETRY:
        tag = "Not connected";
        snprintf(d, sizeof(d), "%s: %s. Trying again.", st->ssid, st->reason);
        bg = HMI_CRITICAL_BG;
        fg = HMI_CRITICAL;
        break;
    }
    hmi_tag_set(s_nw.wifi_tag, tag, bg, fg);
    hmi_set_text(s_nw.wifi_detail, d);
}

static void wifi_card(lv_obj_t *body)
{
    lv_obj_t *card = hmi_card(body, 10, 16, 8);
    lv_obj_set_style_pad_bottom(card, 12, 0);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);

    lv_obj_t *top = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_size(top, LV_PCT(100), 40);
    lv_obj_add_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(top, wifi_toggle_cb, LV_EVENT_CLICKED, NULL);
    hmi_icon(top, HMI_ICON_WIFI, 20, HMI_TEXT);
    hmi_label(top, "Wi-Fi", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_spacer(top);
    s_nw.wifi_sw = hmi_switch(top, s_nw.st.wifi_on);

    lv_obj_t *status = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_nw.wifi_tag = hmi_tag(status, true);
    s_nw.wifi_detail = dots_label(status, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_flex_grow(s_nw.wifi_detail, 1);

    lv_obj_t *head = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_size(head, LV_PCT(100), 56);
    lv_obj_set_style_pad_top(head, 8, 0);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(head, "Networks", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_spacer(head);
    s_nw.scan_label = hmi_label(head, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    s_nw.scan_btn = hmi_control(head, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(s_nw.scan_btn, 16, 0);
    hmi_icon(s_nw.scan_btn, HMI_ICON_SYNC, HMI_ICON_BTN, HMI_TEXT);
    hmi_label(s_nw.scan_btn, "Scan", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(s_nw.scan_btn, scan_cb, LV_EVENT_CLICKED, NULL);

    s_nw.list = hmi_box(card, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(s_nw.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_nw.list, 1);
    hmi_scroll_area(s_nw.list);
    lv_obj_set_style_border_side(s_nw.list, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_nw.list, 1, 0);
    lv_obj_set_style_border_color(s_nw.list, lv_color_black(), 0);
    lv_obj_set_style_border_opa(s_nw.list, HMI_STROKE_OPA, 0);

    lv_obj_t *bottom = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(bottom, LV_PCT(100));
    lv_obj_set_style_pad_top(bottom, 4, 0);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_nw.message = dots_label(bottom, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_flex_grow(s_nw.message, 1);
    lv_obj_t *hidden = hmi_control(bottom, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(hidden, 16, 0);
    hmi_label(hidden, "Hidden network", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(hidden, hidden_cb, LV_EVENT_CLICKED, NULL);
    s_nw.forget_btn = hmi_control(bottom, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(s_nw.forget_btn, 16, 0);
    hmi_label(s_nw.forget_btn, "Forget network", HMI_FS_BODY, HMI_REGULAR, HMI_CRITICAL);
    lv_obj_add_event_cb(s_nw.forget_btn, forget_cb, LV_EVENT_CLICKED, NULL);
}

/* ---- Internet time card ---- */

static void ntp_toggle_cb(lv_event_t *e)
{
    (void)e;
    const bool on = !s_nw.st.ntp_on;
    s_nw.st.ntp_on = on;
    hmi_switch_set(s_nw.ntp_sw, on);
    (void)net_set_ntp(on, NULL);
}

static void server_set_cb(const char *text, void *user)
{
    (void)user;
    if (text && net_set_ntp(s_nw.st.ntp_on, text) != ESP_OK) {
        message("Server name too long.", true);
    }
}

static void server_cb(lv_event_t *e)
{
    (void)e;
    hmi_text_input("Time server", "Name or IP address of an NTP server, e.g. pool.ntp.org or the plant's own server.",
                   s_nw.st.ntp_server, false, 1, NET_SERVER_MAX, server_set_cb, NULL);
}

static void sync_cb(lv_event_t *e)
{
    (void)e;
    (void)net_ntp_sync_now();
    hmi_set_text(s_nw.ntp_err, "Asking the server...");
}

static lv_obj_t *info_row(lv_obj_t *card, const char *name)
{
    lv_obj_t *r = hmi_box(card, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_size(r, LV_PCT(100), 36);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_divider_top(r);
    lv_obj_set_width(hmi_label(r, name, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT), LABEL_W);
    return dots_label(r, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
}

static void time_card(lv_obj_t *body)
{
    lv_obj_t *card = hmi_card(body, 10, 16, 8);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_size(card, RIGHT_W, LV_PCT(100));

    lv_obj_t *top = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_size(top, LV_PCT(100), 40);
    lv_obj_add_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(top, ntp_toggle_cb, LV_EVENT_CLICKED, NULL);
    hmi_icon(top, HMI_ICON_CLOCK, 20, HMI_TEXT);
    hmi_label(top, "Internet time", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_spacer(top);
    s_nw.ntp_sw = hmi_switch(top, s_nw.st.ntp_on);

    lv_obj_t *note = hmi_label(card,
                               "Sets the clock from a time server once Wi-Fi is connected, then every hour, and "
                               "writes it to the RTC. Needed after a power cut while the RTC has no backup cell.",
                               HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(note, LV_PCT(100));

    lv_obj_t *r = hmi_box(card, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_style_pad_ver(r, 6, 0);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(hmi_label(r, "Server", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT), LABEL_W);
    lv_obj_t *b = hmi_control(r, 0, 44);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(b, 12, 0);
    s_nw.server_val = dots_label(b, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_flex_grow(s_nw.server_val, 1);
    hmi_icon(b, HMI_ICON_CHEVRON_RIGHT, 16, HMI_TEXT_2);
    lv_obj_add_event_cb(b, server_cb, LV_EVENT_CLICKED, NULL);

    s_nw.sync_val = info_row(card, "Last sync");
    s_nw.clock_val = info_row(card, "Clock");

    hmi_spacer(card);
    s_nw.ntp_err = hmi_label(card, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(s_nw.ntp_err, LV_PCT(100));
    lv_obj_t *sync = hmi_control(card, LV_PCT(100), HMI_TOUCH_H);
    hmi_icon(sync, HMI_ICON_SYNC, HMI_ICON_BTN, HMI_TEXT);
    hmi_label(sync, "Sync now", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_add_event_cb(sync, sync_cb, LV_EVENT_CLICKED, NULL);
}

static void time_show(void)
{
    const net_status_t *st = &s_nw.st;
    hmi_set_text(s_nw.server_val, st->ntp_server);
    char t[48];
    if (!st->ntp_on) {
        snprintf(t, sizeof(t), "Off");
    } else if (st->last_sync == 0) {
        snprintf(t, sizeof(t), st->state == NET_WIFI_CONNECTED ? "Waiting for the server" : "Waiting for Wi-Fi");
    } else {
        struct tm l;
        localtime_r(&st->last_sync, &l);
        strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", &l);
    }
    hmi_set_text(s_nw.sync_val, t);
    if (machine_clock_valid()) {
        const time_t now = time(NULL);
        struct tm l;
        localtime_r(&now, &l);
        strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", &l);
        lv_obj_set_style_text_color(s_nw.clock_val, HMI_C(HMI_TEXT_2), 0);
    } else {
        snprintf(t, sizeof(t), "Not set");
        lv_obj_set_style_text_color(s_nw.clock_val, HMI_C(HMI_CRITICAL), 0);
    }
    hmi_set_text(s_nw.clock_val, t);
    if (st->ntp_error[0]) {
        hmi_set_text(s_nw.ntp_err, st->ntp_error);
    } else if (st->last_sync && strcmp(lv_label_get_text(s_nw.ntp_err), "Asking the server...") == 0) {
        hmi_set_text(s_nw.ntp_err, "");
    }
}

/* ---- Poll ---- */

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    const time_t last_sync = s_nw.st.last_sync;
    net_status(&s_nw.st);
    if (s_nw.st.last_sync != last_sync && last_sync != 0) {
        hmi_set_text(s_nw.ntp_err, "");
    }
    hmi_switch_set(s_nw.wifi_sw, s_nw.st.wifi_on);
    hmi_switch_set(s_nw.ntp_sw, s_nw.st.ntp_on);
    wifi_status_show();
    hmi_set_text(s_nw.scan_label, s_nw.st.scanning ? "Looking..." : "");
    hmi_show(s_nw.forget_btn, s_nw.st.ssid[0] != '\0');
    /* One scan per visit, as soon as Wi-Fi runs (also right after switching it on here). */
    const net_wifi_state_t ws = s_nw.st.state;
    if (!s_nw.st.wifi_on) {
        s_nw.scan_asked = false;
    } else if (!s_nw.scan_asked && ws != NET_WIFI_OFF && ws != NET_WIFI_STARTING && ws != NET_WIFI_FAILED) {
        s_nw.scan_asked = net_scan() == ESP_OK;
    }
    if (!s_nw.list_built || s_nw.st.scan_gen != s_nw.list_gen || strcmp(s_nw.st.ssid, s_nw.list_ssid) != 0 ||
        (ws == NET_WIFI_CONNECTED) != s_nw.list_connected || s_nw.st.wifi_on != s_nw.list_wifi_on) {
        list_build();
    }
    time_show();
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_nw.poll) {
        lv_timer_delete(s_nw.poll);
    }
    memset(&s_nw, 0, sizeof(s_nw));
}

void hmi_network_create(lv_obj_t *root, lv_obj_t *head)
{
    (void)head;
    memset(&s_nw, 0, sizeof(s_nw));
    net_status(&s_nw.st);

    lv_obj_t *body = hmi_box(root, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_add_event_cb(body, deleted_cb, LV_EVENT_DELETE, NULL);
    wifi_card(body);
    time_card(body);
    poll_cb(NULL);
    s_nw.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
