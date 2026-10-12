/*
 * Settings > System info (no mockup; built like Settings > Time): memory on the left, the device
 * on the right, refreshed every second. The console command 'mem' prints the same memory figures
 * plus the stack reserve of the tasks.
 *
 * Memory: free now, lowest since start-up (heap_caps_get_minimum_free_size) and the largest free
 * block. Internal RAM is the one to watch: the HMI draw buffer and the EtherCAT data live there,
 * and a new block must fit in one piece.
 */

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "hmi_internal.h"
#include "machine_store.h"
#include "net.h"

#define POLL_MS  1000
#define RIGHT_W  380
#define KEY_W    112
#define BAR_H    10

/* Every internal RAM block of the P4 is DMA-capable, so a DMA row would repeat internal RAM. */
typedef enum {
    MEM_INTERNAL,
    MEM_PSRAM,
    MEM_STORE, /* the machine store in flash (machine_store.h) */
    MEM_COUNT,
} mem_t;

static const struct {
    const char *name;
    uint32_t caps;
} s_mem[MEM_COUNT] = {
    [MEM_INTERNAL] = {"Internal RAM", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT},
    [MEM_PSRAM] = {"PSRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT},
    [MEM_STORE] = {"Settings store (flash)", 0},
};

typedef enum {
    DEV_FIRMWARE,
    DEV_IDF,
    DEV_CHIP,
    DEV_FLASH,
    DEV_MAC,
    DEV_UPTIME,
    DEV_WIFI,
    DEV_ETHERCAT,
    DEV_COUNT,
} dev_t_;

static const char *const s_dev_names[DEV_COUNT] = {"Firmware", "ESP-IDF", "Chip", "Flash / PSRAM", "MAC",
                                                    "Uptime",   "Wi-Fi",   "EtherCAT"};

static EXT_RAM_BSS_ATTR struct {
    lv_obj_t *free[MEM_COUNT], *bar[MEM_COUNT], *detail[MEM_COUNT];
    lv_obj_t *dev[DEV_COUNT];
    lv_timer_t *poll;
} s_in;

/* "152 KB", "29.4 MB" */
static void fmt_size(size_t bytes, char *out, size_t len)
{
    if (bytes >= 10u * 1024 * 1024) {
        snprintf(out, len, "%.1f MB", bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, len, "%u KB", (unsigned)((bytes + 512) / 1024));
    }
}

static void store_show(void)
{
    machine_store_info_t st;
    machine_store_info(&st);
    char u[16], t[16], line[96];
    fmt_size(st.used, u, sizeof(u));
    fmt_size(st.size, t, sizeof(t));
    snprintf(line, sizeof(line), "%s used of %s", u, t);
    hmi_set_text(s_in.free[MEM_STORE], st.ok ? line : "Not available");
    snprintf(line, sizeof(line), "%u settings \xC2\xB7 tidied up %u time%s%s", st.keys, (unsigned)st.gen, st.gen == 1 ? "" : "s",
             st.waiting ? " \xC2\xB7 some wait for EtherCAT to leave OP" : "");
    hmi_set_text(s_in.detail[MEM_STORE], line);
    const int used = st.size ? (int)((uint64_t)st.used * 100 / st.size) : 0;
    lv_bar_set_value(s_in.bar[MEM_STORE], used, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_in.bar[MEM_STORE], HMI_C(HMI_ACCENT), LV_PART_INDICATOR);
}

static void mem_show(mem_t m)
{
    if (m == MEM_STORE) {
        store_show();
        return;
    }
    const uint32_t caps = s_mem[m].caps;
    const size_t total = heap_caps_get_total_size(caps);
    const size_t free_b = heap_caps_get_free_size(caps);
    char f[16], t[16], lo[16], big[16], line[96];
    fmt_size(free_b, f, sizeof(f));
    fmt_size(total, t, sizeof(t));
    fmt_size(heap_caps_get_minimum_free_size(caps), lo, sizeof(lo));
    fmt_size(heap_caps_get_largest_free_block(caps), big, sizeof(big));
    snprintf(line, sizeof(line), "%s free of %s", f, t);
    hmi_set_text(s_in.free[m], line);
    snprintf(line, sizeof(line), "Lowest %s \xC2\xB7 largest block %s", lo, big);
    hmi_set_text(s_in.detail[m], line);
    const int used = total ? (int)((total - free_b) * 100 / total) : 0;
    lv_bar_set_value(s_in.bar[m], used, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_in.bar[m], HMI_C(used >= 90 ? HMI_CRITICAL : used >= 75 ? HMI_CAUTION : HMI_ACCENT),
                              LV_PART_INDICATOR);
}

static void dev_dynamic(void)
{
    char t[64];
    const int64_t s = esp_timer_get_time() / 1000000;
    if (s >= 86400) {
        snprintf(t, sizeof(t), "%d d %02d:%02d:%02d", (int)(s / 86400), (int)(s / 3600 % 24), (int)(s / 60 % 60),
                 (int)(s % 60));
    } else {
        snprintf(t, sizeof(t), "%02d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    }
    hmi_set_text(s_in.dev[DEV_UPTIME], t);

    net_status_t n;
    net_status(&n);
    if (n.state == NET_WIFI_CONNECTED) {
        snprintf(t, sizeof(t), "%u.%u.%u.%u \xC2\xB7 %s", n.ip[0], n.ip[1], n.ip[2], n.ip[3], n.ssid);
    } else {
        snprintf(t, sizeof(t), "%s", net_state_text(n.state));
    }
    hmi_set_text(s_in.dev[DEV_WIFI], t);

    hmi_bus_status_t b = {.state = HMI_BUS_STARTING};
    (void)hmi_bus_status_read(&b);
    if (b.state == HMI_BUS_OP || b.state == HMI_BUS_OP_FAULT) {
        snprintf(t, sizeof(t), "OP \xC2\xB7 %u slaves \xC2\xB7 %u \xC2\xB5s", b.slaves, (unsigned)b.cycle_us);
    } else {
        snprintf(t, sizeof(t), "%s", b.state == HMI_BUS_HELD      ? "Held in PRE-OP"
                                     : b.state == HMI_BUS_NO_LINK ? "No link"
                                                                  : "Starting");
    }
    hmi_set_text(s_in.dev[DEV_ETHERCAT], t);
}

static void dev_static(void)
{
    char t[64];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(t, sizeof(t), "%s \xC2\xB7 %s %.5s", app->version, app->date, app->time);
    hmi_set_text(s_in.dev[DEV_FIRMWARE], t);
    hmi_set_text(s_in.dev[DEV_IDF], esp_get_idf_version());

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(t, sizeof(t), "ESP32-P4 v%d.%d \xC2\xB7 %d MHz", chip.revision / 100, chip.revision % 100,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    hmi_set_text(s_in.dev[DEV_CHIP], t);

    uint32_t flash = 0;
    (void)esp_flash_get_size(NULL, &flash);
    snprintf(t, sizeof(t), "%u MB / %u MB", (unsigned)(flash >> 20), (unsigned)(esp_psram_get_size() >> 20));
    hmi_set_text(s_in.dev[DEV_FLASH], t);

    uint8_t mac[6] = {0};
    (void)esp_efuse_mac_get_default(mac);
    snprintf(t, sizeof(t), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    hmi_set_text(s_in.dev[DEV_MAC], t);
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    for (int m = 0; m < MEM_COUNT; m++) {
        mem_show((mem_t)m);
    }
    dev_dynamic();
}

static void mem_card(lv_obj_t *body)
{
    lv_obj_t *card = hmi_card(body, 12, 18, 6);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    hmi_card_title(card, "Memory");
    for (int m = 0; m < MEM_COUNT; m++) {
        lv_obj_t *blk = hmi_box(card, LV_FLEX_FLOW_COLUMN, 6);
        lv_obj_set_width(blk, LV_PCT(100));
        lv_obj_set_style_pad_top(blk, 14, 0);
        lv_obj_t *head = hmi_box(blk, LV_FLEX_FLOW_ROW, 8);
        lv_obj_set_width(head, LV_PCT(100));
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        hmi_label(head, s_mem[m].name, HMI_FS_BODY, HMI_SEMIBOLD, HMI_TEXT);
        s_in.free[m] = hmi_label(head, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);

        lv_obj_t *bar = lv_bar_create(blk);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, LV_PCT(100), BAR_H);
        lv_bar_set_range(bar, 0, 100);
        lv_obj_set_style_radius(bar, BAR_H / 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar, HMI_STROKE_OPA, 0);
        lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        s_in.bar[m] = bar;
        s_in.detail[m] = hmi_label(blk, "", HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    }
    hmi_spacer(card);
    lv_obj_t *note = hmi_label(card,
                               "Lowest: the least that was ever free since start-up. A new block must fit in the "
                               "largest free block. Console: 'mem' adds the stack reserve of each task.",
                               HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2);
    lv_obj_set_width(note, LV_PCT(100));
}

static void dev_card(lv_obj_t *body)
{
    lv_obj_t *card = hmi_card(body, 12, 18, 0);
    lv_obj_set_size(card, RIGHT_W, LV_PCT(100));
    lv_obj_t *title = hmi_card_title(card, "Device");
    lv_obj_set_style_pad_bottom(title, 6, 0);
    for (int i = 0; i < DEV_COUNT; i++) {
        lv_obj_t *r = hmi_box(card, LV_FLEX_FLOW_ROW, 10);
        lv_obj_set_size(r, LV_PCT(100), 46);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        hmi_divider_top(r);
        lv_obj_set_width(hmi_label(r, s_dev_names[i], HMI_FS_SMALL, HMI_REGULAR, HMI_TEXT_2), KEY_W);
        lv_obj_t *v = hmi_label(r, "", HMI_FS_BODY, HMI_REGULAR, HMI_TEXT);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(v, lv_font_get_line_height(hmi_font(HMI_FS_BODY, HMI_REGULAR)));
        lv_obj_set_flex_grow(v, 1);
        s_in.dev[i] = v;
    }
}

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    if (s_in.poll) {
        lv_timer_delete(s_in.poll);
    }
    memset(&s_in, 0, sizeof(s_in));
}

void hmi_info_create(lv_obj_t *root, lv_obj_t *head)
{
    (void)head;
    memset(&s_in, 0, sizeof(s_in));
    lv_obj_t *body = hmi_box(root, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_add_event_cb(body, deleted_cb, LV_EVENT_DELETE, NULL);
    mem_card(body);
    dev_card(body);
    dev_static();
    poll_cb(NULL);
    s_in.poll = lv_timer_create(poll_cb, POLL_MS, NULL);
}
