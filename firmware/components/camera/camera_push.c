/*
 * Push (camera.h): the HMI sends its JPEG frames to go2rtc on a server that it reaches through the
 * Internet (DDNS), so nothing needs to reach into the plant network. docs/CAMERA.md section 5.
 *
 * One endless HTTP(S) POST of H.264 in MPEG-TS to the address set, e.g.
 * https://home.example.net:8556/api/stream.ts?dst=ehmi
 * - go2rtc 1.9.14 takes a POST to /api/stream.ts as the source of stream `dst` and keeps reading it
 *   while nobody watches (internal/mpegts/mpegts.go inputMpegTS, pkg/mpegts/producer.go Start), so a
 *   viewer or the recorder can join at any time.
 * - Not MJPEG: a POST to /api/stream.mjpeg must be multipart, and its reader stops at once when
 *   nobody watches at that moment ("mjpeg: no receivers", pkg/mpjpeg/producer.go). The first version
 *   of this file (2026-10-13) sent bare JPEGs there and never got through.
 * - The camera task makes YUV420 frames; this task encodes the newest one with the P4's hardware
 *   H.264 encoder (esp_h264) whenever it is ready for the next, so a slow connection lowers the frame
 *   rate instead of breaking the stream. A key frame (with SPS and PPS) every H264_GOP_S seconds.
 * - MPEG-TS: camera_ts.c. The length is not known: chunked transfer encoding, framed here
 *   (esp_http_client only sets the header for a write length of -1 and writes raw bytes).
 * - Login: HTTP Basic (go2rtc api.username/password). https:// is checked against the ESP-IDF
 *   certificate bundle; mbedTLS memory is in PSRAM (sdkconfig.defaults).
 *
 * The "cam_push" task (core 0, stack in PSRAM) runs while the push is on, the camera serves and Wi-Fi
 * is connected. The encoder exists only during a session; its buffers are in PSRAM (camera_h264_mem.c). A session first sends a short GET to the same address: go2rtc answers 401 for a
 * wrong login at once, while for the POST it would only show after the body. Failures are retried
 * after 5 s, doubling up to 60 s. Settings in the machine store (key "campush"; the password in plain
 * text, as the Wi-Fi password).
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "camera_internal.h"
#include "camera_ts.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "machine_store.h"
#include "mbedtls/x509.h"
#include "net.h"

static const char *TAG = "cam_push";

#define STORE_KEY      "campush"
#define BLOB_VERSION   1
#define TASK_STACK     8192 /* TLS handshake */
#define TASK_PRIO      2
#define TIMEOUT_MS     10000
#define WAIT_POLL_MS   1000
#define RETRY_FIRST_MS 5000
#define RETRY_MAX_MS   60000
#define FRAME_WAIT_MS  100
#define NO_FRAME_MS    10000
#define H264_GOP_S     2  /* a key frame every 2 s: a viewer that joins waits at most that long */
#define H264_QP_MIN    20
#define H264_QP_MAX    42 /* the rate control never goes coarser than this */
#define URL_OLD        "/api/stream.mjpeg" /* the push of the first version */
#define URL_NEW        "/api/stream.ts"

typedef struct {
    uint8_t version;
    uint8_t on;
    uint8_t reserved[2];
    char url[CAMERA_PUSH_URL_MAX + 1];
    char user[CAMERA_PUSH_USER_MAX + 1];
    char pass[CAMERA_PUSH_PASS_MAX + 1];
} cfg_t;

/* Shared, under s_lock */
static SemaphoreHandle_t s_lock;
static cfg_t s_cfg;
static uint32_t s_gen; /* +1 at every settings change: a running session ends */
static camera_push_state_t s_state;
static char s_reason[CAMERA_REASON_MAX + 1];
static uint32_t s_kbit_s;

static TaskHandle_t s_task;

/* ---- Helpers ---- */

static void set_state(camera_push_state_t st, const char *reason)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = s_state != st || strcmp(s_reason, reason) != 0;
    s_state = st;
    strlcpy(s_reason, reason, sizeof(s_reason));
    if (st != CAMERA_PUSH_SENDING) {
        s_kbit_s = 0;
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        ESP_LOGI(TAG, "%s%s%s", camera_push_state_text(st), reason[0] ? ": " : "", reason);
    }
}

