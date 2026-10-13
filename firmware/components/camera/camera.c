/*
 * Camera (camera.h): capture, JPEG and the HTTP server, all on core 0.
 *
 * The "camera" task owns the video device, the JPEG encoder and the server. It compares what is
 * wanted (the setting, Wi-Fi, viewers) with what runs and acts, like the net task:
 * - switched on: finds the sensor (board_camera_init), opens the device in RGB565 and allocates
 *   the capture and JPEG buffers in PSRAM; once Wi-Fi is connected, starts the server;
 * - a viewer or a snapshot request starts the sensor; STOP_AFTER_MS after the last one it stops;
 * - switched off: stops the server, closes the device, frees the buffers.
 *
 * Frames: sensor -> CSI -> ISP (RGB565, CAP_BUFS buffers of esp_video) -> hardware JPEG, at most
 * CONFIG_CAMERA_MAX_FPS a second -> one of JPEG_BUFS buffers. The stream tasks (one per open
 * request, stacks in PSRAM) always send the newest JPEG, so a slow viewer skips frames instead of
 * queueing them. A JPEG buffer is never overwritten while a task sends it: every task holds at most
 * one, so N_WORKERS + 1 buffers always leave one free for the encoder.
 *
 * HTTP requests are handed from the server task to the stream tasks as async requests
 * (httpd_req_async_handler_begin), so a running stream never blocks the server.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "board_camera.h"
#include "camera.h"
#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "machine_store.h"
#include "net.h"

static const char *TAG = "camera";

#define STORE_KEY       "camera"
#define BLOB_VERSION    1
#define TASK_STACK      8192 /* sensor and esp_video/IPA set-up; 4096 overflowed on the board (2026-10-13) */
#define TASK_PRIO       2 /* below LVGL (4) and the net task (3) */
#define WORKER_STACK    4096
#define WORKER_PRIO     2
#define HTTP_STACK      4096
#define HTTP_PRIO       2
#define QUEUE_LEN       8
#define POLL_MS         250   /* camera task while the sensor is stopped */
#define CAP_BUFS        2     /* as the vendor demo (video_lcd_display EXAMPLE_CAM_BUF_COUNT) */
#define N_WORKERS       (CONFIG_CAMERA_MAX_VIEWERS + 1) /* streams plus one snapshot */
#define JPEG_BUFS       (N_WORKERS + 1)
#define JPEG_TIMEOUT_MS 200
#define DQBUF_TIMEOUT_S 1
#define DQBUF_FAILS_MAX 3     /* timeouts in a row before the camera counts as failed */
#define WARMUP_FRAMES   10    /* dropped after a start while exposure and white balance settle */
#define STOP_AFTER_MS   5000  /* keep the sensor running this long after the last viewer */
#define SNAPSHOT_MS     5000  /* a snapshot waits this long for a frame */
#define FRAME_WAIT_MS   100   /* stream tasks look for a new frame at least this often */
#define NO_FRAME_MS     10000 /* a stream ends after this long without a frame */
#define WIFI_POLL_MS    1000
#define EV_FRAME        BIT0

#define PART_BOUNDARY "ehmiframe"
static const char STREAM_TYPE[] = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char STREAM_BOUNDARY[] = "\r\n--" PART_BOUNDARY "\r\n";

typedef struct {
    uint8_t version;
    uint8_t on;
    uint8_t reserved[2];
} cfg_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint32_t seq;
    uint8_t users; /* stream tasks sending it */
} jpeg_slot_t;

typedef struct {
    httpd_req_t *req; /* async copy, completed by the stream task */
    bool stream;
} work_t;

/* Shared, under s_lock */
static SemaphoreHandle_t s_lock;
static bool s_on;    /* setting */
static bool s_retry; /* camera_set_on() since the last fault */
static camera_status_t s_st;
static jpeg_slot_t s_jpeg[JPEG_BUFS];
static int s_newest = -1; /* slot of the newest frame */
static uint32_t s_seq;    /* its number */
static uint8_t s_viewers; /* stream requests taken (queued or sending) */
static uint8_t s_busy;    /* stream tasks taken */
static bool s_serving;    /* stream tasks may send; false while the server stops */
static TickType_t s_want_until; /* a snapshot wants frames until then */
static uint32_t s_sent;   /* bytes sent since the last rate sample */

