/*
 * Fonts, shared styles and widget helpers of the Fluent light theme (docs/HMI_THEME.md).
 *
 * The LVGL default theme is disabled (CONFIG_LV_USE_THEME_DEFAULT=n): every object here starts
 * from an empty style, so nothing of the Material look leaks in.
 */

#include "esp_log.h"
#include "hmi_internal.h"

static const char *TAG = "hmi_theme";

/* Open Sans 3.003 (OFL, fonts/OFL.txt), embedded by CMake EMBED_FILES. Chosen because Segoe UI is
 * proprietary and Open Sans has the same designer; its digits all have the same advance width. */
extern const uint8_t open_sans_regular_start[] asm("_binary_OpenSans_Regular_ttf_start");
extern const uint8_t open_sans_regular_end[] asm("_binary_OpenSans_Regular_ttf_end");
extern const uint8_t open_sans_semibold_start[] asm("_binary_OpenSans_SemiBold_ttf_start");
extern const uint8_t open_sans_semibold_end[] asm("_binary_OpenSans_SemiBold_ttf_end");

#define FONT_SLOTS        24
#define FONT_GLYPH_CACHE  128 /* ASCII plus a few symbols per size */

static struct {
    int px;
    hmi_weight_t weight;
    lv_font_t *font;
} s_fonts[FONT_SLOTS];

lv_style_t hmi_st_card;
lv_style_t hmi_st_control;
lv_style_t hmi_st_pressed;
lv_style_t hmi_st_pressed_dim;

const lv_font_t *hmi_font(int px, hmi_weight_t weight)
{
    int free_slot = -1;
    for (int i = 0; i < FONT_SLOTS; i++) {
        if (s_fonts[i].font && s_fonts[i].px == px && s_fonts[i].weight == weight) {
            return s_fonts[i].font;
        }
        if (!s_fonts[i].font && free_slot < 0) {
            free_slot = i;
        }
    }
    const uint8_t *start = weight == HMI_SEMIBOLD ? open_sans_semibold_start : open_sans_regular_start;
    const uint8_t *end = weight == HMI_SEMIBOLD ? open_sans_semibold_end : open_sans_regular_end;
    lv_font_t *font = free_slot < 0 ? NULL
                      : lv_tiny_ttf_create_data_ex(start, (size_t)(end - start), px, LV_FONT_KERNING_NORMAL,
                                                   FONT_GLYPH_CACHE);
    if (font == NULL) {
        ESP_LOGE(TAG, "no font Open Sans %d px %s", px, weight == HMI_SEMIBOLD ? "SemiBold" : "Regular");
        return LV_FONT_DEFAULT;
    }
    s_fonts[free_slot].px = px;
    s_fonts[free_slot].weight = weight;
    s_fonts[free_slot].font = font;
    return font;
}

void hmi_theme_init(void)
{
    lv_style_init(&hmi_st_card);
    lv_style_set_bg_color(&hmi_st_card, HMI_C(HMI_WHITE));
    lv_style_set_bg_opa(&hmi_st_card, LV_OPA_COVER);
    lv_style_set_radius(&hmi_st_card, 8);
    lv_style_set_border_width(&hmi_st_card, 1);
    lv_style_set_border_color(&hmi_st_card, lv_color_black());
    lv_style_set_border_opa(&hmi_st_card, HMI_STROKE_OPA);

    /* Control layer: one step off the white card, a stroke a touch darker than the card's; the
     * darker bottom edge is hmi_elevation(). */
    lv_style_init(&hmi_st_control);
    lv_style_set_bg_color(&hmi_st_control, HMI_C(HMI_CONTROL));
    lv_style_set_bg_opa(&hmi_st_control, LV_OPA_COVER);
    lv_style_set_radius(&hmi_st_control, 4);
    lv_style_set_border_width(&hmi_st_control, 1);
    lv_style_set_border_color(&hmi_st_control, lv_color_black());
    lv_style_set_border_opa(&hmi_st_control, 20);
    lv_style_set_text_color(&hmi_st_control, HMI_C(HMI_TEXT));

    lv_style_init(&hmi_st_pressed);
    lv_style_set_bg_color(&hmi_st_pressed, HMI_C(0xF0F0F0));

    lv_style_init(&hmi_st_pressed_dim);
    lv_style_set_color_filter_dsc(&hmi_st_pressed_dim, &lv_color_filter_shade);
    lv_style_set_color_filter_opa(&hmi_st_pressed_dim, LV_OPA_20);
}