static void cfg_get(cfg_t *out, uint32_t *gen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    *gen = s_gen;
    xSemaphoreGive(s_lock);
}

static bool cfg_changed(uint32_t gen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = s_gen != gen;
    xSemaphoreGive(s_lock);
    return changed;
}

static void cfg_save(void)
{
    cfg_t c;
    uint32_t gen;
    cfg_get(&c, &gen);
    const esp_err_t err = machine_store_save(STORE_KEY, &c, sizeof(c));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the push settings failed: %s", esp_err_to_name(err));
    }
}

static bool wifi_connected(void)
{
    net_status_t n;
    net_status(&n);
    return n.state == NET_WIFI_CONNECTED;
}

/** Wait `ms` or until the settings change. */
static void pause_ms(uint32_t ms)
{
    (void)ulTaskNotifyTake(pdTRUE, ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(ms));
}

static bool write_all(esp_http_client_handle_t h, const void *data, size_t len)
{
    return esp_http_client_write(h, data, (int)len) == (int)len;
}

/** One chunk of the chunked body. */
static bool write_chunk(esp_http_client_handle_t h, const uint8_t *data, size_t len)
{
    char head[12];
    const int n = snprintf(head, sizeof(head), "%x\r\n", (unsigned)len);
    return write_all(h, head, (size_t)n) && write_all(h, data, len) && write_all(h, "\r\n", 2);
}

/** Why a connection failed: network (esp-tls opens plain TCP too) or TLS (certificate). */
static void connect_reason(esp_http_client_handle_t h, esp_err_t err, char *out, size_t len)
{
    int tls_code = 0, tls_flags = 0;
    const esp_err_t tls = esp_http_client_get_and_clear_last_tls_error(h, &tls_code, &tls_flags);
    /* esp-tls keeps the mbedTLS code negated (esp_tls_mbedtls.c: capture of -ret) */
    if (tls_flags != 0 || tls_code == -MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        /* Not signed by a CA of the ESP-IDF bundle (a self-signed one, say), expired, or another name */
        snprintf(out, len, "Server certificate not trusted (needs a public CA)");
    } else if (tls == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
        snprintf(out, len, "Server name not found (DNS)");
    } else if (tls == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
        snprintf(out, len, "Server not reachable: no answer (port forwarding?)");
    } else if (tls == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) {
        snprintf(out, len, "Server not reachable: connection refused");
    } else if (tls != ESP_OK && tls != ESP_FAIL) {
        snprintf(out, len, "Secure connection failed (%s)", esp_err_to_name(tls));
    } else {
        snprintf(out, len, "Server not reachable (%s)", esp_err_to_name(err));
    }
}

/** Why the server refused, from its HTTP status. */
static void status_reason(int status, char *out, size_t len)
{
    if (status == 401 || status == 403) {
        snprintf(out, len, "Wrong user or password (HTTP %d)", status);
    } else if (status == 404) {
        snprintf(out, len, "No stream of that name (dst=) on the server (HTTP 404)");
    } else if (status == 413 || status == 502 || status == 504) {
        snprintf(out, len, "The server's proxy refused the stream (HTTP %d)", status);
    } else {
        snprintf(out, len, "The server answered HTTP %d", status);
    }
}

/** An address of the first version (JPEG to /api/stream.mjpeg) to /api/stream.ts. @return true if changed */
static bool url_to_ts(char *url)
{
    char *p = strstr(url, URL_OLD);
    if (p == NULL) {
        return false;
    }
    char *rest = p + sizeof(URL_OLD) - 1;
    memmove(p + sizeof(URL_NEW) - 1, rest, strlen(rest) + 1); /* shorter: stays inside the string */
    memcpy(p, URL_NEW, sizeof(URL_NEW) - 1);
    return true;
}

/* ---- Session (push task) ---- */

/** H.264 bit rate for a picture size: CONFIG_CAMERA_H264_KBPS at 1920x1080, in proportion to the area. */
static uint32_t h264_bitrate(uint16_t w, uint16_t h)
{
    return (uint32_t)((uint64_t)CONFIG_CAMERA_H264_KBPS * 1000u * w * h / (1920u * 1080u));
}

