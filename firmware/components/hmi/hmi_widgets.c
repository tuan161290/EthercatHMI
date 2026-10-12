/*
 * Widgets of the mockup that several pages share: segmented control, header pills, tags, lamps,
 * toggle switch, info card, and the modal confirmation, numeric keypad (TimeSetting.dc.html) and
 * roller pickers (a list, date and time), all in the Fluent light theme of docs/HMI_THEME.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hmi_internal.h"

/* ---- Segmented control ---- */

typedef struct {
    hmi_seg_cb_t cb;
    void *user;
    int n;
    int selected;
    int px;
    bool enabled;
    lv_obj_t **btn;
} seg_t;

static void seg_style(const seg_t *s, int i)
{
    const bool on = i == s->selected;
    lv_obj_t *b = s->btn[i];
    lv_obj_t *label = lv_obj_get_child(b, 0);
    lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(b, on ? HMI_STROKE_OPA : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(label, hmi_font(s->px, on && s->enabled ? HMI_SEMIBOLD : HMI_REGULAR), 0);
    lv_obj_set_style_text_color(label, HMI_C(!s->enabled ? HMI_TEXT_OFF : on ? HMI_ACCENT : HMI_TEXT_2), 0);
    hmi_elevation(b, on && s->enabled ? HMI_ELEV_OPA : 0);
}

static void seg_clicked_cb(lv_event_t *e)
{
    seg_t *s = lv_event_get_user_data(e);
    const int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (!s->enabled) {
        return;
    }
    const int old = s->selected;
    s->selected = i;
    if (old >= 0 && old < s->n) {
        seg_style(s, old);
    }
    seg_style(s, i);
    if (s->cb) {
        s->cb(i, s->user);
    }
}

static void seg_deleted_cb(lv_event_t *e)
{
    seg_t *s = lv_event_get_user_data(e);
    lv_free(s->btn);
    lv_free(s);
}

lv_obj_t *hmi_segmented(lv_obj_t *parent, const char *const *labels, int n, int per_row, int32_t h, int selected,
                        hmi_seg_cb_t cb, void *user)
{
    seg_t *s = lv_malloc_zeroed(sizeof(*s));
    LV_ASSERT_MALLOC(s);
    s->btn = lv_malloc_zeroed(sizeof(lv_obj_t *) * (size_t)(n > 0 ? n : 1));
    LV_ASSERT_MALLOC(s->btn);
    s->cb = cb;
    s->user = user;
    s->n = n;
    s->selected = selected;
    s->px = h >= 44 ? HMI_FS_BODY : HMI_FS_SMALL;
    s->enabled = true;

    lv_obj_t *track = hmi_box(parent, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_width(track, LV_PCT(100));
    lv_obj_set_style_pad_all(track, 3, 0);
    lv_obj_set_style_radius(track, 6, 0);
    lv_obj_set_style_bg_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(track, HMI_TINT_OPA, 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_border_color(track, lv_color_black(), 0);
    lv_obj_set_style_border_opa(track, HMI_STROKE_OPA, 0);
    lv_obj_set_user_data(track, s);
    lv_obj_add_event_cb(track, seg_deleted_cb, LV_EVENT_DELETE, s);

    const int cols = per_row > 0 ? per_row : (n > 0 ? n : 1);
    lv_obj_t *row = NULL;
    for (int i = 0; i < n; i++) {
        if (i % cols == 0) {
            row = hmi_box(track, LV_FLEX_FLOW_ROW, 4);
            lv_obj_set_width(row, LV_PCT(100));
        }
        lv_obj_t *b = hmi_box(row, LV_FLEX_FLOW_ROW, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_height(b, h);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_color(b, HMI_C(HMI_WHITE), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_black(), 0);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, seg_clicked_cb, LV_EVENT_CLICKED, s);
        hmi_label(b, labels[i], s->px, HMI_REGULAR, HMI_TEXT_2);
        s->btn[i] = b;
        seg_style(s, i);
    }
    /* Keep the segments of a short last row as wide as the others. */
    for (int i = n; per_row > 0 && i % cols != 0; i++) {
        lv_obj_t *pad = hmi_box(row, -1, 0);
        lv_obj_set_height(pad, h);
        lv_obj_set_flex_grow(pad, 1);
    }
    return track;
}

void hmi_segmented_select(lv_obj_t *seg, int index)
{
    seg_t *s = lv_obj_get_user_data(seg);
    if (s == NULL || index == s->selected) {
        return;
    }
    const int old = s->selected;
    s->selected = index;
    if (old >= 0 && old < s->n) {
        seg_style(s, old);
    }
    if (index >= 0 && index < s->n) {
        seg_style(s, index);
    }
}

void hmi_segmented_set_enabled(lv_obj_t *seg, bool enabled)
{
    seg_t *s = lv_obj_get_user_data(seg);
    if (s == NULL || s->enabled == enabled) {
        return;
    }
    s->enabled = enabled;
    for (int i = 0; i < s->n; i++) {
        seg_style(s, i);
    }
}

/* ---- Pills, tags, lamps ---- */

lv_obj_t *hmi_pill(lv_obj_t *parent, const char *text, bool selected, bool enabled)
{
    lv_obj_t *p = hmi_box(parent, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_height(p, 48);
    lv_obj_set_style_pad_hor(p, 22, 0);
    lv_obj_set_style_radius(p, 24, 0);
    lv_obj_set_style_bg_color(p, HMI_C(HMI_WHITE), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, selected ? HMI_C(HMI_ACCENT) : lv_color_black(), 0);
    lv_obj_set_style_border_opa(p, selected ? LV_OPA_COVER : LV_OPA_20, 0);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (enabled && !selected) {
        lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_style(p, &hmi_st_pressed, LV_STATE_PRESSED);
        hmi_elevation(p, HMI_ELEV_OPA);
    }
    hmi_label(p, text, HMI_FS_BODY, selected ? HMI_SEMIBOLD : HMI_REGULAR,
              selected ? HMI_ACCENT : enabled ? HMI_TEXT : HMI_TEXT_OFF);
    return p;
}

lv_obj_t *hmi_tag(lv_obj_t *parent, bool dot)
{
    lv_obj_t *t = hmi_box(parent, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_height(t, 30);
    lv_obj_set_style_pad_hor(t, 12, 0);
    lv_obj_set_style_radius(t, 15, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (dot) {
        lv_obj_t *d = hmi_box(t, -1, 0);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    }
    hmi_label(t, "", HMI_FS_SMALL, HMI_SEMIBOLD, HMI_TEXT_2);
    return t;
}

void hmi_tag_set(lv_obj_t *tag, const char *text, uint32_t bg, uint32_t fg)
{
    const uint32_t n = lv_obj_get_child_count(tag);
    lv_obj_t *label = lv_obj_get_child(tag, (int32_t)n - 1);
    lv_obj_set_style_bg_color(tag, HMI_C(bg), 0);
    if (bg == HMI_WHITE) {
        lv_obj_set_style_bg_color(tag, lv_color_black(), 0); /* neutral: black at 6 % */
        lv_obj_set_style_bg_opa(tag, HMI_STROKE_OPA, 0);
    } else {
        lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
    }
    if (n > 1) {
        lv_obj_set_style_bg_color(lv_obj_get_child(tag, 0), HMI_C(fg), 0);
    }
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
    lv_obj_set_style_text_color(label, HMI_C(fg), 0);
}

lv_obj_t *hmi_lamp(lv_obj_t *parent)
{
    lv_obj_t *l = hmi_box(parent, -1, 0);
    lv_obj_set_size(l, 16, 16);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(l, HMI_C(HMI_RING), 0);
    hmi_lamp_set(l, false, HMI_SUCCESS);
    return l;
}

void hmi_lamp_set(lv_obj_t *lamp, bool on, uint32_t color)
{
    lv_obj_set_style_bg_color(lamp, HMI_C(color), 0);
    lv_obj_set_style_bg_opa(lamp, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lamp, on ? 0 : 2, 0);
}

lv_obj_t *hmi_card_title(lv_obj_t *card, const char *text)
{
    return hmi_label(card, text, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
}

/* ---- Modal dialogs on the top layer ---- */

static lv_obj_t *modal_open(int32_t w, lv_obj_t **card)
{
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE); /* nothing below takes a touch while it is open */
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_30, 0);
    *card = hmi_card(ov, 20, 24, 8);
    lv_obj_set_width(*card, w);
    lv_obj_set_style_pad_bottom(*card, 24, 0);
    lv_obj_center(*card);
    return ov;
}

typedef struct {
    hmi_confirm_cb_t cb;
    void *user;
    lv_obj_t *ov;
} confirm_t;

static void confirm_close(confirm_t *c, bool ok)
{
    hmi_confirm_cb_t cb = c->cb;
    void *user = c->user;
    c->cb = NULL;
    lv_obj_delete_async(c->ov); /* frees `c` too */
    if (cb) {
        cb(ok, user);
    }
}

static void confirm_ok_cb(lv_event_t *e)
{
    confirm_close(lv_event_get_user_data(e), true);
}

static void confirm_cancel_cb(lv_event_t *e)
{
    confirm_close(lv_event_get_user_data(e), false);
}

static void free_user_data_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static lv_obj_t *dialog_button(lv_obj_t *row, const char *text, bool filled, uint32_t color, int32_t w)
{
    lv_obj_t *b;
    if (filled) {
        b = hmi_filled_button(row, HMI_TOUCH_H, color, 4);
        lv_obj_set_width(b, w);
        hmi_label(b, text, HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
    } else {
        b = hmi_control(row, w, HMI_TOUCH_H);
        hmi_label(b, text, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    }
    return b;
}

void hmi_confirm(const char *title, const char *body, const char *ok_text, bool danger, hmi_confirm_cb_t cb,
                 void *user)
{
    confirm_t *c = lv_malloc_zeroed(sizeof(*c));
    LV_ASSERT_MALLOC(c);
    c->cb = cb;
    c->user = user;
    lv_obj_t *card;
    c->ov = modal_open(560, &card);
    lv_obj_add_event_cb(c->ov, free_user_data_cb, LV_EVENT_DELETE, c);

    hmi_label(card, title, HMI_FS_HEAD, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_t *text = hmi_label(card, body, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
    lv_obj_set_width(text, LV_PCT(100));

    lv_obj_t *row = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_style_pad_top(row, 12, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(dialog_button(row, "Cancel", false, 0, 120), confirm_cancel_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(dialog_button(row, ok_text, true, danger ? HMI_CRITICAL : HMI_ACCENT, 160), confirm_ok_cb,
                        LV_EVENT_CLICKED, c);
}

/* ---- Numeric keypad ---- */

#define KP_BUF 16

typedef struct {
    hmi_keypad_cb_t cb;
    void *user;
    lv_obj_t *ov;
    lv_obj_t *value;
    lv_obj_t *err;
    double min, max;
    int decimals;
    char buf[KP_BUF];
} keypad_t;

static void kp_show(keypad_t *k)
{
    lv_label_set_text(k->value, k->buf[0] ? k->buf : "0");
    lv_label_set_text(k->err, "");
}

static void kp_key_cb(lv_event_t *e)
{
    keypad_t *k = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    const char *key = lv_label_get_text(lv_obj_get_child(btn, 0));
    size_t len = strlen(k->buf);

    if (strcmp(key, "OK") == 0) {
        char *end;
        double v = strtod(k->buf, &end);
        if (k->buf[0] == '\0' || *end != '\0') {
            lv_label_set_text(k->err, "Enter a value");
        } else if (v < k->min || v > k->max) {
            lv_label_set_text(k->err, "Out of range");
        } else {
            hmi_keypad_cb_t cb = k->cb;
            void *user = k->user;
            lv_obj_delete_async(k->ov);
            if (cb) {
                cb(v, user);
            }
        }
        return;
    }
    if (strcmp(key, "Cancel") == 0) {
        lv_obj_delete_async(k->ov);
        return;
    }
    if (strcmp(key, "Del") == 0) {
        if (len) {
            k->buf[len - 1] = '\0';
        }
    } else if (strcmp(key, "C") == 0) {
        k->buf[0] = '\0';
    } else if (strcmp(key, "\xC2\xB1") == 0) { /* +/- */
        if (k->buf[0] == '-') {
            memmove(k->buf, k->buf + 1, len);
        } else if (len + 1 < KP_BUF && k->min < 0) {
            memmove(k->buf + 1, k->buf, len + 1);
            k->buf[0] = '-';
        }
    } else if (strcmp(key, ".") == 0) {
        if (k->decimals > 0 && strchr(k->buf, '.') == NULL && len + 2 < KP_BUF) {
            strcat(k->buf, len == 0 || strcmp(k->buf, "-") == 0 ? "0." : ".");
        }
    } else if (len + 1 < KP_BUF) {
        const char *dot = strchr(k->buf, '.');
        if (dot == NULL || (int)strlen(dot + 1) < k->decimals) {
            if (strcmp(k->buf, "0") == 0) {
                k->buf[0] = '\0';
            } else if (strcmp(k->buf, "-0") == 0) {
                k->buf[1] = '\0';
            }
            strcat(k->buf, key);
        }
    }
    kp_show(k);
}

void hmi_keypad(const char *title, const char *unit, double current, double min, double max, int decimals,
                hmi_keypad_cb_t cb, void *user)
{
    static const char *const keys[] = {
        "7", "8", "9", "Del", "4", "5", "6", "C", "1", "2", "3", "\xC2\xB1", "0", ".", "Cancel", "OK",
    };
    keypad_t *k = lv_malloc_zeroed(sizeof(*k));
    LV_ASSERT_MALLOC(k);
    k->cb = cb;
    k->user = user;
    k->min = min;
    k->max = max;
    k->decimals = decimals;
    snprintf(k->buf, sizeof(k->buf), "%.*f", decimals, current);

    lv_obj_t *card;
    k->ov = modal_open(440, &card);
    lv_obj_add_event_cb(k->ov, free_user_data_cb, LV_EVENT_DELETE, k);

    hmi_label(card, title, HMI_FS_HEAD, HMI_SEMIBOLD, HMI_TEXT);
    char sub[96];
    snprintf(sub, sizeof(sub), "Current %.*f %s \xC2\xB7 Range %.*f to %.*f", decimals, current, unit, decimals, min,
             decimals, max);
    hmi_label(card, sub, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);

    lv_obj_t *box = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_size(box, LV_PCT(100), 64);
    lv_obj_set_style_pad_hor(box, 16, 0);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_bg_color(box, HMI_C(HMI_WHITE), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, HMI_C(HMI_ACCENT), 0);
    lv_obj_set_style_pad_top(card, 20, 0);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    k->value = hmi_label(box, "", HMI_FS_BIG, HMI_SEMIBOLD, HMI_TEXT);
    hmi_label(box, unit, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    k->err = hmi_label(card, "", HMI_FS_SMALL, HMI_SEMIBOLD, HMI_CRITICAL);
    lv_obj_set_style_min_height(k->err, 16, 0);

    lv_obj_t *row = NULL;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (i % 4 == 0) {
            row = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
            lv_obj_set_width(row, LV_PCT(100));
        }
        const bool ok = strcmp(keys[i], "OK") == 0;
        lv_obj_t *b;
        if (ok) {
            b = hmi_filled_button(row, 60, HMI_ACCENT, 4);
            lv_obj_set_width(b, 0);
            hmi_label(b, keys[i], HMI_FS_BODY, HMI_SEMIBOLD, HMI_WHITE);
        } else {
            b = hmi_control(row, 0, 60);
            const bool digit = keys[i][0] >= '0' && keys[i][0] <= '9';
            hmi_label(b, keys[i], digit ? HMI_FS_HEAD : HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
        }
        lv_obj_set_flex_grow(b, 1);
        lv_obj_add_event_cb(b, kp_key_cb, LV_EVENT_CLICKED, k);
    }
    kp_show(k);
}

/* ---- Text entry with an on-screen keyboard ---- */

#define TI_CARD_W     720
#define TI_KB_H       290
#define TI_KB_BG      0xE6E6E6
#define TI_KEY_PRESS  0xDADADA

typedef struct {
    hmi_text_cb_t cb;
    void *user;
    lv_obj_t *ov, *ta, *kb, *err, *show_label;
    const char *const *map;
    int min_len;
} text_input_t;

/* Open Sans has no arrow or keyboard symbols, so the special keys are words. Printable ASCII:
 * letters and digits on the letter maps, the 32 punctuation characters on the symbol map. */
static const char *const kb_lower[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Del", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "@", "\n",
    "Shift", "z", "x", "c", "v", "b", "n", "m", ".", "-", "_", "\n",
    "#+=", "Space", "",
};
static const char *const kb_upper[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Del", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "@", "\n",
    "Shift", "Z", "X", "C", "V", "B", "N", "M", ".", "-", "_", "\n",
    "#+=", "Space", "",
};
static const char *const kb_symbols[] = {
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "Del", "\n",
    "~", "`", "|", "\\", "/", "?", "<", ">", "[", "]", "\n",
    "{", "}", "=", "+", "-", "_", ":", ";", "'", "\"", "\n",
    "abc", ",", ".", "Space", "",
};

/* Keys type on release (CLICK_TRIG), so a press that slides off is dropped; only Del repeats. */
static void kb_set_map(text_input_t *t, const char *const *map)
{
    t->map = map;
    lv_buttonmatrix_set_map(t->kb, map);
    lv_buttonmatrix_set_button_ctrl_all(t->kb, LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    uint32_t id = 0;
    for (int i = 0; map[i][0] != '\0'; i++) {
        const char *k = map[i];
        if (strcmp(k, "\n") == 0) {
            continue;
        }
        if (strcmp(k, "Space") == 0) {
            lv_buttonmatrix_set_button_width(t->kb, id, 6);
        } else if (strcmp(k, "Del") == 0 || strcmp(k, "Shift") == 0 || strcmp(k, "#+=") == 0 ||
                   strcmp(k, "abc") == 0) {
            lv_buttonmatrix_set_button_width(t->kb, id, 2);
        }
        if (strcmp(k, "Del") == 0) {
            lv_buttonmatrix_clear_button_ctrl(t->kb, id, LV_BUTTONMATRIX_CTRL_NO_REPEAT);
        }
        if (strcmp(k, "Shift") == 0 && map == kb_upper) {
            lv_buttonmatrix_set_button_ctrl(t->kb, id, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
        id++;
    }
}

static void kb_cb(lv_event_t *e)
{
    text_input_t *t = lv_event_get_user_data(e);
    const char *k = lv_buttonmatrix_get_button_text(t->kb, lv_buttonmatrix_get_selected_button(t->kb));
    if (k == NULL) {
        return;
    }
    lv_label_set_text(t->err, "");
    if (strcmp(k, "Del") == 0) {
        lv_textarea_delete_char(t->ta);
    } else if (strcmp(k, "Shift") == 0) {
        kb_set_map(t, t->map == kb_upper ? kb_lower : kb_upper);
    } else if (strcmp(k, "#+=") == 0) {
        kb_set_map(t, kb_symbols);
    } else if (strcmp(k, "abc") == 0) {
        kb_set_map(t, kb_lower);
    } else {
        lv_textarea_add_text(t->ta, strcmp(k, "Space") == 0 ? " " : k);
        if (t->map == kb_upper) {
            kb_set_map(t, kb_lower); /* Shift holds for one letter */
        }
    }
}

static void ti_close(text_input_t *t, const char *text)
{
    hmi_text_cb_t cb = t->cb;
    void *user = t->user;
    t->cb = NULL;
    lv_obj_delete_async(t->ov); /* frees `t`; the text stays valid until then */
    if (cb) {
        cb(text, user);
    }
}

static void ti_ok_cb(lv_event_t *e)
{
    text_input_t *t = lv_event_get_user_data(e);
    const char *text = lv_textarea_get_text(t->ta);
    if ((int)strlen(text) < t->min_len) {
        lv_label_set_text_fmt(t->err, "Enter at least %d characters", t->min_len);
        return;
    }
    ti_close(t, text);
}

static void ti_cancel_cb(lv_event_t *e)
{
    ti_close(lv_event_get_user_data(e), NULL);
}

static void ti_show_cb(lv_event_t *e)
{
    text_input_t *t = lv_event_get_user_data(e);
    const bool hidden = lv_textarea_get_password_mode(t->ta);
    lv_textarea_set_password_mode(t->ta, !hidden);
    lv_label_set_text(t->show_label, hidden ? "Hide" : "Show");
}

void hmi_text_input(const char *title, const char *hint, const char *initial, bool password, int min_len,
                    int max_len, hmi_text_cb_t cb, void *user)
{
    text_input_t *t = lv_malloc_zeroed(sizeof(*t));
    LV_ASSERT_MALLOC(t);
    t->cb = cb;
    t->user = user;
    t->min_len = min_len;

    t->ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(t->ov);
    lv_obj_set_size(t->ov, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(t->ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t->ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(t->ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(t->ov, LV_OPA_30, 0);
    lv_obj_add_event_cb(t->ov, free_user_data_cb, LV_EVENT_DELETE, t);

    lv_obj_t *card = hmi_card(t->ov, 20, 24, 8);
    lv_obj_set_width(card, TI_CARD_W);
    lv_obj_set_style_pad_bottom(card, 20, 0);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 16);
    hmi_label(card, title, HMI_FS_HEAD, HMI_SEMIBOLD, HMI_TEXT);
    if (hint && hint[0]) {
        lv_obj_t *h = hmi_label(card, hint, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
        lv_obj_set_width(h, LV_PCT(100));
    }

    lv_obj_t *row = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    t->ta = lv_textarea_create(row);
    lv_obj_remove_style_all(t->ta);
    lv_obj_set_height(t->ta, HMI_TOUCH_H);
    lv_obj_set_flex_grow(t->ta, 1);
    lv_textarea_set_one_line(t->ta, true);
    lv_textarea_set_max_length(t->ta, (uint32_t)max_len);
    lv_textarea_set_password_mode(t->ta, password);
    lv_textarea_set_text(t->ta, initial ? initial : "");
    lv_obj_set_style_text_font(t->ta, hmi_font(HMI_FS_HEAD, HMI_REGULAR), 0);
    lv_obj_set_style_text_color(t->ta, HMI_C(HMI_TEXT), 0);
    lv_obj_set_style_bg_color(t->ta, HMI_C(HMI_WHITE), 0);
    lv_obj_set_style_bg_opa(t->ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t->ta, 4, 0);
    lv_obj_set_style_border_width(t->ta, 1, 0);
    lv_obj_set_style_border_color(t->ta, HMI_C(HMI_ACCENT), 0);
    lv_obj_set_style_pad_hor(t->ta, 12, 0);
    lv_obj_set_style_pad_ver(t->ta, 9, 0);
    lv_obj_set_style_border_side(t->ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_width(t->ta, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_color(t->ta, HMI_C(HMI_TEXT), LV_PART_CURSOR);
    lv_obj_set_style_border_opa(t->ta, LV_OPA_COVER, LV_PART_CURSOR);
    if (password) {
        lv_obj_t *show = hmi_control(row, 96, HMI_TOUCH_H);
        t->show_label = hmi_label(show, "Show", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
        lv_obj_add_event_cb(show, ti_show_cb, LV_EVENT_CLICKED, t);
    }

    lv_obj_t *bottom = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(bottom, LV_PCT(100));
    lv_obj_set_style_pad_top(bottom, 4, 0);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    t->err = hmi_label(bottom, "", HMI_FS_SMALL, HMI_SEMIBOLD, HMI_CRITICAL);
    lv_obj_set_flex_grow(t->err, 1);
    lv_obj_add_event_cb(dialog_button(bottom, "Cancel", false, 0, 120), ti_cancel_cb, LV_EVENT_CLICKED, t);
    lv_obj_add_event_cb(dialog_button(bottom, "OK", true, HMI_ACCENT, 160), ti_ok_cb, LV_EVENT_CLICKED, t);

    t->kb = lv_buttonmatrix_create(t->ov);
    lv_obj_remove_style_all(t->kb);
    lv_obj_set_size(t->kb, LV_PCT(100), TI_KB_H);
    lv_obj_align(t->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(t->kb, HMI_C(TI_KB_BG), 0);
    lv_obj_set_style_bg_opa(t->kb, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(t->kb, 8, 0);
    lv_obj_set_style_pad_row(t->kb, 6, 0);
    lv_obj_set_style_pad_column(t->kb, 6, 0);
    lv_obj_set_style_bg_color(t->kb, HMI_C(HMI_CONTROL), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(t->kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(t->kb, 4, LV_PART_ITEMS);
    lv_obj_set_style_border_width(t->kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(t->kb, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_border_opa(t->kb, HMI_STROKE_OPA, LV_PART_ITEMS);
    lv_obj_set_style_text_font(t->kb, hmi_font(HMI_FS_HEAD, HMI_REGULAR), LV_PART_ITEMS);
    lv_obj_set_style_text_color(t->kb, HMI_C(HMI_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(t->kb, HMI_C(TI_KEY_PRESS), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(t->kb, HMI_C(HMI_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(t->kb, HMI_C(HMI_WHITE), LV_PART_ITEMS | LV_STATE_CHECKED);
    kb_set_map(t, kb_lower);
    lv_obj_add_event_cb(t->kb, kb_cb, LV_EVENT_VALUE_CHANGED, t);
}

/* ---- Small helpers ---- */

void hmi_show(lv_obj_t *obj, bool show)
{
    if (obj == NULL) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- Touch: a press that slides off its control is cancelled ---- */

/* As on Windows: lifting the finger outside the control does not click it, hold-to-run buttons
 * (jog, Go to) stop as soon as the finger leaves, and the finger does not press what it slides
 * onto. LVGL keeps the pressed object while the finger moves (LV_OBJ_FLAG_PRESS_LOCK, on by
 * default) and clicks it wherever the finger lifts (user report, 2026-10-11). */
#define SLIDE_OFF_MARGIN 12 /* px a finger may stray outside; more than LVGL's 10 px scroll limit */

static void slide_off_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || lv_indev_get_active_obj() != obj) {
        lv_obj_remove_event_cb(obj, slide_off_cb);
        return;
    }
    if (lv_indev_get_scroll_obj(indev)) {
        return; /* a scroll: LVGL sends no click after it */
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (p.x >= a.x1 - SLIDE_OFF_MARGIN && p.x <= a.x2 + SLIDE_OFF_MARGIN && p.y >= a.y1 - SLIDE_OFF_MARGIN &&
        p.y <= a.y2 + SLIDE_OFF_MARGIN) {
        return;
    }
    lv_obj_remove_event_cb(obj, slide_off_cb);
    lv_obj_remove_state(obj, LV_STATE_PRESSED);
    lv_indev_wait_release(indev);                       /* no click, nothing else pressed until it lifts */
    lv_obj_send_event(obj, LV_EVENT_PRESS_LOST, indev); /* hold-to-run buttons stop now */
}

/* Pointer events of the touch screen: the pressed object gets the slide-off check. */
static void touch_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_param(e);
    if (obj == NULL) {
        return;
    }
    lv_obj_remove_event_cb(obj, slide_off_cb);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED && !lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_event_cb(obj, slide_off_cb, LV_EVENT_PRESSING, NULL); /* sliders: dragging off is normal */
    }
}

void hmi_touch_slide_cancel(lv_indev_t *indev)
{
    if (indev) {
        lv_indev_add_event_cb(indev, touch_event_cb, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(indev, touch_event_cb, LV_EVENT_RELEASED, NULL);
    }
}

void hmi_fmt_us(uint32_t us, char *out, size_t len)
{
    if (us <= 100) {
        snprintf(out, len, "%u \xC2\xB5s", (unsigned)us);
    } else if (us < 10000) {
        snprintf(out, len, "%.2f ms", us / 1000.0);
    } else {
        snprintf(out, len, "%.1f ms", us / 1000.0);
    }
}

void hmi_set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void hmi_divider_top(lv_obj_t *obj)
{
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    lv_obj_set_style_border_opa(obj, HMI_STROKE_OPA, 0);
}

void hmi_segmented_set_text(lv_obj_t *seg, int index, const char *text)
{
    seg_t *s = lv_obj_get_user_data(seg);
    if (s && index >= 0 && index < s->n) {
        hmi_set_text(lv_obj_get_child(s->btn[index], 0), text);
    }
}

lv_obj_t *hmi_unsaved_tag(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = hmi_tag(parent, true);
    hmi_tag_set(t, text, HMI_CAUTION_BG, HMI_CAUTION);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    return t;
}

lv_obj_t *hmi_info_card(lv_obj_t *parent, const char *title, const char *text, const char *button,
                        lv_event_cb_t button_cb)
{
    lv_obj_t *c = hmi_card(parent, 14, 16, 12);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_t *row = hmi_box(c, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    hmi_icon(row, HMI_ICON_INFO, 20, HMI_ACCENT);
    lv_obj_t *col = hmi_box(row, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_flex_grow(col, 1);
    hmi_label(col, title, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
    lv_obj_t *t = hmi_label(col, text, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(t, LV_PCT(100));
    if (button) {
        lv_obj_t *b = hmi_control(c, LV_SIZE_CONTENT, 44);
        lv_obj_set_style_pad_hor(b, 16, 0);
        hmi_label(b, button, HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
        if (button_cb) {
            lv_obj_add_event_cb(b, button_cb, LV_EVENT_CLICKED, NULL);
        }
    }
    return c;
}

/* ---- Toggle switch ---- */

lv_obj_t *hmi_switch(lv_obj_t *parent, bool on)
{
    lv_obj_t *track = hmi_box(parent, -1, 0);
    lv_obj_set_size(track, 52, 28);
    lv_obj_set_style_radius(track, 14, 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_t *knob = hmi_box(track, -1, 0);
    lv_obj_set_size(knob, 18, 18);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    hmi_switch_set(track, on);
    return track;
}

void hmi_switch_set(lv_obj_t *sw, bool on)
{
    lv_obj_t *knob = lv_obj_get_child(sw, 0);
    lv_obj_set_style_bg_color(sw, HMI_C(HMI_ACCENT), 0);
    lv_obj_set_style_bg_opa(sw, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(sw, on ? HMI_C(HMI_ACCENT) : lv_color_black(), 0);
    lv_obj_set_style_border_opa(sw, on ? LV_OPA_COVER : 140, 0); /* off: black at 55 % */
    lv_obj_set_style_bg_color(knob, HMI_C(on ? HMI_WHITE : HMI_TEXT_2), 0);
    lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -4 : 4, 0);
}

/* ---- Drop-down list (combo box) ---- */

#define DD_ROW_H    52 /* list rows: HMI_TOUCH_H */
#define DD_MAX_ROWS 6  /* more options scroll */

static void dd_changed_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_current_target_obj(e);
    hmi_dropdown_cb_t cb = (hmi_dropdown_cb_t)lv_event_get_user_data(e);
    if (cb) {
        cb((int)lv_dropdown_get_selected(dd), lv_obj_get_user_data(dd));
    }
}

lv_obj_t *hmi_dropdown(lv_obj_t *parent, int32_t w, const char *options, int selected, hmi_dropdown_cb_t cb,
                       void *user)
{
    const lv_font_t *font = hmi_font(HMI_FS_BODY, HMI_REGULAR);
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_remove_style_all(dd);
    lv_obj_remove_flag(dd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(dd, &hmi_st_control, 0);
    lv_obj_add_style(dd, &hmi_st_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(dd, w, HMI_TOUCH_H);
    lv_obj_set_style_pad_left(dd, 14, 0);
    lv_obj_set_style_pad_right(dd, 14 + HMI_ICON_BTN + 8, 0); /* room for the chevron */
    lv_obj_set_style_text_font(dd, font, 0);
    lv_obj_set_style_text_align(dd, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(dd, HMI_C(HMI_TEXT_OFF), LV_STATE_DISABLED);
    lv_dropdown_set_symbol(dd, NULL);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, (uint32_t)(selected < 0 ? 0 : selected));
    hmi_elevation(dd, HMI_ELEV_OPA);
    lv_obj_t *chev = hmi_icon(dd, HMI_ICON_CHEVRON_DOWN, HMI_ICON_BTN, HMI_TEXT_2);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, HMI_ICON_BTN + 8, 0);
    lv_obj_set_user_data(dd, user);
    lv_obj_add_event_cb(dd, dd_changed_cb, LV_EVENT_VALUE_CHANGED, (void *)cb);

    /* The list (a child of the screen while open): a white card with one touch-sized row per
     * option. Its selection box is font height + line space centred on each line, so the
     * padding is half a line space plus a little, or the first row's box would be cut. */
    const int32_t line_space = DD_ROW_H - lv_font_get_line_height(font);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_remove_style_all(list);
    lv_obj_add_style(list, &hmi_st_card, 0);
    lv_obj_set_style_pad_ver(list, line_space / 2 + 4, 0);
    lv_obj_set_style_pad_hor(list, 14, 0);
    lv_obj_set_style_max_height(list, DD_ROW_H * DD_MAX_ROWS + 8, 0);
    lv_obj_set_style_text_font(list, font, 0);
    lv_obj_set_style_text_color(list, HMI_C(HMI_TEXT), 0);
    lv_obj_set_style_text_line_space(list, line_space, 0);
    lv_obj_set_style_text_font(list, font, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, line_space, LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, HMI_C(HMI_TEXT), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(list, lv_color_black(), LV_PART_SELECTED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(list, HMI_TINT_OPA * 2, LV_PART_SELECTED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(list, HMI_C(HMI_ACCENT), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, HMI_C(HMI_WHITE), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(list, lv_color_black(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_30, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(list, 4, LV_PART_SCROLLBAR);
    return dd;
}

void hmi_dropdown_set_enabled(lv_obj_t *dd, bool enabled)
{
    if (enabled == !lv_obj_has_state(dd, LV_STATE_DISABLED)) {
        return;
    }
    if (!enabled) {
        lv_dropdown_close(dd);
    }
    lv_obj_set_state(dd, LV_STATE_DISABLED, !enabled);
    hmi_elevation(dd, enabled ? HMI_ELEV_OPA : 0);
}

/* ---- Rollers: choice and date/time pickers ---- */

#define ROLLER_ROWS 5

static lv_obj_t *roller(lv_obj_t *parent, const char *options, int selected, int32_t w)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_obj_set_width(r, w);
    lv_obj_set_style_text_font(r, hmi_font(HMI_FS_HEAD, HMI_REGULAR), 0);
    lv_obj_set_style_text_color(r, HMI_C(HMI_TEXT_2), 0);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(r, 14, 0);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, HMI_TINT_OPA, 0);
    lv_obj_set_style_radius(r, 4, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_black(), 0);
    lv_obj_set_style_border_opa(r, HMI_STROKE_OPA, 0);
    lv_obj_set_style_anim_duration(r, 150, 0);
    lv_obj_set_style_bg_color(r, HMI_C(HMI_ACCENT), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, HMI_C(HMI_WHITE), LV_PART_SELECTED);
    lv_obj_set_style_text_font(r, hmi_font(HMI_FS_HEAD, HMI_SEMIBOLD), LV_PART_SELECTED);
    lv_obj_set_style_radius(r, 4, LV_PART_SELECTED);
    lv_roller_set_options(r, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, ROLLER_ROWS);
    lv_roller_set_selected(r, (uint32_t)(selected < 0 ? 0 : selected), LV_ANIM_OFF);
    return r;
}

/* Column of a picker: caption above the roller. */
static lv_obj_t *roller_column(lv_obj_t *row, const char *caption, const char *options, int selected, int32_t w)
{
    lv_obj_t *col = hmi_box(row, LV_FLEX_FLOW_COLUMN, 4);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_label(col, caption, HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    return roller(col, options, selected, w);
}

typedef struct {
    lv_obj_t *ov;
    lv_obj_t *r[6];
    lv_obj_t *err;
    hmi_choice_cb_t choice_cb;
    hmi_datetime_cb_t dt_cb;
    void *user;
} picker_t;

static void picker_deleted_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static void picker_cancel_cb(lv_event_t *e)
{
    picker_t *p = lv_event_get_user_data(e);
    lv_obj_delete_async(p->ov);
}

static lv_obj_t *picker_open(picker_t *p, const char *title, int32_t w)
{
    lv_obj_t *card;
    p->ov = modal_open(w, &card);
    lv_obj_add_event_cb(p->ov, picker_deleted_cb, LV_EVENT_DELETE, p);
    hmi_label(card, title, HMI_FS_HEAD, HMI_SEMIBOLD, HMI_TEXT);
    return card;
}

static void picker_buttons(picker_t *p, lv_obj_t *card, lv_event_cb_t ok_cb)
{
    p->err = hmi_label(card, "", HMI_FS_SMALL, HMI_SEMIBOLD, HMI_CRITICAL);
    lv_obj_t *row = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_style_pad_top(row, 4, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(dialog_button(row, "Cancel", false, 0, 120), picker_cancel_cb, LV_EVENT_CLICKED, p);
    lv_obj_add_event_cb(dialog_button(row, "OK", true, HMI_ACCENT, 160), ok_cb, LV_EVENT_CLICKED, p);
}

static void choice_ok_cb(lv_event_t *e)
{
    picker_t *p = lv_event_get_user_data(e);
    const int index = (int)lv_roller_get_selected(p->r[0]);
    hmi_choice_cb_t cb = p->choice_cb;
    void *user = p->user;
    lv_obj_delete_async(p->ov);
    if (cb) {
        cb(index, user);
    }
}

void hmi_choice(const char *title, const char *options, int selected, hmi_choice_cb_t cb, void *user)
{
    picker_t *p = lv_malloc_zeroed(sizeof(*p));
    LV_ASSERT_MALLOC(p);
    p->choice_cb = cb;
    p->user = user;
    lv_obj_t *card = picker_open(p, title, 460);
    p->r[0] = roller(card, options, selected, LV_PCT(100)); /* the roller keeps its own copy */
    picker_buttons(p, card, choice_ok_cb);
}

#define YEAR_FIRST 2024
#define YEAR_LAST  2099

static int days_in_month(int year, int month0)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month0 == 1 && leap ? 29 : d[month0 % 12];
}

static void datetime_ok_cb(lv_event_t *e)
{
    picker_t *p = lv_event_get_user_data(e);
    struct tm t = {0};
    t.tm_year = YEAR_FIRST + (int)lv_roller_get_selected(p->r[0]) - 1900;
    t.tm_mon = (int)lv_roller_get_selected(p->r[1]);
    t.tm_mday = 1 + (int)lv_roller_get_selected(p->r[2]);
    t.tm_hour = (int)lv_roller_get_selected(p->r[3]);
    t.tm_min = (int)lv_roller_get_selected(p->r[4]);
    t.tm_sec = (int)lv_roller_get_selected(p->r[5]);
    t.tm_isdst = 0;
    if (t.tm_mday > days_in_month(t.tm_year + 1900, t.tm_mon)) {
        lv_label_set_text(p->err, "That day does not exist in this month");
        return;
    }
    hmi_datetime_cb_t cb = p->dt_cb;
    void *user = p->user;
    lv_obj_delete_async(p->ov);
    if (cb) {
        cb(&t, user);
    }
}

/* "00\n01\n...": numbers first..last, `width` digits. Caller frees. */
static char *number_options(int first, int last, int width)
{
    const size_t n = (size_t)(last - first + 1);
    char *s = lv_malloc(n * (size_t)(width + 1) + 1);
    LV_ASSERT_MALLOC(s);
    char *w = s;
    for (int v = first; v <= last; v++) {
        w += sprintf(w, "%0*d%s", width, v, v < last ? "\n" : "");
    }
    return s;
}

void hmi_datetime(const char *title, const struct tm *initial, hmi_datetime_cb_t cb, void *user)
{
    picker_t *p = lv_malloc_zeroed(sizeof(*p));
    LV_ASSERT_MALLOC(p);
    p->dt_cb = cb;
    p->user = user;
    lv_obj_t *card = picker_open(p, title, 680);

    struct tm t = *initial;
    int year = t.tm_year + 1900;
    year = year < YEAR_FIRST ? YEAR_FIRST : year > YEAR_LAST ? YEAR_LAST : year;

    lv_obj_t *row = hmi_box(card, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    static const char *const captions[] = {"Year", "Month", "Day", "Hour", "Minute", "Second"};
    const int first[] = {YEAR_FIRST, 1, 1, 0, 0, 0};
    const int last[] = {YEAR_LAST, 12, 31, 23, 59, 59};
    const int sel[] = {year - YEAR_FIRST, t.tm_mon, t.tm_mday - 1, t.tm_hour, t.tm_min, t.tm_sec};
    for (int i = 0; i < 6; i++) {
        char *opts = number_options(first[i], last[i], i == 0 ? 4 : 2);
        p->r[i] = roller_column(row, captions[i], opts, sel[i], i == 0 ? 120 : 88); /* the roller copies them */
        lv_free(opts);
    }
    lv_obj_t *note = hmi_label(card, "Local time. The RTC keeps it while the power is off.", HMI_FS_SMALL, HMI_REGULAR,
                               HMI_TEXT_2);
    lv_obj_set_width(note, LV_PCT(100));
    picker_buttons(p, card, datetime_ok_cb);
}
