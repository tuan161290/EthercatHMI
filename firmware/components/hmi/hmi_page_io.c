/*
 * IO monitor (mockup IoMonitor.dc.html): digital inputs and outputs of the remote IO couplers
 * (R1EC), analog channels, and the couplers' module list and Device Status.
 *
 * Data come from ecat_io.h snapshots; outputs are forced through its command queue. Force mode:
 *   - needs Manual mode and the bus in OP, and is confirmed in a dialog;
 *   - every output row then toggles its output;
 *   - it ends when switched off or when the page is left, and forced outputs go back to off
 *     (there is no machine logic driving outputs yet).
 *
 * Channel names are not known until the machine is (no IO list yet): rows show the address and
 * the module slot and channel instead.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "ecat_io.h"
#include "hmi_internal.h"

#define POLL_MS     100
#define MAX_CH      256 /* channels shown per direction */
#define CH_ROW_H    46
#define FORCE_BG    0x9D5D00

enum { VIEW_DIGITAL, VIEW_ANALOG, VIEW_ETHERCAT, VIEW_COUNT };
static const char *const s_view_names[VIEW_COUNT] = {"Digital I/O", "Analog", "EtherCAT"};

typedef struct {
    lv_obj_t *row;
    lv_obj_t *lamp;
    lv_obj_t *addr;
    uint8_t coupler;
    uint16_t bit;  /* in the coupler's input or output image */
    int8_t state;  /* last shown: -1 unknown */
} ch_t;

static int s_view = VIEW_DIGITAL;
static ecat_io_layout_t *s_l; /* ~5 KB, LVGL heap (PSRAM): internal RAM is kept for DMA and EtherCAT */
static EXT_RAM_BSS_ATTR ecat_io_image_t s_img;

typedef struct {
    lv_obj_t *page;
    lv_obj_t *root;
    lv_timer_t *poll;
    uint32_t gen;
    bool force_shown;
    lv_obj_t *force_btn, *force_sw, *force_state;
    lv_obj_t *banner;
    lv_obj_t *out_card;
    ch_t in[MAX_CH];
    ch_t out[MAX_CH];
    uint16_t n_in, n_out;
    /* analog: value labels */
    lv_obj_t *an_val[ECAT_IO_MAX_COUPLERS * ECAT_TOPO_MAX_MODULES][4];
    lv_obj_t *an_diag[ECAT_IO_MAX_COUPLERS * ECAT_TOPO_MAX_MODULES][4];
    uint8_t an_coupler[ECAT_IO_MAX_COUPLERS * ECAT_TOPO_MAX_MODULES];
    const ecat_io_module_t *an_mod[ECAT_IO_MAX_COUPLERS * ECAT_TOPO_MAX_MODULES];
    int n_an;
    /* EtherCAT view: Device Status tags */
    lv_obj_t *status_tag[ECAT_IO_MAX_COUPLERS];
} io_page_t;

static io_page_t *s_io; /* ~13 KB, LVGL heap (PSRAM), allocated on first use */

static void build(void);

/* ---- Helpers ---- */

static void addr_text(char prefix, uint16_t n, char *out, size_t len)
{
    snprintf(out, len, "%c%u.%u", prefix, n / 8, n % 8);
}

static int16_t img_i16(const uint8_t *img, uint16_t bit)
{
    const uint16_t b = bit / 8;
    if (b + 1 >= ECAT_IO_IMAGE_BYTES) {
        return 0;
    }
    return (int16_t)(img[b] | (img[b + 1] << 8));
}

static uint8_t img_u8(const uint8_t *img, uint16_t bit)
{
    return bit / 8 < ECAT_IO_IMAGE_BYTES ? img[bit / 8] : 0;
}

static bool force_allowed(char *why, size_t len)
{
    if (!s_img.in_op) {
        snprintf(why, len, "EtherCAT is not in OP.");
        return false;
    }
    if (s_l->n_do == 0) {
        snprintf(why, len, "The bus has no digital outputs.");
        return false;
    }
    if (hmi_shell_auto_mode()) {
        snprintf(why, len, "Switch the machine to Manual first (Main page).");
        return false;
    }
    return true;
}

/* ---- Channel rows ---- */