static QueueHandle_t s_q;      /* wakes the camera task */
static QueueHandle_t s_work_q; /* async requests for the stream tasks */
static EventGroupHandle_t s_ev;

/* Camera task only */
static int s_fd = -1;
static uint8_t *s_cap[CAP_BUFS];
static jpeg_encoder_handle_t s_enc;
static httpd_handle_t s_httpd;
static bool s_open;      /* device open, buffers allocated */
static bool s_streaming; /* sensor running */
static int s_warmup;
static int s_dq_fails;
static int64_t s_next_frame_us;
static TickType_t s_demand_at; /* last time someone wanted frames */
static TickType_t s_wifi_at;
static bool s_wifi_up;
static uint32_t s_win_frames; /* rate sample */
static int64_t s_win_start_us;

/* ---- Helpers ---- */

static void wake(void)
{
    const uint8_t m = 1;
    if (s_q) {
        (void)xQueueSend(s_q, &m, 0); /* a full queue wakes the task as well */
    }
}

static void set_state(camera_state_t st, const char *reason)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = s_st.state != st;
    s_st.state = st;
    if (reason) {
        strlcpy(s_st.reason, reason, sizeof(s_st.reason));
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        ESP_LOGI(TAG, "%s%s%s", camera_state_text(st), reason && reason[0] ? ": " : "", reason ? reason : "");
    }
}

static camera_state_t state_get(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const camera_state_t st = s_st.state;
    xSemaphoreGive(s_lock);
    return st;
}

static TickType_t ms_since(TickType_t t)
{
    return (xTaskGetTickCount() - t) * portTICK_PERIOD_MS;
}

/* ---- JPEG slots ---- */

/** Newest frame if it is not `*seen`: held until slot_release(). @return slot or -1 */
static int slot_take_newer(uint32_t *seen)
{
    int slot = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_newest >= 0 && s_jpeg[s_newest].seq != *seen) {
        slot = s_newest;
        s_jpeg[slot].users++;
        *seen = s_jpeg[slot].seq;
    }
    xSemaphoreGive(s_lock);
    return slot;
}

static void slot_release(int slot, size_t sent)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_jpeg[slot].users--;
    s_sent += sent;
    xSemaphoreGive(s_lock);
}

/** A slot nobody sends, for the encoder; the newest frame only if nothing else is free. */
static int slot_for_encoder(void)
{
    int slot = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < JPEG_BUFS && slot < 0; i++) {
        if (s_jpeg[i].users == 0 && i != s_newest) {
            slot = i;
        }
    }
    if (slot < 0 && s_newest >= 0 && s_jpeg[s_newest].users == 0) {
        slot = s_newest;
        s_newest = -1;
    }
    xSemaphoreGive(s_lock);
    return slot;
}

/* ---- Stream tasks ---- */

static bool serving(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool on = s_serving;
    xSemaphoreGive(s_lock);
    return on;
}

static void wait_frame(void)
{
    (void)xEventGroupWaitBits(s_ev, EV_FRAME, pdFALSE, pdFALSE, pdMS_TO_TICKS(FRAME_WAIT_MS));
}

static void send_stream(httpd_req_t *req)
{
    wake(); /* s_viewers was counted when the request was taken */
    httpd_resp_set_type(req, STREAM_TYPE);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    uint32_t seen = 0;
    TickType_t last = xTaskGetTickCount();
    while (serving()) {
        const int slot = slot_take_newer(&seen);
        if (slot < 0) {
            if (ms_since(last) > NO_FRAME_MS) {
                break;
            }
            wait_frame();
            continue;
        }
        last = xTaskGetTickCount();
        const jpeg_slot_t *j = &s_jpeg[slot];
        char part[96];
        const int n = snprintf(part, sizeof(part), "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               (unsigned)j->len);
        esp_err_t err = httpd_resp_send_chunk(req, STREAM_BOUNDARY, sizeof(STREAM_BOUNDARY) - 1);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, part, n);
        }
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, (const char *)j->buf, j->len);
        }
        slot_release(slot, err == ESP_OK ? j->len : 0);
        if (err != ESP_OK) {
            break; /* the viewer left */
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_viewers--;
    xSemaphoreGive(s_lock);
    wake();
}

