/*
 * Outline icons in the style of the mockup (24-unit grid, 1.5-unit round strokes), drawn with
 * the LVGL draw API at any size and colour. No icon font or image is needed; the firmware may
 * switch to Fluent UI System Icons later (docs/HMI_THEME.md section 4).
 *
 * Coordinates below are the mockup's SVG coordinates (24 x 24). Curves of the SVG paths are
 * approximated with arcs and short polylines.
 */

#include <math.h>
#include "hmi_internal.h"

typedef struct {
    lv_layer_t *layer;
    float x0, y0, s;  /* origin and scale (px per unit) */
    int32_t w;        /* stroke width in px */
    lv_color_t c;
} pen_t;

static float px(const pen_t *p, float u, float origin)
{
    return origin + u * p->s;
}

static void line(const pen_t *p, float x1, float y1, float x2, float y2)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = p->c;
    d.width = p->w;
    d.opa = LV_OPA_COVER;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = (lv_value_precise_t)px(p, x1, p->x0);
    d.p1.y = (lv_value_precise_t)px(p, y1, p->y0);
    d.p2.x = (lv_value_precise_t)px(p, x2, p->x0);
    d.p2.y = (lv_value_precise_t)px(p, y2, p->y0);
    lv_draw_line(p->layer, &d);
}

/* Polyline through n points given as x0, y0, x1, y1, ... */
static void poly(const pen_t *p, const float *xy, int n)
{
    for (int i = 0; i + 1 < n; i++) {
        line(p, xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3]);
    }
}

/* Stroked arc of radius r (stroke centre) from a0 to a1 degrees, clockwise, 0 = 3 o'clock. */
static void arc(const pen_t *p, float cx, float cy, float r, int a0, int a1)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = p->c;
    d.width = p->w;
    d.opa = LV_OPA_COVER;
    d.rounded = 1;
    d.center.x = (int32_t)lroundf(px(p, cx, p->x0));
    d.center.y = (int32_t)lroundf(px(p, cy, p->y0));
    d.radius = (uint16_t)lroundf(r * p->s + p->w / 2.0f); /* LVGL takes the outer radius */
    d.start_angle = a0;
    d.end_angle = a1;
    lv_draw_arc(p->layer, &d);
}

/* Rectangle in units; filled, or outlined with the pen width. */
static void rect(const pen_t *p, float x, float y, float w, float h, float r, bool filled, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = (int32_t)lroundf(r * p->s);
    if (filled) {
        d.bg_color = c;
        d.bg_opa = LV_OPA_COVER;
        d.border_width = 0;
    } else {
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = c;
        d.border_width = p->w;
        d.border_opa = LV_OPA_COVER;
    }
    lv_area_t a = {
        .x1 = (int32_t)lroundf(px(p, x, p->x0)),
        .y1 = (int32_t)lroundf(px(p, y, p->y0)),
        .x2 = (int32_t)lroundf(px(p, x + w, p->x0)) - 1,
        .y2 = (int32_t)lroundf(px(p, y + h, p->y0)) - 1,
    };
    lv_draw_rect(p->layer, &d, &a);
}

static void triangle(const pen_t *p, float ax, float ay, float bx, float by, float cx, float cy)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = p->c;
    d.opa = LV_OPA_COVER;
    d.p[0].x = (lv_value_precise_t)px(p, ax, p->x0);
    d.p[0].y = (lv_value_precise_t)px(p, ay, p->y0);
    d.p[1].x = (lv_value_precise_t)px(p, bx, p->x0);
    d.p[1].y = (lv_value_precise_t)px(p, by, p->y0);
    d.p[2].x = (lv_value_precise_t)px(p, cx, p->x0);
    d.p[2].y = (lv_value_precise_t)px(p, cy, p->y0);
    lv_draw_triangle(p->layer, &d);
}