static void ch_row_style(ch_t *c, bool tappable)
{
    if (tappable) {
        lv_obj_add_style(c->row, &hmi_st_control, 0);
        lv_obj_add_style(c->row, &hmi_st_pressed, LV_STATE_PRESSED);
        lv_obj_add_flag(c->row, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_style(c->row, &hmi_st_control, 0);
        lv_obj_remove_style(c->row, &hmi_st_pressed, LV_STATE_PRESSED);
        lv_obj_remove_flag(c->row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c->row, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(c->row, 8, 0); /* black at 3 % */
        lv_obj_set_style_radius(c->row, 4, 0);
    }
}

static void ch_show(ch_t *c, bool on)
{
    if (c->state == (int8_t)on) {
        return;
    }
    c->state = (int8_t)on;
    hmi_lamp_set(c->lamp, on, HMI_SUCCESS);
    lv_obj_set_style_text_color(c->addr, HMI_C(on ? HMI_TEXT : HMI_TEXT_2), 0);
}

static void out_clicked_cb(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < s_io->n_out && s_img.force) {
        const ch_t *c = &s_io->out[i];
        (void)ecat_io_force_bit(c->coupler, c->bit, !ecat_io_bit(s_img.out[c->coupler], c->bit));
    }
}

static ch_t *ch_row(lv_obj_t *col, ch_t *c, char prefix, uint16_t number, const ecat_io_module_t *m, int channel)
{
    c->row = hmi_box(col, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_size(c->row, LV_PCT(100), CH_ROW_H);
    lv_obj_set_style_pad_hor(c->row, 10, 0);
    lv_obj_set_flex_align(c->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    c->lamp = hmi_lamp(c->row);
    char t[24];
    addr_text(prefix, number, t, sizeof(t));
    c->addr = hmi_label(c->row, t, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT_2);
    lv_obj_set_width(c->addr, 56);
    (void)m; /* the slot is in the card caption or the module heading */
    snprintf(t, sizeof(t), "ch %d", channel);
    lv_obj_t *name = hmi_label(c->row, t, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_flex_grow(name, 1);
    c->state = -1;
    ch_row_style(c, false);
    return c;
}

/* One module's channels: two columns, filled column by column like the mockup. */
static void module_grid(lv_obj_t *list, const ecat_io_module_t *m, uint8_t coupler, bool out)
{
    const int n = out ? m->dout : m->di;
    const int rows = (n + 1) / 2;
    lv_obj_t *grid = hmi_box(list, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_t *col[2];
    for (int k = 0; k < 2; k++) {
        col[k] = hmi_box(grid, LV_FLEX_FLOW_COLUMN, 4);
        lv_obj_set_flex_grow(col[k], 1);
    }
    for (int ch = 0; ch < n; ch++) {
        uint16_t *count = out ? &s_io->n_out : &s_io->n_in;
        if (*count >= MAX_CH) {
            return;
        }
        ch_t *c = out ? &s_io->out[*count] : &s_io->in[*count];
        c->coupler = coupler;
        c->bit = (uint16_t)((out ? m->out_bit : m->in_bit) + ch);
        ch_row(col[ch / rows], c, out ? 'Y' : 'X', (uint16_t)((out ? m->y_first : m->x_first) + ch), m, ch);
        if (out) {
            lv_obj_add_event_cb(c->row, out_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)*count);
        }
        (*count)++;
    }
}

static void io_card(lv_obj_t *body, bool out)
{
    lv_obj_t *card = hmi_card(body, 12, 14, 10);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    if (out) {
        s_io->out_card = card;
    }

    /* Modules of this direction, for the caption */
    int n_mod = 0;
    const ecat_io_module_t *only = NULL;
    uint8_t only_c = 0;
    for (uint8_t k = 0; k < s_l->n; k++) {
        for (uint8_t m = 0; m < s_l->c[k].n_modules; m++) {
            const ecat_io_module_t *mod = &s_l->c[k].mod[m];
            if (out ? mod->dout : mod->di) {
                n_mod++;
                only = mod;
                only_c = k;
            }
        }
    }

    lv_obj_t *title = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(title, out ? "Outputs" : "Inputs", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    char t[96], a0[12], a1[12];
    const int total = out ? s_l->n_do : s_l->n_di;
    if (n_mod == 1) {
        const int n = out ? only->dout : only->di;
        addr_text(out ? 'Y' : 'X', out ? only->y_first : only->x_first, a0, sizeof(a0));
        addr_text(out ? 'Y' : 'X', (uint16_t)((out ? only->y_first : only->x_first) + n - 1), a1, sizeof(a1));
        snprintf(t, sizeof(t), "%s slot %u \xC2\xB7 %s \xC2\xB7 %d %s \xC2\xB7 %s\xE2\x80\x93%s", s_l->c[only_c].name,
                 only->slot, only->name, n, out ? "DO" : "DI", a0, a1);
    } else {
        snprintf(t, sizeof(t), "%d module(s) \xC2\xB7 %d %s", n_mod, total, out ? "DO" : "DI");
    }
    lv_obj_t *cap = hmi_label(title, t, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_flex_grow(cap, 1);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *list = hmi_box(card, LV_FLEX_FLOW_COLUMN, 8);
    hmi_scroll_area(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    if (total == 0) {
        hmi_label(list, out ? "No digital outputs on the bus." : "No digital inputs on the bus.", HMI_FS_BODY, HMI_REGULAR,
                  HMI_TEXT_2);
        return;
    }
    for (uint8_t k = 0; k < s_l->n; k++) {
        for (uint8_t m = 0; m < s_l->c[k].n_modules; m++) {
            const ecat_io_module_t *mod = &s_l->c[k].mod[m];
            const int n = out ? mod->dout : mod->di;
            if (n == 0) {
                continue;
            }
            if (n_mod > 1) {
                addr_text(out ? 'Y' : 'X', out ? mod->y_first : mod->x_first, a0, sizeof(a0));
                addr_text(out ? 'Y' : 'X', (uint16_t)((out ? mod->y_first : mod->x_first) + n - 1), a1, sizeof(a1));
                snprintf(t, sizeof(t), "%s slot %u \xC2\xB7 %s \xC2\xB7 %s\xE2\x80\x93%s", s_l->c[k].name, mod->slot,
                         mod->name, a0, a1);
                hmi_label(list, t, HMI_FS_SMALL, HMI_SEMIBOLD, HMI_TEXT_2);
            }
            module_grid(list, mod, k, out);
        }
    }
    if ((out ? s_io->n_out : s_io->n_in) >= MAX_CH) {
        hmi_label(list, "More channels than this page shows (256).", HMI_FS_SMALL, HMI_REGULAR, HMI_CAUTION);
    }
}

/* ---- Analog ---- */

static void analog_card(lv_obj_t *list, uint8_t k, const ecat_io_module_t *m)
{
    if (s_io->n_an >= (int)(sizeof(s_io->an_mod) / sizeof(s_io->an_mod[0]))) {
        return;
    }
    const int idx = s_io->n_an++;
    s_io->an_mod[idx] = m;
    s_io->an_coupler[idx] = k;
    const bool in = m->ai > 0;

    lv_obj_t *c = hmi_card(list, 12, 14, 0);
    lv_obj_set_width(c, LV_PCT(100));
    char t[96];
    snprintf(t, sizeof(t), "%s slot %u \xC2\xB7 %s \xC2\xB7 %u %s", s_l->c[k].name, m->slot, m->name, in ? m->ai : m->ao,
             in ? "analog inputs" : "analog outputs");
    lv_obj_t *title = hmi_label(c, t, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_set_style_pad_bottom(title, 6, 0);

    lv_obj_t *h = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(h, LV_PCT(100), 24);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_width(hmi_label(h, "Channel", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), 120);
    lv_obj_set_width(hmi_label(h, "Raw value", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), 120);
    hmi_label(h, "Diagnosis", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    const int n = in ? m->ai : m->ao;
    for (int ch = 0; ch < n && ch < 4; ch++) {
        lv_obj_t *r = hmi_box(c, LV_FLEX_FLOW_ROW, 8);
        lv_obj_set_size(r, LV_PCT(100), 36);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        hmi_divider_top(r);
        snprintf(t, sizeof(t), "%s %d", in ? "AI" : "AO", ch);
        lv_obj_set_width(hmi_label(r, t, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT), 120);
        s_io->an_val[idx][ch] = hmi_label(r, "", HMI_FS_LARGE, HMI_REGULAR, HMI_TEXT);
        lv_obj_set_width(s_io->an_val[idx][ch], 120);
        s_io->an_diag[idx][ch] = hmi_label(r, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    }
}

static void analog_update(void)
{
    char t[24];
    for (int i = 0; i < s_io->n_an; i++) {
        const ecat_io_module_t *m = s_io->an_mod[i];
        const uint8_t k = s_io->an_coupler[i];
        const bool in = m->ai > 0;
        for (int ch = 0; ch < 4 && s_io->an_val[i][ch]; ch++) {
            /* AI: 4 INT then 4 USINT diagnoses in the inputs; AO: 4 INT outputs, 4 USINT diagnoses in. */
            const int16_t v = in ? img_i16(s_img.in[k], (uint16_t)(m->in_bit + 16 * ch))
                                 : img_i16(s_img.out[k], (uint16_t)(m->out_bit + 16 * ch));
            const uint8_t d = img_u8(s_img.in[k], (uint16_t)(m->in_bit + (in ? 64 : 0) + 8 * ch));
            snprintf(t, sizeof(t), "%d", v);
            hmi_set_text(s_io->an_val[i][ch], t);
            snprintf(t, sizeof(t), d ? "0x%02X" : "OK", d);
            hmi_set_text(s_io->an_diag[i][ch], t);
            lv_obj_set_style_text_color(s_io->an_diag[i][ch], HMI_C(d ? HMI_CRITICAL : HMI_TEXT_2), 0);
        }
    }
}

static void analog_view(lv_obj_t *body)
{
    lv_obj_t *list = hmi_box(body, LV_FLEX_FLOW_COLUMN, 12);
    hmi_scroll_area(list);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    for (uint8_t k = 0; k < s_l->n; k++) {
        for (uint8_t m = 0; m < s_l->c[k].n_modules; m++) {
            if (s_l->c[k].mod[m].ai || s_l->c[k].mod[m].ao) {
                analog_card(list, k, &s_l->c[k].mod[m]);
            }
        }
    }
    if (s_io->n_an == 0) {
        hmi_info_card(list, "No analog modules",
                      "The couplers on the bus have no SC-A0400-IV or SC-A0004-IV module. Values are raw INT counts "
                      "of the module's configured range.",
                      NULL, NULL);
    } else {
        lv_obj_t *note = hmi_label(list, "Raw INT counts: the scale depends on the range configured in the module.",
                                   HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
        lv_obj_set_width(note, LV_PCT(100));
    }
}

/* ---- EtherCAT ---- */

static void ethercat_view(lv_obj_t *body)
{
    lv_obj_t *list = hmi_box(body, LV_FLEX_FLOW_COLUMN, 12);
    hmi_scroll_area(list);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    char t[64];
    for (uint8_t k = 0; k < s_l->n; k++) {
        const ecat_io_coupler_t *c = &s_l->c[k];
        lv_obj_t *card = hmi_card(list, 12, 14, 0);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_t *title = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
        lv_obj_set_width(title, LV_PCT(100));
        lv_obj_set_style_pad_bottom(title, 6, 0);
        lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        snprintf(t, sizeof(t), "%s \xC2\xB7 slave %u \xC2\xB7 %u in + %u out bytes", c->name, c->slave, c->in_bytes,
                 c->out_bytes);
        hmi_label(title, t, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
        hmi_spacer(title);
        s_io->status_tag[k] = hmi_tag(title, true);

        static const char *const heads[] = {"Slot", "Module", "Channels", "Inputs", "Outputs"};
        static const int32_t widths[] = {56, 180, 220, 100, 100};
        lv_obj_t *h = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
        lv_obj_set_size(h, LV_PCT(100), 24);
        lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        for (int i = 0; i < 5; i++) {
            lv_obj_set_width(hmi_label(h, heads[i], HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), widths[i]);
        }
        for (uint8_t m = 0; m < c->n_modules; m++) {
            const ecat_io_module_t *mod = &c->mod[m];
            lv_obj_t *r = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
            lv_obj_set_size(r, LV_PCT(100), 36);
            lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            hmi_divider_top(r);
            char cells[5][40];
            snprintf(cells[0], sizeof(cells[0]), "%u", mod->slot);
            snprintf(cells[1], sizeof(cells[1]), "%s", mod->name);
            if (mod->di || mod->dout) {
                snprintf(cells[2], sizeof(cells[2]), "%u DI \xC2\xB7 %u DO", mod->di, mod->dout);
            } else if (mod->ai || mod->ao) {
                snprintf(cells[2], sizeof(cells[2]), "%u AI \xC2\xB7 %u AO", mod->ai, mod->ao);
            } else {
                snprintf(cells[2], sizeof(cells[2]), "not shown here");
            }
            snprintf(cells[3], sizeof(cells[3]), "%u bit", mod->in_bits);
            snprintf(cells[4], sizeof(cells[4]), "%u bit", mod->out_bits);
            for (int i = 0; i < 5; i++) {
                lv_obj_set_width(hmi_label(r, cells[i], 14, i == 1 ? HMI_SEMIBOLD : HMI_REGULAR, HMI_TEXT), widths[i]);
            }
        }
    }
}

static void ethercat_update(void)
{
    char t[48];
    for (uint8_t k = 0; k < s_l->n; k++) {
        if (s_io->status_tag[k] == NULL) {
            continue;
        }
        const uint32_t st = s_img.device_status[k];
        if (st) {
            snprintf(t, sizeof(t), "Device Status 0x%08" PRIX32, st);
            hmi_tag_set(s_io->status_tag[k], t, HMI_CRITICAL_BG, HMI_CRITICAL);
        } else {
            hmi_tag_set(s_io->status_tag[k], "Device Status OK", HMI_SUCCESS_BG, HMI_SUCCESS);
        }
    }
}

/* ---- Force mode ---- */

static void force_confirm_cb(bool ok, void *user)
{
    (void)user;
    char why[64];
    if (ok && s_io->root && force_allowed(why, sizeof(why))) {
        (void)ecat_io_force_mode(true);
    }
}

static void force_cb(lv_event_t *e)
{
    (void)e;
    if (s_img.force) {
        (void)ecat_io_force_mode(false);
        return;
    }
    char why[64];
    if (!force_allowed(why, sizeof(why))) {
        hmi_confirm("Force mode not available", why, "OK", false, NULL, NULL);
        return;
    }
    hmi_confirm("Force outputs?",
                "Outputs follow this page until force mode is switched off or you leave the page; then forced "
                "outputs switch off. Make sure nobody is inside the machine.",
                "Force outputs", true, force_confirm_cb, NULL);
}

static void force_update(void)
{
    const bool on = s_img.force;
    if (on == s_io->force_shown) {
        return;
    }
    s_io->force_shown = on;
    hmi_switch_set(s_io->force_sw, on);
    hmi_show(s_io->banner, on);
    if (s_io->out_card) {
        lv_obj_set_style_border_width(s_io->out_card, on ? 2 : 1, 0);
        lv_obj_set_style_border_color(s_io->out_card, on ? HMI_C(FORCE_BG) : lv_color_black(), 0);
        lv_obj_set_style_border_opa(s_io->out_card, on ? LV_OPA_COVER : HMI_STROKE_OPA, 0);
    }
    for (int i = 0; i < s_io->n_out; i++) {
        ch_row_style(&s_io->out[i], on);
    }
}

/* ---- Page ---- */

static void update(void)
{
    ecat_io_image_read(&s_img);
    if (s_img.gen != s_io->gen) {
        return; /* the layout changed; the next poll rebuilds */
    }
    force_update();
    for (int i = 0; i < s_io->n_in; i++) {
        ch_show(&s_io->in[i], ecat_io_bit(s_img.in[s_io->in[i].coupler], s_io->in[i].bit));
    }
    for (int i = 0; i < s_io->n_out; i++) {
        ch_show(&s_io->out[i], ecat_io_bit(s_img.out[s_io->out[i].coupler], s_io->out[i].bit));
    }
    analog_update();
    ethercat_update();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (ecat_io_layout_gen() != s_io->gen) {
        build();
        return;
    }
    update();
}

static void view_async(void *arg)
{
    if (s_io->root) {
        s_view = (int)(intptr_t)arg;
        build();
    }
}

static void view_cb(lv_event_t *e)
{
    lv_async_call(view_async, lv_event_get_user_data(e)); /* the pill is rebuilt too */
}

static void open_ethercat_cb(lv_event_t *e)
{
    (void)e;
    hmi_settings_open_ethercat();
}

static void header(void)
{
    lv_obj_t *row = hmi_page_header(s_io->root, "IO monitor", NULL, HMI_TEXT_2);
    lv_obj_t *pills = hmi_box(row, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_style_pad_left(pills, 4, 0);
    for (int v = 0; v < VIEW_COUNT; v++) {
        lv_obj_t *p = hmi_pill(pills, s_view_names[v], v == s_view, true);
        lv_obj_add_event_cb(p, view_cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
    }
    hmi_spacer(row);

    s_io->force_btn = hmi_control(row, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(s_io->force_btn, 16, 0);
    lv_obj_set_style_pad_column(s_io->force_btn, 14, 0);
    s_io->force_state = hmi_label(s_io->force_btn, "Force mode", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    s_io->force_sw = hmi_switch(s_io->force_btn, false);
    lv_obj_add_event_cb(s_io->force_btn, force_cb, LV_EVENT_CLICKED, NULL);

    s_io->banner = hmi_box(s_io->root, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_size(s_io->banner, LV_PCT(100), 44);
    lv_obj_set_style_pad_hor(s_io->banner, 12, 0);
    lv_obj_set_style_radius(s_io->banner, 8, 0);
    lv_obj_set_style_bg_color(s_io->banner, HMI_C(HMI_CAUTION_BG), 0);
    lv_obj_set_style_bg_opa(s_io->banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_io->banner, 1, 0);
    lv_obj_set_style_border_color(s_io->banner, HMI_C(HMI_CAUTION), 0);
    lv_obj_set_style_border_opa(s_io->banner, LV_OPA_20, 0);
    lv_obj_set_flex_align(s_io->banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_icon(s_io->banner, HMI_ICON_WARNING, 20, HMI_CAUTION);
    hmi_label(s_io->banner, "Force active", HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    hmi_label(s_io->banner, "Outputs are driven from the HMI. Tap an output to switch it.", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    hmi_show(s_io->banner, false);
}

static void build(void)
{
    lv_obj_t *page = s_io->page;
    lv_obj_t *root = s_io->root;
    lv_timer_t *poll = s_io->poll;
    memset(s_io, 0, sizeof(*s_io));
    s_io->page = page;
    s_io->root = root;
    s_io->poll = poll;
    lv_obj_clean(root);

    ecat_io_layout_read(s_l);
    ecat_io_image_read(&s_img);
    s_io->gen = s_l->gen;

    header();
    if (!s_l->in_op || s_l->n == 0) {
        hmi_info_card(s_io->root, "No remote IO",
                      s_l->in_op ? "The accepted topology has no IO coupler (R1EC)."
                                : "The EtherCAT bus is not in OP. Scan the bus and accept the topology in "
                                  "Settings \xE2\x80\xBA EtherCAT.",
                      "Open EtherCAT settings", open_ethercat_cb);
        return;
    }

    lv_obj_t *body = hmi_box(s_io->root, LV_FLEX_FLOW_ROW, 16);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    if (s_view == VIEW_DIGITAL) {
        io_card(body, false);
        io_card(body, true);
    } else if (s_view == VIEW_ANALOG) {
        analog_view(body);
    } else {
        ethercat_view(body);
    }
    s_io->force_shown = !s_img.force; /* force the first update to style everything */
    update();
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_io->poll) {
        lv_timer_delete(s_io->poll);
    }
    (void)ecat_io_force_mode(false); /* leaving the page ends force mode */
    memset(s_io, 0, sizeof(*s_io));
}

void hmi_page_io_create(lv_obj_t *page)
{
    if (s_io == NULL) {
        s_io = lv_malloc(sizeof(*s_io));
        s_l = lv_malloc(sizeof(*s_l));
        LV_ASSERT_MALLOC(s_io);
        LV_ASSERT_MALLOC(s_l);
    }
    memset(s_io, 0, sizeof(*s_io));
    s_io->page = page;
    s_io->root = hmi_box(page, LV_FLEX_FLOW_COLUMN, 12);
    lv_obj_set_size(s_io->root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(s_io->root, deleted_cb, LV_EVENT_DELETE, NULL);
    build();
    s_io->poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