static void send_snapshot(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t seen = s_seq; /* a frame made after the request */
    s_want_until = xTaskGetTickCount() + pdMS_TO_TICKS(SNAPSHOT_MS);
    xSemaphoreGive(s_lock);
    wake();
    const TickType_t start = xTaskGetTickCount();
    int slot = -1;
    while (serving() && ms_since(start) < SNAPSHOT_MS && (slot = slot_take_newer(&seen)) < 0) {
        wait_frame();
    }
    if (slot < 0) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "No picture from the camera");
        return;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
    const esp_err_t err = httpd_resp_send(req, (const char *)s_jpeg[slot].buf, s_jpeg[slot].len);
    slot_release(slot, err == ESP_OK ? s_jpeg[slot].len : 0);
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        work_t w;
        (void)xQueueReceive(s_work_q, &w, portMAX_DELAY);
        if (w.stream) {
            send_stream(w.req);
        } else {
            send_snapshot(w.req);
        }
        (void)httpd_req_async_handler_complete(w.req);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_busy--;
        xSemaphoreGive(s_lock);
    }
}

/* ---- HTTP handlers (server task) ---- */

static esp_err_t busy_reply(httpd_req_t *req, const char *text)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, text);
}

static esp_err_t queue_request(httpd_req_t *req, bool stream)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *refuse = !s_serving                                         ? "Camera is stopping"
                         : stream && s_viewers >= CONFIG_CAMERA_MAX_VIEWERS ? "Too many viewers"
                         : s_busy >= N_WORKERS                              ? "Camera is busy"
                                                                            : NULL;
    if (refuse == NULL) {
        s_busy++;
        s_viewers += stream ? 1 : 0;
    }
    xSemaphoreGive(s_lock);
    if (refuse) {
        return busy_reply(req, refuse);
    }
    work_t w = {.stream = stream};
    if (httpd_req_async_handler_begin(req, &w.req) != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_busy--;
        s_viewers -= stream ? 1 : 0;
        xSemaphoreGive(s_lock);
        return httpd_resp_send_500(req);
    }
    (void)xQueueSend(s_work_q, &w, portMAX_DELAY); /* N_WORKERS long and s_busy counted: never full */
    return ESP_OK;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    return queue_request(req, true);
}

static esp_err_t snapshot_handler(httpd_req_t *req)
{
    return queue_request(req, false);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Camera</title>"
        "<style>body{margin:0;background:#1b1b1b;color:#ccc;font:15px sans-serif;text-align:center}"
        "img{display:block;max-width:100%;height:auto;margin:0 auto}a{color:#7fc8a0}</style></head>"
        "<body><img src=\"/stream\" alt=\"Camera\"><p><a href=\"/snapshot.jpg\">Snapshot</a></p></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, sizeof(page) - 1);
}

/* ---- Server (camera task) ---- */

static esp_err_t server_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = CONFIG_CAMERA_HTTP_PORT;
    c.core_id = 0;
    c.task_priority = HTTP_PRIO;
    c.stack_size = HTTP_STACK;
    c.max_open_sockets = N_WORKERS + 2;
    c.lru_purge_enable = true;
    c.send_wait_timeout = 2;
    c.recv_wait_timeout = 5;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &c), TAG, "server");
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler},
        {.uri = "/snapshot.jpg", .method = HTTP_GET, .handler = snapshot_handler},
        {.uri = "/snapshot", .method = HTTP_GET, .handler = snapshot_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        (void)httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_serving = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "server on port %d: /stream, /snapshot.jpg", CONFIG_CAMERA_HTTP_PORT);
    return ESP_OK;
}

