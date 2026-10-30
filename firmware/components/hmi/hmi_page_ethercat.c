/*
 * Settings > EtherCAT (mockup EthercatSetting.dc.html): the topology Scan -> Accept -> Save of
 * docs/ETHERCAT_PLAN.md section 3.6, on screen.
 *
 *   - Check view: the saved topology, each slave compared with the bus slave at the same cable
 *     position (Match, Missing, Wrong model, ...), plus slaves the bus has in excess.
 *     Buttons: Re-check, Scan bus.
 *   - Scan view (after Scan, or when nothing is saved yet): the scanned slaves and what Accept
 *     would do with each (Keep, New model, Add) or the issue that blocks it.
 *     Buttons: Cancel (or Scan again), Accept, which asks for confirmation with the changes.
 *
 * Data comes from the ecat_busview snapshot; requests go through hmi_bus_request(). Scan and
 * Re-check while in OP stop every axis first, so they ask for confirmation.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "ecat_busview.h"
#include "hmi_internal.h"

#define PANEL_W     300
#define ROW_H       44
#define COL_POS     36
#define COL_NAME    64
#define COL_PRODUCT 72
#define COL_REV     64
#define COL_STATUS  112
#define POLL_MS     300

#define TIMING_KEY_W 104

/* Values of the side panel: topology (key above value), then cycle timing (one line each,
 * timing_update()). The panel has no room to scroll: keep it short (photo of 2026-10-10). */
enum { INFO_EXPECTED, INFO_BUS, INFO_CYCLE, INFO_OVERRUNS, INFO_JITTER, INFO_RTT, INFO_ROWS };

typedef enum { CHIP_OK, CHIP_NEUTRAL, CHIP_CAUTION, CHIP_CRITICAL } chip_t;

static struct {
    lv_obj_t *body;
    lv_obj_t *phase_tag;
    lv_obj_t *table_title;
    lv_obj_t *table_note;
    lv_obj_t *status_head;
    lv_obj_t *rows;
    lv_obj_t *banner;
    lv_obj_t *banner_title;
    lv_obj_t *banner_text;
    lv_obj_t *message;
    lv_obj_t *info[INFO_ROWS];
    lv_obj_t *btn_left, *btn_left_label;
    lv_obj_t *btn_right, *btn_right_label;
    lv_timer_t *poll;
    uint32_t gen;
    uint32_t scan_gen_at_request;
    bool waiting_scan;
    bool scan_view;
} s_ec;

static EXT_RAM_BSS_ATTR ecat_busview_t s_view; /* ~3 KB, PSRAM: not on the LVGL task stack */

/* ---- Small pieces ---- */

static void chip_colors(chip_t kind, uint32_t *bg, uint32_t *fg)
{
    switch (kind) {
    case CHIP_OK:
        *bg = HMI_SUCCESS_BG;
        *fg = HMI_SUCCESS;
        break;
    case CHIP_CAUTION:
        *bg = HMI_CAUTION_BG;
        *fg = HMI_CAUTION;
        break;
    case CHIP_CRITICAL:
        *bg = HMI_CRITICAL_BG;
        *fg = HMI_CRITICAL;
        break;
    case CHIP_NEUTRAL:
    default:
        *bg = HMI_WHITE; /* hmi_tag_set(): black at 6 % */
        *fg = HMI_TEXT_2;
        break;
    }
}

static lv_obj_t *cell(lv_obj_t *row, const char *text, int32_t w, int px, hmi_weight_t wt, uint32_t color, bool right)
{
    lv_obj_t *l = hmi_label(row, text, px, wt, color);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(hmi_font(px, wt)));
    if (w > 0) {
        lv_obj_set_width(l, w);
    } else {
        lv_obj_set_flex_grow(l, 1);
    }
    if (right) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
    return l;
}