lv_obj_t *hmi_box(lv_obj_t *parent, int flow, int32_t gap)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    if (flow >= 0) {
        lv_obj_set_flex_flow(o, (lv_flex_flow_t)flow);
        lv_obj_set_style_pad_row(o, gap, 0);
        lv_obj_set_style_pad_column(o, gap, 0);
    }
    return o;
}

void hmi_scroll_area(lv_obj_t *obj)
{
    lv_obj_set_scrollable(obj, true);
    lv_obj_set_clickable(obj, true);
    lv_obj_set_click_focusable(obj, false); /* a tap on it must not change any state */
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
}

lv_obj_t *hmi_label(lv_obj_t *parent, const char *text, int px, hmi_weight_t weight, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, hmi_font(px, weight), 0);
    lv_obj_set_style_text_color(l, HMI_C(color), 0);
    return l;
}

lv_obj_t *hmi_card(lv_obj_t *parent, int32_t pad_ver, int32_t pad_hor, int32_t gap)
{
    lv_obj_t *c = hmi_box(parent, LV_FLEX_FLOW_COLUMN, gap);
    lv_obj_add_style(c, &hmi_st_card, 0);
    lv_obj_set_style_pad_ver(c, pad_ver, 0);
    lv_obj_set_style_pad_hor(c, pad_hor, 0);
    return c;
}

/* ---- Elevation (Windows 11 ControlElevationBorder) ---- */

static void elevation_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    if (lv_obj_has_state(obj, LV_STATE_PRESSED) || lv_obj_get_style_bg_opa(obj, 0) < LV_OPA_COVER) {
        return; /* pressed controls look flat; an empty segment has no edge */
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t r = lv_obj_get_style_radius(obj, 0);
    const int32_t inset = r < lv_area_get_height(&a) / 2 ? r : lv_area_get_height(&a) / 2;
    const lv_area_t edge = {a.x1 + inset, a.y2, a.x2 - inset, a.y2}; /* the straight part of the bottom */
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_black();
    d.bg_opa = (lv_opa_t)(uintptr_t)lv_event_get_user_data(e);
    lv_draw_rect(lv_event_get_layer(e), &d, &edge);
}

void hmi_elevation(lv_obj_t *obj, lv_opa_t opa)
{
    lv_obj_remove_event_cb(obj, elevation_cb);
    if (opa > LV_OPA_MIN) {
        lv_obj_add_event_cb(obj, elevation_cb, LV_EVENT_DRAW_MAIN_END, (void *)(uintptr_t)opa);
    }
    lv_obj_invalidate(obj);
}

void hmi_field_style(lv_obj_t *box, lv_obj_t *value, hmi_field_t state)
{
    const bool ch = state == HMI_FIELD_CHANGED;
    const bool edit = state != HMI_FIELD_READONLY;
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_bg_color(box, HMI_C(ch ? HMI_CAUTION_BG : HMI_CONTROL), 0);
    lv_obj_set_style_bg_opa(box, edit ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(box, ch ? HMI_C(HMI_CAUTION) : lv_color_black(), 0);
    lv_obj_set_style_border_opa(box, ch ? LV_OPA_40 : edit ? 20 : HMI_STROKE_OPA, 0);
    lv_obj_set_style_text_color(value, HMI_C(ch ? HMI_CAUTION : edit ? HMI_TEXT : HMI_TEXT_2), 0);
    hmi_elevation(box, edit ? HMI_FIELD_OPA : 0);
}

lv_obj_t *hmi_control(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *b = hmi_box(parent, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_clickable(b, true);
    lv_obj_add_style(b, &hmi_st_control, 0);
    lv_obj_add_style(b, &hmi_st_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(b, w, h);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    hmi_elevation(b, HMI_ELEV_OPA);
    return b;
}

lv_obj_t *hmi_filled_button(lv_obj_t *parent, int32_t h, uint32_t bg, int32_t radius)
{
    lv_obj_t *b = hmi_box(parent, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_clickable(b, true);
    lv_obj_set_size(b, LV_PCT(100), h);
    lv_obj_set_style_bg_color(b, HMI_C(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_black(), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_20, 0);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(b, &hmi_st_pressed_dim, LV_STATE_PRESSED);
    hmi_elevation(b, LV_OPA_40); /* accent buttons: black at 40 % (ControlStrokeColorOnAccentSecondary) */
    return b;
}

void hmi_spacer(lv_obj_t *parent)
{
    lv_obj_t *s = hmi_box(parent, -1, 0);
    lv_obj_set_size(s, 0, 0);
    lv_obj_set_flex_grow(s, 1);
}