static esp_h264_enc_handle_t encoder_new(uint16_t w, uint16_t h)
{
    const esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY, /* the only input of the encoder on P4 v1.x */
        .gop = H264_GOP_S * CONFIG_CAMERA_MAX_FPS,
        .fps = CONFIG_CAMERA_MAX_FPS,
        .res = {.width = w, .height = h},
        .rc = {.bitrate = h264_bitrate(w, h), .qp_min = H264_QP_MIN, .qp_max = H264_QP_MAX},
    };
    esp_h264_enc_handle_t enc = NULL;
    esp_h264_err_t err = esp_h264_enc_hw_new(&cfg, &enc);
    if (err == ESP_H264_ERR_OK) {
        err = esp_h264_enc_open(enc);
        if (err != ESP_H264_ERR_OK) {
            (void)esp_h264_enc_del(enc);
            enc = NULL;
        }
    }
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGW(TAG, "H.264 encoder %ux%u: error %d", w, h, (int)err);
        return NULL;
    }
    ESP_LOGI(TAG, "H.264 %ux%u, %d frames/s at most, key frame every %d s, %" PRIu32 " kbit/s", w, h,
             CONFIG_CAMERA_MAX_FPS, H264_GOP_S, h264_bitrate(w, h) / 1000);
    return enc;
}

static void encoder_del(esp_h264_enc_handle_t enc)
{
    if (enc) {
        (void)esp_h264_enc_close(enc);
        (void)esp_h264_enc_del(enc);
    }
}

#define FRAME_NONE  (-1) /* the session should end: settings, camera or Wi-Fi */
#define FRAME_LATE  (-2) /* no frame for NO_FRAME_MS */

/** The newest frame not seen yet, waiting for it. @return slot, FRAME_NONE or FRAME_LATE */
static int next_frame(uint32_t *seen, camera_yuv_frame_t *f, uint32_t gen)
{
    const TickType_t start = xTaskGetTickCount();
    while (camera_serving() && !cfg_changed(gen) && wifi_connected()) {
        const int slot = camera_yuv_take(seen, f);
        if (slot >= 0) {
            return slot;
        }
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS > NO_FRAME_MS) {
            return FRAME_LATE;
        }
        camera_frame_wait(FRAME_WAIT_MS);
    }
    return FRAME_NONE;
}

typedef struct {
    esp_http_client_handle_t h;
    uint32_t bytes; /* sent since the last rate sample */
} sink_t;

static bool sink_out(void *ctx, const uint8_t *data, size_t len)
{
    sink_t *s = ctx;
    if (!write_chunk(s->h, data, len)) {
        return false;
    }
    s->bytes += len;
    return true;
}

typedef struct {
    esp_h264_enc_handle_t enc;
    uint8_t *bs; /* one encoded frame */
    size_t bs_cap;
    uint8_t *ts; /* TS_BATCH packets */
    uint16_t w, h;
} coder_t;

/** Encode and send frames until the session should end. @return NULL, or why it failed */
static const char *send_frames(esp_http_client_handle_t h, const coder_t *c, uint32_t gen, char *why, size_t len)
{
    sink_t sink = {.h = h};
    ts_mux_t mux;
    ts_mux_init(&mux, c->ts, sink_out, &sink);
    uint32_t seen = 0;
    int64_t t0 = -1;
    int64_t window = esp_timer_get_time();
    for (;;) {
        camera_yuv_frame_t f;
        const int slot = next_frame(&seen, &f, gen);
        if (slot == FRAME_LATE) {
            return "No picture from the camera";
        } else if (slot < 0) {
            return NULL;
        }
        if (f.width != c->w || f.height != c->h) {
            camera_yuv_release(slot);
            return NULL; /* new picture size: a new session with a new encoder */
        }
        if (t0 < 0) {
            t0 = f.time_us;
        }
        const uint64_t pts = 90000u + (uint64_t)(f.time_us - t0) * 9u / 100u; /* 90 kHz from 1 s */
        esp_h264_enc_in_frame_t in = {.raw_data = {.buffer = (uint8_t *)f.buf, .len = f.len}, .pts = (uint32_t)pts};
        esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = c->bs, .len = c->bs_cap}};
        const esp_h264_err_t err = esp_h264_enc_process(c->enc, &in, &out);
        camera_yuv_release(slot);
        if (err != ESP_H264_ERR_OK) {
            snprintf(why, len, "H.264 encoding failed (error %d)", (int)err);
            return why;
        }
        const bool key = out.frame_type == ESP_H264_FRAME_TYPE_IDR || out.frame_type == ESP_H264_FRAME_TYPE_I;
        if (!ts_mux_frame(&mux, c->bs, out.length, key, pts)) {
            /* The server may have answered before it closed, e.g. 404 for an unknown stream name. */
            snprintf(why, len, "Connection lost");
            if (esp_http_client_fetch_headers(h) >= 0 && esp_http_client_get_status_code(h) >= 300) {
                status_reason(esp_http_client_get_status_code(h), why, len);
            }
            return why;
        }
        const int64_t now = esp_timer_get_time();
        if (now - window >= 1000000) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_kbit_s = (uint32_t)(sink.bytes * 8000LL / (now - window));
            xSemaphoreGive(s_lock);
            sink.bytes = 0;
            window = now;
        }
    }
}