/** @return true once stopped; false while a stream task still finishes (try again later) */
static bool server_stop(void)
{
    if (s_httpd == NULL) {
        return true;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_serving = false;
    const uint8_t busy = s_busy;
    xSemaphoreGive(s_lock);
    if (busy) {
        return false; /* each stream task ends within FRAME_WAIT_MS plus a send timeout */
    }
    (void)httpd_stop(s_httpd);
    s_httpd = NULL;
    return true;
}

/* ---- Video device (camera task) ---- */

static void device_close(void)
{
    if (s_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)ioctl(s_fd, VIDIOC_STREAMOFF, &type);
        s_streaming = false;
    }
    if (s_fd >= 0) {
        close(s_fd); /* frees the capture buffers */
        s_fd = -1;
    }
    memset(s_cap, 0, sizeof(s_cap));
    if (s_enc) {
        (void)jpeg_del_encoder_engine(s_enc);
        s_enc = NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < JPEG_BUFS; i++) {
        heap_caps_free(s_jpeg[i].buf);
        s_jpeg[i] = (jpeg_slot_t){0};
    }
    s_newest = -1;
    xSemaphoreGive(s_lock);
    s_open = false;
}

/** Sensor, device in RGB565, buffers. @return "" or why it failed */
static const char *device_open(void)
{
    const esp_err_t err = board_camera_init();
    if (err == ESP_ERR_NOT_FOUND) {
        return "Camera not found: no answer on I2C (FPC2 cable?)";
    } else if (err != ESP_OK) {
        return "Camera start failed (esp_video)";
    }
    s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (s_fd < 0) {
        return "Camera device did not open";
    }
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (ioctl(s_fd, VIDIOC_G_FMT, &f) != 0) {
        return "Camera format not readable";
    }
    const uint32_t w = f.fmt.pix.width, h = f.fmt.pix.height;
    f = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    f.fmt.pix.width = w;
    f.fmt.pix.height = h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565; /* what the hardware JPEG encoder takes */
    if (ioctl(s_fd, VIDIOC_S_FMT, &f) != 0) {
        return "Camera refused RGB565";
    }
    struct timeval to = {.tv_sec = DQBUF_TIMEOUT_S};
    (void)ioctl(s_fd, VIDIOC_S_DQBUF_TIMEOUT, &to);
#if CONFIG_CAMERA_HMIRROR
    struct v4l2_ext_control mirror = {.id = V4L2_CID_HFLIP, .value = 1};
    struct v4l2_ext_controls ctrls = {.ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &mirror};
    if (ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        ESP_LOGW(TAG, "mirror not set");
    }
#endif
    struct v4l2_requestbuffers rb = {.count = CAP_BUFS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                     .memory = V4L2_MEMORY_MMAP};
    if (ioctl(s_fd, VIDIOC_REQBUFS, &rb) != 0) {
        return "No memory for the camera buffers";
    }
    for (int i = 0; i < CAP_BUFS; i++) {
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &b) != 0) {
            return "Camera buffer not readable";
        }
        s_cap[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, b.m.offset);
        if (s_cap[i] == NULL || s_cap[i] == MAP_FAILED) {
            return "Camera buffer not mapped";
        }
    }
    const jpeg_encode_engine_cfg_t ec = {.timeout_ms = JPEG_TIMEOUT_MS};
    if (jpeg_new_encoder_engine(&ec, &s_enc) != ESP_OK) {
        return "JPEG encoder did not start";
    }
    /* A JPEG of this sensor at quality <= 80 stays far below half the RGB565 size (not measured). */
    const jpeg_encode_memory_alloc_cfg_t mc = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    for (int i = 0; i < JPEG_BUFS; i++) {
        size_t got = 0;
        uint8_t *buf = jpeg_alloc_encoder_mem(w * h / 2, &mc, &got);
        if (buf == NULL) {
            return "No memory for the JPEG buffers";
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_jpeg[i] = (jpeg_slot_t){.buf = buf, .cap = got};
        xSemaphoreGive(s_lock);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.sensor, BOARD_CAMERA_SENSOR_NAME, sizeof(s_st.sensor));
    s_st.width = (uint16_t)w;
    s_st.height = (uint16_t)h;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s %" PRIu32 "x%" PRIu32 " RGB565, %d JPEG buffers of %u KB", BOARD_CAMERA_SENSOR_NAME, w, h,
             JPEG_BUFS, (unsigned)(w * h / 2 / 1024));
    s_open = true;
    return "";
}

