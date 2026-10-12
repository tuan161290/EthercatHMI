/*
 * Machine data store (machine_store.h): values in RAM, an append-only log in flash.
 *
 * Why a log and not NVS: a flash erase stalls both cores (the cache is off) for tens to hundreds
 * of milliseconds, longer than the drives' 100 ms SM watchdog, and NVS (24 KB, 6 pages) erases a
 * sector whenever a page fills. Programming already erased flash takes one page program per 256
 * bytes (about 0.5 ms, unverified for this Boya chip). So:
 *   - a save is appended at once, one flash page at a time from an internal-RAM buffer (one
 *     esp_partition_write() per page, so each stall is one page program); while EtherCAT is in
 *     OP a pause follows every page so the 1 ms cycle runs between the stalls;
 *   - flash is erased only by a compaction, and only while flash_ok(): at start-up when the
 *     active half is more than half full, or when a save does not fit (it then waits in RAM until
 *     the bus leaves OP).
 *
 * Layout of the "mstore" partition: two halves. A half starts with {magic, generation, CRC}; the
 * valid half with the higher generation is active. Records follow it:
 * {magic, seq, len, key, data CRC, header CRC} + data, padded to 16 bytes. The record with the
 * highest seq of a key wins; len 0 removes the key. A compaction erases the other half, writes
 * the newest record of every key and then the half's header with the next generation, so a power
 * cut at any moment leaves one valid half. A record cut short by a power cut fails its CRC and is
 * skipped.
 *
 * Keys of earlier firmware (NVS namespace "machine") are imported once at start-up.
 */

#include <stddef.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "machine_store.h"
#include "nvs.h"

static const char *TAG = "machine";

#define PART_LABEL  "mstore"
#define NVS_NS      "machine"   /* earlier firmware */
#define MAX_KEYS    32
#define KEY_LEN     16
#define MAX_LEN     4096
#define RETRY_US    1000000
#define HALF_MAGIC  0x3148534Du /* "MSH1" */
#define REC_MAGIC   0x3152534Du /* "MSR1" */
#define REC_ALIGN   16
#define FLASH_PAGE  256         /* one page program per write */
#define PACE_MS     2           /* pause after each page program while in OP */

typedef struct {
    uint32_t magic;
    uint32_t gen;
    uint32_t crc; /* over magic and gen */
    uint32_t reserved;
} half_hdr_t;

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint16_t len; /* 0: the key was removed */
    uint16_t reserved;
    char key[KEY_LEN];
    uint32_t dcrc;
    uint32_t hcrc; /* over the fields above */
} rec_hdr_t;

typedef struct {
    char key[KEY_LEN]; /* "" = free slot */
    uint32_t seq;
    uint16_t len;
    bool dirty;        /* not in flash yet */
    bool removed;      /* a removal not in flash yet (data NULL) */
    void *data;
} entry_t;

static machine_flash_ok_t s_flash_ok;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_retry;
static const esp_partition_t *s_part;
static uint32_t s_half;  /* bytes per half */
static int s_active;
static uint32_t s_gen;
static uint32_t s_wr;    /* next record offset in the active half; s_half when full */
static uint32_t s_seq = 1;
static bool s_full_warned; /* "log full" said once until the next compaction */
static EXT_RAM_BSS_ATTR entry_t s_e[MAX_KEYS];
static uint8_t s_page[FLASH_PAGE]; /* internal RAM: written directly, no bounce copies */

/* ---- Helpers ---- */

static bool flash_ok(void)
{
    return s_flash_ok == NULL || s_flash_ok();
}

static uint32_t crc32(const void *p, size_t n)
{
    return esp_rom_crc32_le(0, (const uint8_t *)p, (uint32_t)n);
}

static uint32_t rec_size(uint16_t len)
{
    return (uint32_t)((sizeof(rec_hdr_t) + len + REC_ALIGN - 1) / REC_ALIGN * REC_ALIGN);
}

static uint32_t base(int half)
{
    return (uint32_t)half * s_half;
}