static void draw_icon(const pen_t *p, hmi_icon_t icon)
{
    switch (icon) {
    case HMI_ICON_APP:
        rect(p, 3.5f, 3.5f, 7, 7, 1.5f, false, p->c);
        rect(p, 13.5f, 3.5f, 7, 7, 1.5f, false, p->c);
        rect(p, 3.5f, 13.5f, 7, 7, 1.5f, false, p->c);
        rect(p, 13.5f, 13.5f, 7, 7, 1.5f, false, p->c);
        break;
    case HMI_ICON_HOME: {
        static const float roof[] = {3.5f, 10.5f, 12, 3.5f, 20.5f, 10.5f};
        static const float walls[] = {5.5f, 9, 5.5f, 20, 18.5f, 20, 18.5f, 9};
        static const float door[] = {10, 20, 10, 14, 14, 14, 14, 20};
        poly(p, roof, 3);
        poly(p, walls, 4);
        poly(p, door, 4);
        break;
    }
    case HMI_ICON_MOVE: {
        static const float up[] = {9, 6, 12, 3, 15, 6};
        static const float down[] = {9, 18, 12, 21, 15, 18};
        static const float left[] = {6, 9, 3, 12, 6, 15};
        static const float right[] = {18, 9, 21, 12, 18, 15};
        line(p, 12, 3, 12, 21);
        line(p, 3, 12, 21, 12);
        poly(p, up, 3);
        poly(p, down, 3);
        poly(p, left, 3);
        poly(p, right, 3);
        break;
    }
    case HMI_ICON_CHIP:
        rect(p, 6, 6, 12, 12, 2, false, p->c);
        line(p, 9.5f, 3, 9.5f, 6);
        line(p, 14.5f, 3, 14.5f, 6);
        line(p, 9.5f, 18, 9.5f, 21);
        line(p, 14.5f, 18, 14.5f, 21);
        line(p, 3, 9.5f, 6, 9.5f);
        line(p, 3, 14.5f, 6, 14.5f);
        line(p, 18, 9.5f, 21, 9.5f);
        line(p, 18, 14.5f, 21, 14.5f);
        break;
    case HMI_ICON_BELL: {
        static const float left[] = {6, 9, 5.8f, 13, 5, 15.6f, 3.5f, 17};
        static const float right[] = {18, 9, 18.2f, 13, 19, 15.6f, 20.5f, 17};
        arc(p, 12, 9, 6, 180, 360);
        poly(p, left, 4);
        poly(p, right, 4);
        line(p, 3.5f, 17, 20.5f, 17);
        arc(p, 12, 20.5f, 2, 0, 180);
        break;
    }
    case HMI_ICON_GEAR:
        arc(p, 12, 12, 3, 0, 360);
        arc(p, 12, 12, 7, 0, 360);
        for (int k = 0; k < 8; k++) {
            float a = (float)k * (float)M_PI / 4.0f;
            line(p, 12 + 7 * cosf(a), 12 + 7 * sinf(a), 12 + 9.5f * cosf(a), 12 + 9.5f * sinf(a));
        }
        break;
    case HMI_ICON_SYNC: {
        static const float head1[] = {18, 3, 18, 7, 14, 7};
        static const float head2[] = {6, 21, 6, 17, 10, 17};
        arc(p, 12, 12, 8, 180, 318);
        arc(p, 12, 12, 8, 0, 138);
        poly(p, head1, 3);
        poly(p, head2, 3);
        break;
    }
    case HMI_ICON_CHEVRON_DOWN: {
        static const float v[] = {6, 9, 12, 15, 18, 9};
        poly(p, v, 3);
        break;
    }
    case HMI_ICON_CHEVRON_RIGHT: {
        static const float v[] = {9, 6, 15, 12, 9, 18};
        poly(p, v, 3);
        break;
    }
    case HMI_ICON_PLAY:
        triangle(p, 8, 4.8f, 8, 19.2f, 20, 12);
        break;
    case HMI_ICON_STOP:
        rect(p, 5, 5, 14, 14, 2, true, p->c);
        break;
    case HMI_ICON_RESET: {
        static const float head[] = {4, 4, 4, 8, 8, 8};
        arc(p, 12, 12, 8, 225, 180);
        poly(p, head, 3);
        break;
    }
    case HMI_ICON_ERROR: {
        pen_t white = *p;
        white.c = lv_color_white();
        white.w = p->w < 2 ? 2 : p->w;
        rect(p, 2, 2, 20, 20, 10, true, HMI_C(HMI_CRITICAL));
        line(&white, 9, 9, 15, 15);
        line(&white, 15, 9, 9, 15);
        break;
    }
    case HMI_ICON_INFO:
        arc(p, 12, 12, 9, 0, 360);
        line(p, 12, 11, 12, 16.5f);
        line(p, 12, 7.8f, 12, 7.9f);
        break;
    case HMI_ICON_CLOCK: {
        static const float hands[] = {12, 7, 12, 12, 15, 14};
        arc(p, 12, 12, 9, 0, 360);
        poly(p, hands, 3);
        break;
    }
    case HMI_ICON_GAUGE:
        arc(p, 12, 18, 8, 180, 360);
        line(p, 12, 18, 16, 12);
        break;
    case HMI_ICON_DOCUMENT:
        rect(p, 5, 3, 14, 18, 2, false, p->c);
        line(p, 9, 8, 15, 8);
        line(p, 9, 12, 15, 12);
        line(p, 9, 16, 13, 16);
        break;
    case HMI_ICON_USER:
        arc(p, 12, 8, 4, 0, 360);
        arc(p, 12, 23.33f, 8.33f, 196, 344); /* circle through the SVG's (4,21), (12,15), (20,21) */
        break;
    case HMI_ICON_WRENCH:
        arc(p, 15.5f, 8.5f, 4.5f, 345, 285); /* head, open towards the top right */
        line(p, 12.3f, 11.7f, 5, 19);
        break;
    case HMI_ICON_NETWORK:
        rect(p, 3, 9, 6, 6, 1, false, p->c);
        rect(p, 15, 3, 6, 6, 1, false, p->c);
        rect(p, 15, 15, 6, 6, 1, false, p->c);
        line(p, 9, 12, 12, 12);
        line(p, 12, 6, 12, 18);
        line(p, 12, 6, 15, 6);
        line(p, 12, 18, 15, 18);
        break;
    case HMI_ICON_CHECK: {
        static const float tick[] = {4.5f, 12.5f, 9.5f, 17.5f, 19.5f, 6.5f};
        poly(p, tick, 3);
        break;
    }
    case HMI_ICON_WIFI: /* three arcs over a dot, 90 degrees wide */
        arc(p, 12, 19, 3.5f, 225, 315);
        arc(p, 12, 19, 7.5f, 225, 315);
        arc(p, 12, 19, 11.5f, 225, 315);
        rect(p, 11, 18, 2, 2, 1, true, p->c);
        break;
    case HMI_ICON_MONITOR:
        rect(p, 3, 4, 18, 12, 1.5f, false, p->c);
        line(p, 8, 20, 16, 20);
        line(p, 12, 16, 12, 20);
        break;
    case HMI_ICON_DOWNLOAD: {
        static const float head[] = {7, 10, 12, 15, 17, 10};
        line(p, 12, 3, 12, 15);
        poly(p, head, 3);
        line(p, 4, 19, 20, 19);
        break;
    }
    case HMI_ICON_UPLOAD: {
        static const float head[] = {7, 14, 12, 9, 17, 14};
        line(p, 12, 20, 12, 9);
        poly(p, head, 3);
        line(p, 5, 4, 19, 4);
        break;
    }
    case HMI_ICON_SAVE: {
        static const float body[] = {5, 4, 16, 4, 19, 7, 19, 20, 5, 20, 5, 4};
        static const float shutter[] = {8, 4, 8, 9, 15, 9, 15, 4};
        static const float label[] = {8, 20, 8, 14, 16, 14, 16, 20};
        poly(p, body, 6);
        poly(p, shutter, 4);
        poly(p, label, 4);
        break;
    }
    case HMI_ICON_PLUS:
        line(p, 12, 5, 12, 19);
        line(p, 5, 12, 19, 12);
        break;
    case HMI_ICON_WARNING: {
        pen_t fill = *p;
        fill.c = HMI_C(HMI_CAUTION);
        pen_t white = *p;
        white.c = lv_color_white();
        white.w = p->w < 2 ? 2 : p->w;
        triangle(&fill, 12, 2.5f, 1.5f, 21, 22.5f, 21);
        line(&white, 12, 9, 12, 13);
        line(&white, 12, 17, 12, 17.05f);
        break;
    }
    }
}

static void icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    float size = (float)lv_area_get_width(&a);
    pen_t p = {
        .layer = lv_event_get_layer(e),
        .x0 = (float)a.x1,
        .y0 = (float)a.y1,
        .s = size / 24.0f,
        .w = (int32_t)lroundf(1.75f * size / 24.0f),
        .c = lv_obj_get_style_text_color(obj, LV_PART_MAIN),
    };
    if (p.w < 1) {
        p.w = 1;
    }
    draw_icon(&p, (hmi_icon_t)(uintptr_t)lv_event_get_user_data(e));
}

lv_obj_t *hmi_icon(lv_obj_t *parent, hmi_icon_t icon, int32_t size, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_text_color(o, HMI_C(color), 0);
    lv_obj_add_event_cb(o, icon_draw_cb, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)icon);
    return o;
}

void hmi_icon_set_color(lv_obj_t *icon, uint32_t color)
{
    lv_obj_set_style_text_color(icon, HMI_C(color), 0); /* also invalidates the icon */
}