static bool stream_on(void)
{
    for (int i = 0; i < CAP_BUFS; i++) { /* STREAMOFF took every buffer back */
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
        if (ioctl(s_fd, VIDIOC_QBUF, &b) != 0) {
            return false;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        return false;
    }
    s_streaming = true;
    s_warmup = WARMUP_FRAMES;
    s_dq_fails = 0;
    s_next_frame_us = 0;
    ESP_LOGI(TAG, "sensor started");
    return true;
}

static void stream_off(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    (void)ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    s_streaming = false;
    ESP_LOGI(TAG, "sensor stopped");
}

static void encode(const uint8_t *src, uint32_t len)
{
    const int slot = slot_for_encoder();
    if (slot < 0) {
        return;
    }
    const jpeg_encode_cfg_t cfg = {
        .width = s_st.width, /* written by this task only */
        .height = s_st.height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = CONFIG_CAMERA_JPEG_QUALITY,
    };
    uint32_t out = 0;
    const esp_err_t err = jpeg_encoder_process(s_enc, &cfg, src, len, s_jpeg[slot].buf, s_jpeg[slot].cap, &out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG: %s", esp_err_to_name(err));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_jpeg[slot].len = out;
    s_jpeg[slot].seq = ++s_seq;
    s_newest = slot;
    s_st.frames++;
    s_st.frame_bytes = out;
    xSemaphoreGive(s_lock);
    s_win_frames++;
    /* Wakes every task waiting now; the bit is cleared at once for the next frame. */
    (void)xEventGroupSetBits(s_ev, EV_FRAME);
    (void)xEventGroupClearBits(s_ev, EV_FRAME);
}

/** One frame from the sensor; compressed when the frame-rate limit allows. */
static void capture(void)
{
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (ioctl(s_fd, VIDIOC_DQBUF, &b) != 0) { /* DQBUF_TIMEOUT_S */
        if (++s_dq_fails >= DQBUF_FAILS_MAX) {
            stream_off();
            set_state(CAMERA_FAULT, "No picture from the sensor");
        }
        return;
    }
    s_dq_fails = 0;
    const int64_t now = esp_timer_get_time();
    if (s_warmup > 0) {
        s_warmup--;
    } else if ((b.flags & V4L2_BUF_FLAG_DONE) && b.index < CAP_BUFS && now >= s_next_frame_us) {
        /* On the 30 frames/s grid: the next frame due one period after this one was due. */
        const int64_t period = 1000000 / CONFIG_CAMERA_MAX_FPS;
        s_next_frame_us = (s_next_frame_us && now - s_next_frame_us < period) ? s_next_frame_us + period
                                                                               : now + period;
        encode(s_cap[b.index], b.bytesused);
    }
    (void)ioctl(s_fd, VIDIOC_QBUF, &b);
}

/* ---- Camera task ---- */

static bool wifi_connected(void)
{
    if (ms_since(s_wifi_at) >= WIFI_POLL_MS) {
        net_status_t n;
        net_status(&n);
        s_wifi_up = n.state == NET_WIFI_CONNECTED;
        s_wifi_at = xTaskGetTickCount();
    }
    return s_wifi_up;
}

/* Make the camera match the setting, Wi-Fi and the viewers. */
static void apply(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool on = s_on;
    const bool retry = s_retry;
    s_retry = false;
    const bool demand = s_viewers > 0 || (int32_t)(s_want_until - xTaskGetTickCount()) > 0;
    xSemaphoreGive(s_lock);

    if (!on) {
        if (!server_stop()) {
            return; /* stream tasks still finishing */
        }
        if (s_open) {
            device_close();
        }
        set_state(CAMERA_OFF, "");
        return;
    }
    if (state_get() == CAMERA_FAULT && !retry) {
        return; /* switch off and on (or on again) to retry */
    }
    if (!s_open) {
        set_state(CAMERA_STARTING, "");
        const char *why = device_open();
        if (why[0]) {
            device_close();
            set_state(CAMERA_FAULT, why);
            return;
        }
    } else if (retry && state_get() == CAMERA_FAULT && !s_streaming) {
        s_dq_fails = 0; /* "No picture from the sensor": try the stream again */
    }
    if (s_httpd == NULL) {
        if (!wifi_connected()) {
            set_state(CAMERA_WAIT_WIFI, "");
            return;
        }
        if (server_start() != ESP_OK) {
            set_state(CAMERA_FAULT, "HTTP server did not start");
            return;
        }
    }
    if (demand) {
        s_demand_at = xTaskGetTickCount();
    }
    if (demand && !s_streaming) {
        if (!stream_on()) {
            stream_off();
            set_state(CAMERA_FAULT, "Sensor did not start");
            return;
        }
    } else if (!demand && s_streaming && ms_since(s_demand_at) >= STOP_AFTER_MS) {
        stream_off();
    }
    set_state(s_streaming ? CAMERA_STREAMING : CAMERA_READY, "");
}

static void rate_sample(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t dt = now - s_win_start_us;
    if (dt < 1000000) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.fps_x10 = (uint16_t)(s_win_frames * 10000000LL / dt);
    s_st.kbit_s = (uint32_t)(s_sent * 8000LL / dt);
    s_sent = 0;
    xSemaphoreGive(s_lock);
    s_win_frames = 0;
    s_win_start_us = now;
}

static void camera_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t m;
        (void)xQueueReceive(s_q, &m, s_streaming ? 0 : pdMS_TO_TICKS(POLL_MS));
        apply();
        if (s_streaming) {
            capture(); /* paced by the sensor: one frame, at most DQBUF_TIMEOUT_S */
        }
        rate_sample();
    }
}