static bool all_ff(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static entry_t *find(const char *key)
{
    for (int i = 0; i < MAX_KEYS; i++) {
        if (s_e[i].key[0] && strncmp(s_e[i].key, key, KEY_LEN) == 0) {
            return &s_e[i];
        }
    }
    return NULL;
}

static entry_t *find_or_add(const char *key)
{
    entry_t *e = find(key);
    for (int i = 0; e == NULL && i < MAX_KEYS; i++) {
        if (s_e[i].key[0] == '\0') {
            e = &s_e[i];
            memset(e, 0, sizeof(*e));
            strlcpy(e->key, key, sizeof(e->key));
        }
    }
    return e;
}

static void forget(entry_t *e)
{
    heap_caps_free(e->data);
    memset(e, 0, sizeof(*e));
}

static bool set_data(entry_t *e, const void *buf, size_t len)
{
    if (e->data == NULL || e->len != len) {
        void *d = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (d == NULL) {
            d = heap_caps_malloc(len, MALLOC_CAP_8BIT);
        }
        if (d == NULL) {
            return false;
        }
        heap_caps_free(e->data);
        e->data = d;
        e->len = (uint16_t)len;
    }
    memcpy(e->data, buf, len);
    e->removed = false;
    return true;
}

/* ---- Flash ---- */

/* Program erased flash at partition offset `off`, one flash page per write. */
static esp_err_t program(uint32_t off, const void *src, uint32_t len)
{
    const uint8_t *p = src;
    const bool pace = !flash_ok();
    while (len) {
        uint32_t n = FLASH_PAGE - (s_part->address + off) % FLASH_PAGE; /* up to the end of the page */
        n = n < len ? n : len;
        memcpy(s_page, p, n);
        esp_err_t err = esp_partition_write(s_part, off, s_page, n);
        if (err != ESP_OK) {
            return err;
        }
        off += n;
        p += n;
        len -= n;
        if (len && pace) {
            vTaskDelay(pdMS_TO_TICKS(PACE_MS) ? pdMS_TO_TICKS(PACE_MS) : 1);
        }
    }
    return ESP_OK;
}

/* Append a record to the active half. @return ESP_ERR_NO_MEM if it does not fit */
static esp_err_t append(const char *key, uint32_t seq, const void *data, uint16_t len)
{
    const uint32_t size = rec_size(len);
    if (s_wr > s_half || size > s_half - s_wr) {
        return ESP_ERR_NO_MEM;
    }
    rec_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = REC_MAGIC;
    h.seq = seq;
    h.len = len;
    strlcpy(h.key, key, sizeof(h.key));
    h.dcrc = len ? crc32(data, len) : 0;
    h.hcrc = crc32(&h, offsetof(rec_hdr_t, hcrc));
    const uint32_t off = base(s_active) + s_wr;
    s_wr += size; /* used even if the write fails half-way */
    esp_err_t err = program(off, &h, sizeof(h));
    if (err == ESP_OK && len) {
        err = program(off + sizeof(h), data, len);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "store: writing '%s' failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

static bool half_valid(int half, uint32_t *gen)
{
    half_hdr_t hh;
    if (esp_partition_read(s_part, base(half), &hh, sizeof(hh)) != ESP_OK || hh.magic != HALF_MAGIC ||
        hh.crc != crc32(&hh, offsetof(half_hdr_t, crc))) {
        return false;
    }
    *gen = hh.gen;
    return true;
}

static esp_err_t write_half_header(int half, uint32_t gen)
{
    half_hdr_t hh = {.magic = HALF_MAGIC, .gen = gen};
    hh.crc = crc32(&hh, offsetof(half_hdr_t, crc));
    hh.reserved = 0;
    return program(base(half), &hh, sizeof(hh));
}

/*
 * Copy the newest record of every key into the other half and make it the active one. Erases
 * flash: only while flash_ok(). On failure the old half stays active.
 */
static esp_err_t compact(void)
{
    const int64_t t0 = esp_timer_get_time();
    const int from = s_active, to = 1 - s_active;
    const uint32_t wr_from = s_wr;
    esp_err_t err = esp_partition_erase_range(s_part, base(to), s_half);
    if (err == ESP_OK) {
        s_active = to;
        s_wr = sizeof(half_hdr_t);
        for (int i = 0; i < MAX_KEYS && err == ESP_OK; i++) {
            if (s_e[i].key[0] && s_e[i].data) {
                err = append(s_e[i].key, s_e[i].seq, s_e[i].data, s_e[i].len);
            }
        }
        if (err == ESP_OK) {
            err = write_half_header(to, s_gen + 1);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "store: compaction failed (%s), half %d stays active", esp_err_to_name(err), from);
        s_active = from;
        s_wr = wr_from;
        return err;
    }
    s_gen++;
    s_full_warned = false;
    for (int i = 0; i < MAX_KEYS; i++) {
        if (s_e[i].removed) {
            forget(&s_e[i]); /* not copied: gone for good */
        }
        s_e[i].dirty = false;
    }
    ESP_LOGI(TAG, "store: compacted into half %d (generation %lu): %lu of %lu bytes used, %d ms", to,
             (unsigned long)s_gen, (unsigned long)s_wr, (unsigned long)s_half,
             (int)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

/* Write what is not in flash yet: appended, or by a compaction if the half is full and flash may
 * be erased. A key whose write failed stays dirty and is tried again by the timer. */
static void flush(void)
{
    if (s_part == NULL) {
        return;
    }
    bool full = false;
    for (int i = 0; i < MAX_KEYS && !full; i++) {
        entry_t *e = &s_e[i];
        if (!e->key[0] || !e->dirty) {
            continue;
        }
        esp_err_t err = e->removed ? append(e->key, e->seq, NULL, 0) : append(e->key, e->seq, e->data, e->len);
        if (err == ESP_OK) {
            if (e->removed) {
                forget(e);
            } else {
                e->dirty = false;
            }
        } else if (err == ESP_ERR_NO_MEM) {
            full = true;
        }
    }
    if (full) {
        if (flash_ok()) {
            (void)compact();
        } else if (!s_full_warned) {
            s_full_warned = true;
            ESP_LOGW(TAG, "store: log full, saves wait in RAM until EtherCAT leaves OP");
        }
    }
}

/* Read the active half into the entries. @return false if it holds something unreadable. */
static bool scan(void)
{
    uint8_t *buf = heap_caps_malloc(MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        s_wr = s_half;
        return false;
    }
    bool clean = true;
    uint32_t off = sizeof(half_hdr_t);
    s_wr = s_half;
    while (off + sizeof(rec_hdr_t) <= s_half) {
        rec_hdr_t h;
        if (esp_partition_read(s_part, base(s_active) + off, &h, sizeof(h)) != ESP_OK) {
            clean = false;
            break;
        }
        if (all_ff(&h, sizeof(h))) {
            s_wr = off; /* free from here */
            break;
        }
        if (h.magic != REC_MAGIC || h.hcrc != crc32(&h, offsetof(rec_hdr_t, hcrc)) || h.len > MAX_LEN ||
            rec_size(h.len) > s_half - off) {
            ESP_LOGW(TAG, "store: unreadable record at 0x%05lX of half %d", (unsigned long)off, s_active);
            clean = false; /* nothing is appended behind it until a compaction */
            break;
        }
        h.key[KEY_LEN - 1] = '\0';
        const uint32_t size = rec_size(h.len);
        if (h.len && (esp_partition_read(s_part, base(s_active) + off + sizeof(h), buf, h.len) != ESP_OK ||
                      crc32(buf, h.len) != h.dcrc)) {
            ESP_LOGW(TAG, "store: record of '%s' cut short (power cut while saving?): skipped", h.key);
            off += size;
            continue;
        }
        entry_t *e = find(h.key);
        if (e == NULL || h.seq > e->seq) {
            if (h.len == 0) {
                if (e) {
                    forget(e);
                }
            } else {
                e = find_or_add(h.key);
                if (e == NULL || !set_data(e, buf, h.len)) {
                    ESP_LOGE(TAG, "store: no room for '%s'", h.key);
                } else {
                    e->seq = h.seq;
                }
            }
        }
        if (h.seq >= s_seq) {
            s_seq = h.seq + 1;
        }
        off += size;
    }
    heap_caps_free(buf);
    return clean;
}

/* Keys of earlier firmware in NVS: taken over once (written to the log by the caller). */
static int import_nvs(void)
{
    nvs_iterator_t it = NULL;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    int n = 0;
    uint8_t *buf = heap_caps_malloc(MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NS, NVS_TYPE_BLOB, &it);
    while (buf && err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        size_t len = MAX_LEN;
        if (find(info.key) == NULL && nvs_get_blob(h, info.key, buf, &len) == ESP_OK && len > 0) {
            entry_t *e = find_or_add(info.key);
            if (e && set_data(e, buf, len)) {
                e->seq = s_seq++;
                e->dirty = true;
                n++;
            }
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    heap_caps_free(buf);
    nvs_close(h);
    return n;
}

/* esp_timer task (core 0): keys that could not be written are tried again. */
static void retry_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool any = false;
    for (int i = 0; i < MAX_KEYS && !any; i++) {
        any = s_e[i].key[0] && s_e[i].dirty;
    }
    if (any) {
        flush();
    }
    xSemaphoreGive(s_lock);
}

/* ---- Public ---- */

esp_err_t machine_store_init(machine_flash_ok_t flash_ok_cb)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_flash_ok = flash_ok_cb;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    if (s_part && (s_part->size < 2 * 4096 || (s_part->size / 2) % 4096 != 0)) {
        ESP_LOGE(TAG, "store: partition '%s' has a bad size (%lu)", PART_LABEL, (unsigned long)s_part->size);
        s_part = NULL;
    }
    if (s_part == NULL) {
        ESP_LOGE(TAG, "store: no '%s' partition (flash the partition table): machine data cannot be saved",
                 PART_LABEL);
    } else {
        s_half = s_part->size / 2;
        uint32_t g0 = 0, g1 = 0;
        const bool v0 = half_valid(0, &g0), v1 = half_valid(1, &g1);
        if (!v0 && !v1) {
            ESP_LOGW(TAG, "store: empty or unknown '%s' partition: formatting it", PART_LABEL);
            esp_err_t err = esp_partition_erase_range(s_part, 0, s_half);
            if (err == ESP_OK) {
                err = write_half_header(0, 1);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "store: formatting failed: %s", esp_err_to_name(err));
                s_part = NULL;
            }
            s_active = 0;
            s_gen = 1;
        } else {
            s_active = (v1 && (!v0 || g1 > g0)) ? 1 : 0;
            s_gen = s_active ? g1 : g0;
        }
    }

    bool clean = true;
    if (s_part) {
        clean = scan();
    }
    const int imported = import_nvs();
    if (imported) {
        ESP_LOGI(TAG, "store: %d key(s) taken over from NVS", imported);
    }
    if (s_part) {
        if (!clean || s_wr > s_half / 2) {
            for (int i = 0; i < MAX_KEYS; i++) {
                s_e[i].dirty |= s_e[i].key[0] != '\0'; /* all go into the compacted half */
            }
            (void)compact(); /* start-up: flash may be erased */
        } else {
            flush();
        }
        bool waiting = false;
        for (int i = 0; i < MAX_KEYS; i++) {
            waiting |= s_e[i].key[0] && s_e[i].dirty;
        }
        if (imported && !waiting) {
            /* In the log now: drop the NVS copies, or a key removed later would come back from
             * them at the next start-up. Start-up, so flash may be erased. */
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                (void)nvs_erase_all(h);
                (void)nvs_commit(h);
                nvs_close(h);
            }
        }
        int keys = 0;
        for (int i = 0; i < MAX_KEYS; i++) {
            keys += s_e[i].key[0] != '\0';
        }
        ESP_LOGI(TAG, "store: %d key(s), half %d, %lu of %lu bytes used", keys, s_active, (unsigned long)s_wr,
                 (unsigned long)s_half);
    }

    const esp_timer_create_args_t args = {.callback = retry_cb, .name = "machine_store"};
    esp_err_t err = esp_timer_create(&args, &s_retry);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_retry, RETRY_US);
    }
    return err;
}

esp_err_t machine_store_load(const char *key, void *buf, size_t len)
{
    if (s_lock == NULL || strlen(key) >= KEY_LEN) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const entry_t *e = find(key);
    esp_err_t err;
    if (e == NULL || e->data == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else if (e->len != len) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        memcpy(buf, e->data, len);
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t machine_store_save(const char *key, const void *buf, size_t len)
{
    if (s_lock == NULL || strlen(key) >= KEY_LEN || key[0] == '\0' || len == 0 || len > MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find_or_add(key);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (e && set_data(e, buf, len)) {
        e->seq = s_seq++;
        e->dirty = true;
        if (s_part == NULL) {
            err = ESP_ERR_NOT_SUPPORTED; /* kept in RAM only */
        } else {
            flush();
            err = ESP_OK; /* written, or waiting for a compaction */
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t machine_store_remove(const char *key)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(key);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (e && !e->removed) {
        heap_caps_free(e->data);
        e->data = NULL;
        e->len = 0;
        e->removed = true;
        e->seq = s_seq++;
        e->dirty = true;
        flush();
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t machine_store_compact(void)
{
    if (s_lock == NULL || s_part == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!flash_ok()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = compact();
    xSemaphoreGive(s_lock);
    return err;
}

bool machine_store_pending(void)
{
    if (s_lock == NULL) {
        return false;
    }
    bool any = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_KEYS && !any; i++) {
        any = s_e[i].key[0] && s_e[i].dirty;
    }
    xSemaphoreGive(s_lock);
    return any;
}

bool machine_store_key_pending(const char *key)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const entry_t *e = find(key);
    const bool any = e && e->dirty;
    xSemaphoreGive(s_lock);
    return any;
}

void machine_store_info(machine_store_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->ok = s_part != NULL;
    out->half = (uint8_t)s_active;
    out->gen = s_gen;
    out->used = s_wr;
    out->size = s_half;
    for (int i = 0; i < MAX_KEYS; i++) {
        if (s_e[i].key[0] && !s_e[i].removed) {
            out->keys++;
        }
        if (s_e[i].key[0] && s_e[i].dirty) {
            out->waiting++;
        }
    }
    xSemaphoreGive(s_lock);
}

bool machine_store_key_at(int i, char *key, size_t key_len, size_t *len)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (int k = 0, n = 0; k < MAX_KEYS && !found; k++) {
        if (s_e[k].key[0] && !s_e[k].removed && n++ == i) {
            strlcpy(key, s_e[k].key, key_len);
            *len = s_e[k].len;
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}