static lv_obj_t *table_line(lv_obj_t *parent, int32_t h)
{
    lv_obj_t *row = hmi_box(parent, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(row, LV_PCT(100), h);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void hex_text(char *buf, size_t len, uint32_t v)
{
    snprintf(buf, len, "0x%lX", (unsigned long)v);
}

static const char *issue_short(uint8_t code)
{
    switch (code) {
    case ECAT_ISSUE_UNKNOWN_MODULE:    return "Unknown module";
    case ECAT_ISSUE_RACK_UNREADABLE:   return "Rack unreadable";
    case ECAT_ISSUE_RACK_GEOMETRY:     return "Rack geometry";
    case ECAT_ISSUE_TOO_MANY:          return "Too many";
    case ECAT_ISSUE_UNKNOWN_DEVICE:    return "Unknown device";
    default:                           return "Issue";
    }
}

/* First issue the scan reported for bus slave `idx`, or NULL. */
static const ecat_issue_t *issue_of(uint16_t idx)
{
    const ecat_issue_t *found = NULL;
    for (uint8_t k = 0; k < s_view.issues.n; k++) {
        const ecat_issue_t *it = &s_view.issues.item[k];
        if (it->slave == idx && (found == NULL || ecat_issue_is_blocking((ecat_issue_code_t)it->code))) {
            found = it;
        }
    }
    return found;
}

static void rack_lines(lv_obj_t *parent, const ecat_topo_slave_t *s, const ecat_topology_t *t)
{
    if (s->kind != ECAT_KIND_COUPLER || s->rack == ECAT_TOPO_NO_RACK || s->rack >= t->n_racks) {
        return;
    }
    const ecat_rack_t *rack = &t->rack[s->rack];
    lv_obj_t *line = hmi_box(parent, LV_FLEX_FLOW_ROW_WRAP, 16);
    lv_obj_set_width(line, LV_PCT(100));
    lv_obj_set_style_pad_left(line, COL_POS + COL_NAME + 16, 0);
    lv_obj_set_style_pad_bottom(line, 6, 0);
    for (uint8_t m = 0; m < rack->n_modules; m++) {
        const ecat_module_info_t *mi = ecat_catalog_module(s->product, rack->ident[m]);
        char text[64];
        if (mi) {
            snprintf(text, sizeof(text), "Slot %u \xC2\xB7 %s \xC2\xB7 %u DI %u DO", m, mi->name, mi->di, mi->dout);
        } else {
            snprintf(text, sizeof(text), "Slot %u \xC2\xB7 0x%08lX", m, (unsigned long)rack->ident[m]);
        }
        hmi_label(line, text, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    }
}

static lv_obj_t *slave_row(const ecat_topo_slave_t *s, const char *device, uint32_t device_color, const char *status,
                           chip_t kind, const ecat_topology_t *rack_src)
{
    lv_obj_t *wrap = hmi_box(s_ec.rows, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_style_border_side(wrap, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(wrap, 1, 0);
    lv_obj_set_style_border_color(wrap, lv_color_black(), 0);
    lv_obj_set_style_border_opa(wrap, HMI_STROKE_OPA, 0);

    lv_obj_t *row = table_line(wrap, ROW_H);
    char a[12], b[12], c[12];
    snprintf(a, sizeof(a), "%u", s->position);
    cell(row, a, COL_POS, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2, true);
    cell(row, s->name, COL_NAME, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT, false);
    cell(row, device, 0, HMI_FS_BODY, HMI_REGULAR, device_color, false);
    hex_text(b, sizeof(b), s->product);
    cell(row, b, COL_PRODUCT, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2, false);
    hex_text(c, sizeof(c), s->revision);
    cell(row, c, COL_REV, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2, false);
    lv_obj_t *slot = hmi_box(row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_width(slot, COL_STATUS);
    uint32_t bg, fg;
    chip_colors(kind, &bg, &fg);
    hmi_tag_set(hmi_tag(slot, false), status, bg, fg);

    rack_lines(wrap, s, rack_src);
    return wrap;
}

/* ---- Views ---- */

static void build_check_rows(void)
{
    const ecat_topology_t *sv = &s_view.saved;
    const ecat_topology_t *bus = &s_view.bus;
    const ecat_compare_t *cmp = &s_view.cmp;
    for (uint16_t i = 0; i < sv->n_slaves; i++) {
        const ecat_topo_slave_t *s = &sv->slave[i];
        const uint8_t diff = s_view.cmp_valid ? cmp->diff[i] : 0;
        const uint16_t b = s_view.cmp_valid ? cmp->bus[i] : ECAT_TOPO_NONE;
        const ecat_topo_slave_t *found = b < bus->n_slaves ? &bus->slave[b] : NULL;
        char status[24];
        char device[64];
        chip_t kind = CHIP_OK;
        uint32_t dev_color = HMI_TEXT;
        strlcpy(device, ecat_topology_device_name(s), sizeof(device));
        strlcpy(status, "Match", sizeof(status));
        if (!s_view.cmp_valid) {
            strlcpy(status, "Not checked", sizeof(status));
            kind = CHIP_NEUTRAL;
        } else if (diff & ECAT_DIFF_MISSING) {
            strlcpy(status, "Missing", sizeof(status));
            kind = CHIP_CRITICAL;
        } else if (diff & ECAT_DIFF_MODEL) {
            strlcpy(status, "Wrong model", sizeof(status));
            kind = CHIP_CRITICAL;
            dev_color = HMI_CRITICAL;
            snprintf(device, sizeof(device), "%s, found %s", ecat_topology_device_name(s),
                     found ? ecat_topology_device_name(found) : "?");
        } else if (diff & ECAT_DIFF_RACK) {
            strlcpy(status, "IO modules differ", sizeof(status));
            kind = CHIP_CRITICAL;
        } else if (diff & ECAT_DIFF_REVISION) {
            strlcpy(status, "New revision", sizeof(status));
            kind = CHIP_CAUTION;
        }
        slave_row(s, device, dev_color, status, kind, sv);
    }
    for (uint16_t k = 0; s_view.cmp_valid && k < cmp->n_extra; k++) {
        const uint16_t b = cmp->extra[k];
        if (b < bus->n_slaves) {
            slave_row(&bus->slave[b], ecat_topology_device_name(&bus->slave[b]), HMI_TEXT, "Extra", CHIP_CRITICAL,
                      bus);
        }
    }
}

static void row_accept_cb(lv_event_t *e);

static void build_scan_rows(void)
{
    const ecat_topology_t *bus = &s_view.bus;
    const ecat_compare_t *cmp = &s_view.cmp;
    for (uint16_t b = 0; b < bus->n_slaves; b++) {
        const ecat_topo_slave_t *s = &bus->slave[b];
        const ecat_issue_t *issue = issue_of(b);
        const char *status = "Add";
        chip_t kind = s_view.have_saved ? CHIP_CAUTION : CHIP_OK;
        uint32_t dev_color = HMI_TEXT;
        if (s_view.have_saved && s_view.cmp_valid) {
            for (uint16_t i = 0; i < s_view.saved.n_slaves; i++) {
                if (cmp->bus[i] != b) {
                    continue;
                }
                const uint8_t diff = cmp->diff[i];
                status = "Keep";
                kind = CHIP_OK;
                if (diff & ECAT_DIFF_MODEL) {
                    status = "New model";
                    kind = CHIP_CAUTION;
                    dev_color = HMI_CAUTION;
                } else if (diff & ECAT_DIFF_RACK) {
                    status = "New IO modules";
                    kind = CHIP_CAUTION;
                }
                break;
            }
        }
        if (issue) {
            const bool blocking = ecat_issue_is_blocking((ecat_issue_code_t)issue->code);
            status = issue_short(issue->code);
            kind = blocking ? CHIP_CRITICAL : CHIP_CAUTION;
        }
        lv_obj_t *row = slave_row(s, ecat_topology_device_name(s), dev_color, status, kind, bus);
        /* Tapping a row (its "Add" chip looks like a button) opens the same Accept dialog. */
        if (s_view.issues.n_blocking == 0 && s_view.phase == ECAT_BUS_PHASE_HELD) {
            lv_obj_set_clickable(row, true);
            lv_obj_set_style_bg_color(row, lv_color_black(), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, HMI_TINT_OPA, LV_STATE_PRESSED);
            lv_obj_add_event_cb(row, row_accept_cb, LV_EVENT_CLICKED, NULL);
        }
    }
}

static void message_set(const char *text);

static void set_banner(chip_t kind, const char *title, const char *text)
{
    uint32_t bg, fg;
    chip_colors(kind, &bg, &fg);
    if (kind == CHIP_NEUTRAL) {
        lv_obj_set_style_bg_color(s_ec.banner, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_ec.banner, HMI_TINT_OPA, 0);
    } else {
        lv_obj_set_style_bg_color(s_ec.banner, HMI_C(bg), 0);
        lv_obj_set_style_bg_opa(s_ec.banner, LV_OPA_COVER, 0);
    }
    lv_label_set_text(s_ec.banner_title, title);
    lv_obj_set_style_text_color(s_ec.banner_title, HMI_C(kind == CHIP_NEUTRAL ? HMI_TEXT : fg), 0);
    lv_label_set_text(s_ec.banner_text, text);
}

static void count_kinds(const ecat_topology_t *t, unsigned *axes, unsigned *io)
{
    *axes = 0;
    *io = 0;
    for (uint16_t i = 0; i < t->n_slaves; i++) {
        if (ecat_kind_is_drive((ecat_kind_t)t->slave[i].kind)) {
            (*axes)++;
        } else if (t->slave[i].kind == ECAT_KIND_COUPLER || t->slave[i].kind == ECAT_KIND_IO) {
            (*io)++;
        }
    }
}

/* Banner text for a mismatch: the first blocking difference, by name. */
static void mismatch_text(char *buf, size_t len)
{
    const ecat_compare_t *cmp = &s_view.cmp;
    for (uint16_t i = 0; i < s_view.saved.n_slaves; i++) {
        const ecat_topo_slave_t *s = &s_view.saved.slave[i];
        const uint8_t diff = cmp->diff[i];
        if (diff & ECAT_DIFF_MISSING) {
            snprintf(buf, len, "%s (position %u) is not on the bus. The bus is held in PRE-OP and motion is blocked.",
                     s->name, s->position);
            return;
        }
        if (diff & ECAT_DIFF_MODEL) {
            const uint16_t b = cmp->bus[i];
            snprintf(buf, len, "%s (position %u): saved %s, found %s. The bus is held in PRE-OP and motion is "
                     "blocked.", s->name, s->position, ecat_topology_device_name(s),
                     b < s_view.bus.n_slaves ? ecat_topology_device_name(&s_view.bus.slave[b]) : "?");
            return;
        }
        if (diff & ECAT_DIFF_RACK) {
            snprintf(buf, len, "%s: the IO modules differ from the saved rack. The bus is held in PRE-OP.", s->name);
            return;
        }
    }
    if (cmp->n_extra) {
        snprintf(buf, len, "%u slave(s) on the bus are not in the saved topology. Scan and accept to add them.",
                 cmp->n_extra);
        return;
    }
    snprintf(buf, len, "The bus does not match the saved topology. Scan the bus to see why.");
}

static void update_panel(void)
{
    char text[200];
    unsigned axes, io;
    const bool op = s_view.phase == ECAT_BUS_PHASE_OP;
    const bool starting = s_view.phase == ECAT_BUS_PHASE_STARTING;

    /* Header tag */
    if (op) {
        hmi_tag_set(s_ec.phase_tag, "Bus in OP", HMI_SUCCESS_BG, HMI_SUCCESS);
    } else if (starting || s_ec.waiting_scan) {
        hmi_tag_set(s_ec.phase_tag, "Working", HMI_WHITE, HMI_TEXT_2);
    } else {
        hmi_tag_set(s_ec.phase_tag, "Held in PRE-OP", HMI_CAUTION_BG, HMI_CAUTION);
    }

    /* Banner */
    if (s_ec.waiting_scan || starting) {
        set_banner(CHIP_NEUTRAL, "Working", "Scanning and configuring the bus. This takes a few seconds.");
    } else if (s_ec.scan_view) {
        if (s_view.issues.n_blocking) {
            const ecat_issue_t *first = NULL;
            for (uint8_t k = 0; k < s_view.issues.n && first == NULL; k++) {
                if (ecat_issue_is_blocking((ecat_issue_code_t)s_view.issues.item[k].code)) {
                    first = &s_view.issues.item[k];
                }
            }
            const char *who = first && first->slave < s_view.bus.n_slaves ? s_view.bus.slave[first->slave].name : "";
            snprintf(text, sizeof(text), "%u blocking issue(s). %s: %s.", s_view.issues.n_blocking, who,
                     first ? ecat_issue_text((ecat_issue_code_t)first->code) : "");
            set_banner(CHIP_CRITICAL, "Scan cannot be accepted", text);
        } else {
            snprintf(text, sizeof(text), "%u slave(s) found. Review the list, then Accept to make it the expected topology.",
                     s_view.bus.n_slaves);
            set_banner(CHIP_CAUTION, s_view.have_saved ? "New scan \xC2\xB7 not saved" : "No saved topology", text);
        }
    } else if (op) {
        if (s_view.n_blocked) {
            snprintf(text, sizeof(text), "%u slave(s) failed the identity check after a recovery and are held in "
                     "PRE-OP. Stop the machine, then Re-check.", s_view.n_blocked);
            set_banner(CHIP_CRITICAL, "Slave held", text);
        } else {
            if (s_view.saved.n_slaves == 1) {
                strlcpy(text, "The slave matches the saved topology. Home all axes before running.", sizeof(text));
            } else {
                snprintf(text, sizeof(text), "All %u slaves match the saved topology. Home all axes before running.",
                         s_view.saved.n_slaves);
            }
            set_banner(CHIP_OK, "Topology OK", text);
        }
    } else if (s_view.have_saved && s_view.cmp_valid && !s_view.cmp.ok) {
        mismatch_text(text, sizeof(text));
        set_banner(CHIP_CRITICAL, "Topology mismatch", text);
    } else {
        set_banner(CHIP_CAUTION, "Bus held in PRE-OP", "The topology matches, but the bus did not go to OP. "
                   "See the message below and the console log, then Re-check.");
    }
    message_set(s_view.message);

    /* Info list */
    if (s_view.have_saved) {
        char when[24] = "time unknown";
        if (s_view.meta.saved_at) {
            time_t t = (time_t)s_view.meta.saved_at;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        }
        snprintf(text, sizeof(text), "%u slave%s \xC2\xB7 %s \xC2\xB7 %s", s_view.saved.n_slaves,
                 s_view.saved.n_slaves == 1 ? "" : "s", when, s_view.meta.saved_by);
    } else {
        strlcpy(text, "Not saved yet", sizeof(text));
    }
    lv_label_set_text(s_ec.info[INFO_EXPECTED], text);
    count_kinds(&s_view.bus, &axes, &io);
    snprintf(text, sizeof(text), "%u slave%s \xC2\xB7 %u ax%s \xC2\xB7 %u IO", s_view.bus.n_slaves,
             s_view.bus.n_slaves == 1 ? "" : "s", axes, axes == 1 ? "is" : "es", io);
    lv_label_set_text(s_ec.info[INFO_BUS], text);

    /* Buttons */
    const bool busy = starting || s_ec.waiting_scan;
    lv_label_set_text(s_ec.btn_left_label, s_ec.scan_view ? (s_view.have_saved ? "Cancel" : "Scan again") : "Re-check");
    lv_label_set_text(s_ec.btn_right_label, s_ec.scan_view ? "Accept" : "Scan bus");
    const bool right_ok = !busy && (!s_ec.scan_view || (s_view.issues.n_blocking == 0 && s_view.bus.n_slaves > 0));
    lv_obj_set_state(s_ec.btn_left, LV_STATE_DISABLED, busy);
    lv_obj_set_state(s_ec.btn_right, LV_STATE_DISABLED, !right_ok);
    lv_obj_set_style_opa(s_ec.btn_left, busy ? LV_OPA_40 : LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_ec.btn_right, right_ok ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void update_all(void)
{
    /* Nothing saved yet: the scan is the only thing to show. */
    if (!s_view.have_saved && s_view.bus.n_slaves) {
        s_ec.scan_view = true;
    }
    lv_label_set_text(s_ec.table_title, s_ec.scan_view ? "Scan result" : "Expected topology");
    lv_label_set_text(s_ec.table_note, s_ec.scan_view ? "Axes are numbered in cable order"
                                                      : "Compared position by position with the last scan");
    lv_label_set_text(s_ec.status_head, s_ec.scan_view ? "On accept" : "Vs. bus");
    lv_obj_clean(s_ec.rows);
    if (s_ec.scan_view) {
        build_scan_rows();
    } else if (s_view.have_saved) {
        build_check_rows();
    }
    update_panel();
}

/* Cycle timing rows, refreshed on every poll: main.c publishes them once a second. */
static void timing_update(void)
{
    hmi_bus_status_t st = {0};
    (void)hmi_bus_status_read(&st);
    const bool op = st.state == HMI_BUS_OP || st.state == HMI_BUS_OP_FAULT;
    char t[64], a[16], b[16];
    if (!op) {
        for (int i = INFO_CYCLE; i < INFO_ROWS; i++) {
            hmi_set_text(s_ec.info[i], i == INFO_CYCLE ? "\xE2\x80\x94 not in OP" : "\xE2\x80\x94");
        }
        return;
    }
    hmi_fmt_us(st.cycle_us, a, sizeof(a));
    hmi_set_text(s_ec.info[INFO_CYCLE], a);
    snprintf(t, sizeof(t), "%lu since OP", (unsigned long)st.overruns);
    hmi_set_text(s_ec.info[INFO_OVERRUNS], t);
    hmi_fmt_us(st.jitter_us, a, sizeof(a));
    snprintf(t, sizeof(t), "%s worst", a);
    hmi_set_text(s_ec.info[INFO_JITTER], t);
    hmi_fmt_us(st.rtt_avg_us, a, sizeof(a));
    hmi_fmt_us(st.rtt_max_us, b, sizeof(b));
    snprintf(t, sizeof(t), "%s / %s", a, b);
    hmi_set_text(s_ec.info[INFO_RTT], t);
}

static void message_set(const char *text)
{
    hmi_set_text(s_ec.message, text);
    lv_obj_set_hidden(s_ec.message, text[0] == '\0');
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    timing_update();
    const uint32_t gen = ecat_busview_gen();
    if (gen == s_ec.gen) {
        return;
    }
    ecat_busview_read(&s_view);
    s_ec.gen = s_view.gen;
    if (s_ec.waiting_scan && s_view.scan_gen != s_ec.scan_gen_at_request && s_view.phase != ECAT_BUS_PHASE_STARTING) {
        s_ec.waiting_scan = false;
        s_ec.scan_view = true;
    }
    update_all();
}

/* ---- Actions ---- */

static void request(hmi_bus_req_t req)
{
    if (!hmi_bus_request(req)) {
        message_set("The EtherCAT task is busy. Try again.");
        return;
    }
    if (req == HMI_BUS_REQ_SCAN) {
        s_ec.waiting_scan = true;
        s_ec.scan_gen_at_request = s_view.scan_gen;
    } else {
        s_ec.scan_view = false;
    }
    update_panel();
}

static void confirmed_cb(bool ok, void *user)
{
    if (ok) {
        request((hmi_bus_req_t)(intptr_t)user);
    }
}

static void ask_leave_op(hmi_bus_req_t req)
{
    if (s_view.phase == ECAT_BUS_PHASE_OP) {
        hmi_confirm(req == HMI_BUS_REQ_SCAN ? "Scan the bus?" : "Re-check the bus?",
                    "EtherCAT leaves OP first: every axis stops and its servo is switched off. "
                    "Make sure the machine is stopped.",
                    req == HMI_BUS_REQ_SCAN ? "Scan bus" : "Re-check", true, confirmed_cb, (void *)(intptr_t)req);
    } else {
        request(req);
    }
}

static void accept_dialog(void)
{
    static char body[512];
    size_t used = 0;
    const ecat_compare_t *cmp = &s_view.cmp;
    unsigned axes, io;
    count_kinds(&s_view.bus, &axes, &io);
    if (!s_view.have_saved) {
        used += (size_t)snprintf(body + used, sizeof(body) - used, "First topology: %u slaves (%u axes, %u IO).\n",
                                 s_view.bus.n_slaves, axes, io);
    } else if (s_view.cmp_valid) {
        unsigned changes = 0;
        for (uint16_t i = 0; i < s_view.saved.n_slaves && used < sizeof(body) - 80; i++) {
            const ecat_topo_slave_t *s = &s_view.saved.slave[i];
            const uint8_t d = cmp->diff[i];
            const uint16_t b = cmp->bus[i];
            const ecat_topo_slave_t *f = b < s_view.bus.n_slaves ? &s_view.bus.slave[b] : NULL;
            if (d & ECAT_DIFF_MISSING) {
                used += (size_t)snprintf(body + used, sizeof(body) - used, "%s: removed\n", s->name);
            } else if ((d & ECAT_DIFF_MODEL) && f) {
                used += (size_t)snprintf(body + used, sizeof(body) - used, "%s: model %s, was %s\n", s->name,
                                         ecat_topology_device_name(f), ecat_topology_device_name(s));
            } else if (d & ECAT_DIFF_RACK) {
                used += (size_t)snprintf(body + used, sizeof(body) - used, "%s: new IO modules\n", s->name);
            } else {
                continue;
            }
            changes++;
        }
        for (uint16_t k = 0; k < cmp->n_extra && used < sizeof(body) - 80; k++) {
            const uint16_t b = cmp->extra[k];
            if (b < s_view.bus.n_slaves) {
                used += (size_t)snprintf(body + used, sizeof(body) - used, "%s: added (%s)\n",
                                         s_view.bus.slave[b].name, ecat_topology_device_name(&s_view.bus.slave[b]));
                changes++;
            }
        }
        if (changes == 0) {
            used += (size_t)snprintf(body + used, sizeof(body) - used, "No changes to the saved topology.\n");
        }
    }
    snprintf(body + used, sizeof(body) - used,
             "\nThe bus restarts (PRE-OP to OP) and every axis must be homed again. Check Settings > Servo for a "
             "drive with a new model before running.");
    hmi_confirm("Save this as the expected topology?", body, "Save topology", false, confirmed_cb,
                (void *)(intptr_t)HMI_BUS_REQ_ACCEPT);
}

static void left_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ec.scan_view) {
        ask_leave_op(HMI_BUS_REQ_RECHECK);
    } else if (s_view.have_saved) {
        request(HMI_BUS_REQ_RECHECK); /* back to the saved topology: compare again and go to OP */
    } else {
        request(HMI_BUS_REQ_SCAN);
    }
}

static void row_accept_cb(lv_event_t *e)
{
    (void)e;
    if (s_ec.scan_view && !s_ec.waiting_scan) {
        accept_dialog();
    }
}

static void right_cb(lv_event_t *e)
{
    (void)e;
    if (s_ec.scan_view) {
        accept_dialog();
    } else {
        ask_leave_op(HMI_BUS_REQ_SCAN);
    }
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_ec.poll) {
        lv_timer_delete(s_ec.poll);
    }
    memset(&s_ec, 0, sizeof(s_ec));
}

/* ---- Layout ---- */

static lv_obj_t *panel_button(lv_obj_t *row, bool filled, lv_obj_t **label, lv_event_cb_t cb)
{
    lv_obj_t *b;
    if (filled) {
        b = hmi_filled_button(row, HMI_TOUCH_H, HMI_ACCENT, 4);
        lv_obj_set_width(b, 0);
        *label = hmi_label(b, "", HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    } else {
        b = hmi_control(row, 0, HMI_TOUCH_H);
        *label = hmi_label(b, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    }
    lv_obj_set_flex_grow(b, 1);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void hmi_ethercat_create(lv_obj_t *root, lv_obj_t *head)
{
    memset(&s_ec, 0, sizeof(s_ec));
    hmi_spacer(head);
    s_ec.phase_tag = hmi_tag(head, true);

    s_ec.body = hmi_box(root, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(s_ec.body, LV_PCT(100));
    lv_obj_set_flex_grow(s_ec.body, 1);
    lv_obj_add_event_cb(s_ec.body, deleted_cb, LV_EVENT_DELETE, NULL);

    /* Table */
    lv_obj_t *table = hmi_card(s_ec.body, 6, 14, 0);
    lv_obj_set_style_pad_bottom(table, 8, 0);
    lv_obj_set_height(table, LV_PCT(100));
    lv_obj_set_flex_grow(table, 1);
    lv_obj_t *title = table_line(table, 30);
    s_ec.table_title = hmi_card_title(title, "");
    hmi_spacer(title);
    s_ec.table_note = hmi_label(title, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);

    lv_obj_t *hdr = table_line(table, 28);
    cell(hdr, "Pos", COL_POS, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2, true);
    cell(hdr, "Name", COL_NAME, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2, false);
    cell(hdr, "Device", 0, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2, false);
    cell(hdr, "Product", COL_PRODUCT, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2, false);
    cell(hdr, "Rev", COL_REV, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2, false);
    s_ec.status_head = cell(hdr, "", COL_STATUS, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2, false);

    s_ec.rows = hmi_box(table, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(s_ec.rows, LV_PCT(100));
    lv_obj_set_flex_grow(s_ec.rows, 1);
    hmi_scroll_area(s_ec.rows);

    /* Side panel: the text scrolls if it is too long, the buttons always stay at the bottom. */
    lv_obj_t *panel = hmi_card(s_ec.body, 12, 14, 10);
    lv_obj_set_size(panel, PANEL_W, LV_PCT(100));

    lv_obj_t *info = hmi_box(panel, LV_FLEX_FLOW_COLUMN, 8);
    lv_obj_set_width(info, LV_PCT(100));
    lv_obj_set_flex_grow(info, 1);
    hmi_scroll_area(info);

    s_ec.banner = hmi_box(info, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_width(s_ec.banner, LV_PCT(100));
    lv_obj_set_style_radius(s_ec.banner, 8, 0);
    lv_obj_set_style_pad_ver(s_ec.banner, 10, 0);
    lv_obj_set_style_pad_hor(s_ec.banner, 12, 0);
    lv_obj_set_style_border_width(s_ec.banner, 1, 0);
    lv_obj_set_style_border_color(s_ec.banner, lv_color_black(), 0);
    lv_obj_set_style_border_opa(s_ec.banner, HMI_STROKE_OPA, 0);
    s_ec.banner_title = hmi_label(s_ec.banner, "", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    s_ec.banner_text = hmi_label(s_ec.banner, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_width(s_ec.banner_text, LV_PCT(100));

    s_ec.message = hmi_label(info, "", HMI_FS_SMALL, HMI_SEMIBOLD, HMI_CAUTION);
    lv_obj_set_width(s_ec.message, LV_PCT(100));
    lv_obj_set_hidden(s_ec.message, true);

    static const char *const topo_keys[] = {"Expected", "On the bus"};
    for (int i = INFO_EXPECTED; i <= INFO_BUS; i++) {
        lv_obj_t *kv = hmi_box(info, LV_FLEX_FLOW_COLUMN, 0);
        lv_obj_set_width(kv, LV_PCT(100));
        hmi_label(kv, topo_keys[i], HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
        s_ec.info[i] = cell(kv, "", LV_PCT(100), HMI_FS_BODY, HMI_REGULAR, HMI_TEXT, false);
    }

    /* Cycle timing of the last second: one line per value */
    static const char *const timing_keys[] = {"Cycle", "Overruns", "Jitter", "RTT avg / max"};
    lv_obj_t *timing = hmi_box(info, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_width(timing, LV_PCT(100));
    lv_obj_set_style_pad_top(timing, 8, 0);
    hmi_divider_top(timing);
    hmi_label(timing, "Cycle timing (last second)", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    for (int i = INFO_CYCLE; i < INFO_ROWS; i++) {
        lv_obj_t *r = hmi_box(timing, LV_FLEX_FLOW_ROW, 8);
        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_width(hmi_label(r, timing_keys[i - INFO_CYCLE], HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), TIMING_KEY_W);
        s_ec.info[i] = cell(r, "", 0, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT, false);
    }


    lv_obj_t *buttons = hmi_box(panel, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(buttons, LV_PCT(100));
    s_ec.btn_left = panel_button(buttons, false, &s_ec.btn_left_label, left_cb);
    s_ec.btn_right = panel_button(buttons, true, &s_ec.btn_right_label, right_cb);

    ecat_busview_read(&s_view);
    s_ec.gen = s_view.gen;
    update_all();
    timing_update();
    s_ec.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