/**
 * Check the login, then send frames until the settings change, the camera stops serving or the
 * connection fails. @return NULL when it ended on purpose, else why it failed
 */
static const char *session(const cfg_t *c, uint32_t gen)
{
    static char why[CAMERA_REASON_MAX + 1]; /* push task only */
    set_state(CAMERA_PUSH_CONNECTING, "");
    esp_http_client_config_t hc = {
        .url = c->url,
        .timeout_ms = TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    if (c->user[0]) {
        hc.username = c->user;
        hc.password = c->pass;
        hc.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    if (h == NULL) {
        return "Server address not usable";
    }

    /* 1. Reachable, and the login accepted? go2rtc answers 404 to this GET (no src given) once
     *    the login is right. */
    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK) {
        connect_reason(h, err, why, sizeof(why));
        esp_http_client_cleanup(h);
        return why;
    }
    (void)esp_http_client_fetch_headers(h);
    const int status = esp_http_client_get_status_code(h);
    esp_http_client_close(h);
    if (status == 401 || status == 403 || status >= 500 || status == 0) {
        if (status == 0) {
            snprintf(why, sizeof(why), "No answer from the server");
        } else {
            status_reason(status, why, sizeof(why));
        }
        esp_http_client_cleanup(h);
        return why;
    }

    /* 2. The encoder, sized by the first frame */
    if (!camera_push_enter()) {
        esp_http_client_cleanup(h);
        return NULL; /* the camera stopped meanwhile */
    }
    const char *result = NULL;
    coder_t k = {0};
    uint32_t seen = 0;
    camera_yuv_frame_t f;
    const int slot = next_frame(&seen, &f, gen);
    if (slot == FRAME_LATE) {
        result = "No picture from the camera";
    } else if (slot >= 0) {
        k.w = f.width;
        k.h = f.height;
        camera_yuv_release(slot);
        k.enc = encoder_new(k.w, k.h);
        k.bs = camera_dma_alloc((size_t)k.w * k.h / 2, &k.bs_cap); /* a frame stays far below this at QP >= 20 */
        k.ts = heap_caps_malloc(TS_BATCH * TS_PACKET, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (k.enc == NULL || k.bs == NULL || k.ts == NULL) {
            result = "H.264 encoder did not start (no memory?)";
        }
    }

    /* 3. The stream */
    if (slot >= 0 && result == NULL) {
        esp_http_client_set_method(h, HTTP_METHOD_POST);
        esp_http_client_set_header(h, "Content-Type", "video/mp2t");
        err = esp_http_client_open(h, -1); /* chunked */
        if (err != ESP_OK) {
            connect_reason(h, err, why, sizeof(why));
            result = why;
        } else {
            set_state(CAMERA_PUSH_SENDING, "");
            result = send_frames(h, &k, gen, why, sizeof(why));
            if (result == NULL) {
                (void)write_all(h, "0\r\n\r\n", 5); /* end of the body */
            }
        }
    }
    camera_push_leave();
    encoder_del(k.enc);
    heap_caps_free(k.bs);
    heap_caps_free(k.ts);
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return result;
}

static void push_task(void *arg)
{
    (void)arg;
    uint32_t retry_ms = RETRY_FIRST_MS;
    for (;;) {
        cfg_t c;
        uint32_t gen;
        cfg_get(&c, &gen);
        if (!c.on) {
            set_state(CAMERA_PUSH_OFF, "");
            retry_ms = RETRY_FIRST_MS;
            pause_ms(portMAX_DELAY);
            continue;
        }
        if (c.url[0] == '\0') {
            set_state(CAMERA_PUSH_WAITING, "Set the server address");
            pause_ms(portMAX_DELAY);
            continue;
        }
        if (!camera_serving()) {
            set_state(CAMERA_PUSH_WAITING, "Camera not running");
            pause_ms(WAIT_POLL_MS);
            continue;
        }
        if (!wifi_connected()) {
            set_state(CAMERA_PUSH_WAITING, "Wi-Fi not connected");
            pause_ms(WAIT_POLL_MS);
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, 0); /* a change during the session already ended it */
        const char *why = session(&c, gen);
        if (why == NULL) {
            retry_ms = RETRY_FIRST_MS;
            continue;
        }
        set_state(CAMERA_PUSH_RETRY, why);
        pause_ms(retry_ms);
        retry_ms = retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retry_ms * 2;
    }
}

/* ---- camera_internal.h ---- */

esp_err_t camera_push_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");
    cfg_t c;
    if (machine_store_load(STORE_KEY, &c, sizeof(c)) == ESP_OK && c.version == BLOB_VERSION) {
        c.url[CAMERA_PUSH_URL_MAX] = c.user[CAMERA_PUSH_USER_MAX] = c.pass[CAMERA_PUSH_PASS_MAX] = '\0';
    } else {
        c = (cfg_t){.version = BLOB_VERSION};
    }
    s_cfg = c;
    if (url_to_ts(s_cfg.url)) {
        ESP_LOGI(TAG, "push address changed to %s (H.264)", URL_NEW);
        cfg_save();
    }
    ESP_LOGI(TAG, "push %s%s%s", c.on ? "on" : "off", c.url[0] ? " to " : "", c.url);
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(push_task, "cam_push", TASK_STACK, NULL, TASK_PRIO, &s_task, 0,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void camera_push_status(camera_status_t *out)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->push = s_state;
    strlcpy(out->push_reason, s_reason, sizeof(out->push_reason));
    out->push_kbit_s = s_kbit_s;
    xSemaphoreGive(s_lock);
}

/* ---- Public ---- */

void camera_push_get(camera_push_cfg_t *out)
{
    *out = (camera_push_cfg_t){0};
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->on = s_cfg.on;
    strlcpy(out->url, s_cfg.url, sizeof(out->url));
    strlcpy(out->user, s_cfg.user, sizeof(out->user));
    out->has_password = s_cfg.pass[0] != '\0';
    xSemaphoreGive(s_lock);
}

esp_err_t camera_push_set_on(bool on)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.on = on;
    s_gen++;
    xSemaphoreGive(s_lock);
    cfg_save();
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t camera_push_set(const char *url, const char *user, const char *password)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    if (url && url[0] && strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((url && strlen(url) > CAMERA_PUSH_URL_MAX) || (user && strlen(user) > CAMERA_PUSH_USER_MAX) ||
        (password && strlen(password) > CAMERA_PUSH_PASS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (url) {
        strlcpy(s_cfg.url, url, sizeof(s_cfg.url));
        (void)url_to_ts(s_cfg.url); /* an address from the first version's documents */
    }
    if (user) {
        strlcpy(s_cfg.user, user, sizeof(s_cfg.user));
    }
    if (password) {
        strlcpy(s_cfg.pass, password, sizeof(s_cfg.pass));
    }
    s_gen++;
    xSemaphoreGive(s_lock);
    cfg_save();
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

const char *camera_push_state_text(camera_push_state_t s)
{
    switch (s) {
    case CAMERA_PUSH_OFF:
        return "Off";
    case CAMERA_PUSH_WAITING:
        return "Waiting";
    case CAMERA_PUSH_CONNECTING:
        return "Connecting";
    case CAMERA_PUSH_SENDING:
        return "Sending";
    case CAMERA_PUSH_RETRY:
        return "Not connected";
    }
    return "?";
}