/* ---- Public ---- */

esp_err_t camera_start(void)
{
    ESP_RETURN_ON_FALSE(s_q == NULL, ESP_ERR_INVALID_STATE, TAG, "started twice");
    s_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(QUEUE_LEN, sizeof(uint8_t));
    s_work_q = xQueueCreate(N_WORKERS, sizeof(work_t));
    s_ev = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_lock && s_q && s_work_q && s_ev, ESP_ERR_NO_MEM, TAG, "queues");

    cfg_t c;
    if (machine_store_load(STORE_KEY, &c, sizeof(c)) != ESP_OK || c.version != BLOB_VERSION) {
        c = (cfg_t){.version = BLOB_VERSION, .on = false};
    }
    s_on = c.on;
    s_st = (camera_status_t){
        .state = CAMERA_OFF, .port = CONFIG_CAMERA_HTTP_PORT, .max_fps = CONFIG_CAMERA_MAX_FPS};
    ESP_LOGI(TAG, "camera %s (port %d, at most %d frames/s)", s_on ? "on" : "off", CONFIG_CAMERA_HTTP_PORT,
             CONFIG_CAMERA_MAX_FPS);

    /* Stacks in PSRAM: these tasks only wait, encode through DMA and send on sockets. */
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(camera_task, "camera", TASK_STACK, NULL, TASK_PRIO, NULL, 0,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    for (int i = 0; i < N_WORKERS; i++) {
        char name[12];
        snprintf(name, sizeof(name), "cam_http%d", i);
        ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(worker_task, name, WORKER_STACK, NULL, WORKER_PRIO, NULL,
                                                            0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "stream task");
    }
    s_win_start_us = esp_timer_get_time();
    wake();
    return ESP_OK;
}

void camera_status(camera_status_t *out)
{
    if (s_lock == NULL) {
        *out = (camera_status_t){.state = CAMERA_OFF, .port = CONFIG_CAMERA_HTTP_PORT,
                                 .max_fps = CONFIG_CAMERA_MAX_FPS};
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    out->on = s_on;
    out->viewers = s_viewers;
    xSemaphoreGive(s_lock);
}

esp_err_t camera_set_on(bool on)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_on = on;
    s_retry = true;
    xSemaphoreGive(s_lock);
    const cfg_t c = {.version = BLOB_VERSION, .on = on};
    const esp_err_t err = machine_store_save(STORE_KEY, &c, sizeof(c));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the camera setting failed: %s", esp_err_to_name(err));
    }
    wake();
    return ESP_OK;
}

const char *camera_state_text(camera_state_t s)
{
    switch (s) {
    case CAMERA_OFF:
        return "Off";
    case CAMERA_STARTING:
        return "Starting";
    case CAMERA_FAULT:
        return "Not available";
    case CAMERA_WAIT_WIFI:
        return "Waiting for Wi-Fi";
    case CAMERA_READY:
        return "Ready";
    case CAMERA_STREAMING:
        return "Streaming";
    }
    return "?";
}
