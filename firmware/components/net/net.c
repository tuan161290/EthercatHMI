/*
 * Network (net.h): Wi-Fi station through the ESP32-C6 and the clock from NTP.
 *
 * One task on core 0 owns Wi-Fi: the public functions only change the settings under s_lock and
 * post a request; Wi-Fi and IP events are posted to the same queue. The task compares what is
 * wanted with what runs and acts. Retries after a lost or refused connection back off from 5 s
 * to 60 s.
 *
 * esp_hosted (2.11.7) starts itself from a constructor and creates its tasks without a core at
 * priority 23, above the EtherCAT cycle task (19) and emac_rx (20) on core 1. Its OS layer is a
 * table of function pointers (g_hosted_osi_funcs); hosted_pin_tasks() replaces thread create and
 * cancel before that constructor runs, so every esp_hosted task lands on core 0 with its stack in
 * PSRAM. Only fields 1-12 of the table are used: they come before its first #if, so their layout
 * does not depend on esp_hosted's private configuration.
 */

#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted_event.h"
#include "esp_hosted_os_abstraction.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "machine_clock.h"
#include "machine_store.h"
#include "net.h"

static const char *TAG = "net";

#define STORE_KEY      "wifi"
#define BLOB_VERSION   1
#define TASK_STACK     5120 /* 4096 left 852 bytes never used (console "mem", 2026-10-11) */
#define TASK_PRIO      3
#define HOSTED_CORE    0
#define QUEUE_LEN      16
#define RETRY_FIRST_MS 5000
#define RETRY_MAX_MS   60000
#define RSSI_POLL_MS   10000
#define HOSTNAME       "ethercat-hmi"

typedef struct {
    uint8_t version;
    uint8_t wifi_on;
    uint8_t ntp_on;
    uint8_t reserved;
    char ssid[NET_SSID_MAX + 1];
    char pass[NET_PASS_MAX + 1];
    char server[NET_SERVER_MAX + 1];
} cfg_t;

typedef enum {
    REQ_APPLY = 1, /* settings changed */
    REQ_JOIN,      /* a network was chosen: connect again even if it is the same */
    REQ_SCAN,
    REQ_SYNC,
    EV_STA_START,
    EV_DISCONNECTED, /* arg: reason */
    EV_GOT_IP,       /* arg: address */
    EV_LOST_IP,
    EV_SCAN_DONE,
    EV_TIME_SYNCED,
    EV_TRANSPORT_FAILED,
} msg_type_t;

typedef struct {
    msg_type_t type;
    int32_t arg;
} msg_t;

/* Shared, under s_lock */
static SemaphoreHandle_t s_lock;
static cfg_t s_cfg;
static net_status_t s_st; /* state part; the settings part is filled from s_cfg */
static EXT_RAM_BSS_ATTR net_ap_t s_aps[NET_SCAN_MAX];
static int s_n_aps;

static QueueHandle_t s_q;

/* Net task only */
static bool s_ready;      /* esp_wifi_init done */
static bool s_started;    /* esp_wifi_start done */
static bool s_wait_start; /* connect once WIFI_EVENT_STA_START has come */
static bool s_scan_running;
static bool s_sntp_running;
static char s_sntp_server[NET_SERVER_MAX + 1];
static char s_set_ssid[NET_SSID_MAX + 1]; /* what esp_wifi_set_config() was given */
static char s_set_pass[NET_PASS_MAX + 1];
static bool s_retry_pending;
static TickType_t s_retry_at;
static uint32_t s_retry_ms = RETRY_FIRST_MS;
static TickType_t s_rssi_at;

/* ---- esp_hosted tasks on core 0 ---- */

static void *net_hosted_thread_create(const char *name, uint32_t prio, uint32_t stack, void (*fn)(void const *),
                                      void *arg)
{
    TaskHandle_t *h = g_hosted_osi_funcs._h_malloc(sizeof(*h));
    if (h == NULL) {
        return NULL;
    }
    if (xTaskCreatePinnedToCoreWithCaps((TaskFunction_t)fn, name, stack, arg, prio, h, HOSTED_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        g_hosted_osi_funcs._h_free(h);
        return NULL;
    }
    return h;
}

static int net_hosted_thread_cancel(void *handle)
{
    if (handle == NULL) {
        return -1;
    }
    vTaskDeleteWithCaps(*(TaskHandle_t *)handle);
    g_hosted_osi_funcs._h_free(handle);
    return 0;
}

/* Before esp_hosted's own constructor (default priority, runs after all prioritised ones). */
static void __attribute__((constructor(200))) hosted_pin_tasks(void)
{
    g_hosted_osi_funcs._h_thread_create = net_hosted_thread_create;
    g_hosted_osi_funcs._h_thread_cancel = net_hosted_thread_cancel;
}

/* ---- Helpers ---- */

static void post(msg_type_t type, int32_t arg)
{
    const msg_t m = {.type = type, .arg = arg};
    if (s_q == NULL || xQueueSend(s_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "request %d dropped", (int)type);
    }
}

static void cfg_get(cfg_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

static void cfg_save(void)
{
    cfg_t c;
    cfg_get(&c);
    const esp_err_t err = machine_store_save(STORE_KEY, &c, sizeof(c));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the network settings failed: %s", esp_err_to_name(err));
    }
}

static void set_state(net_wifi_state_t st, const char *reason)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = s_st.state != st;
    s_st.state = st;
    if (reason) {
        strlcpy(s_st.reason, reason, sizeof(s_st.reason));
    }
    if (st != NET_WIFI_CONNECTED) {
        memset(s_st.ip, 0, sizeof(s_st.ip));
        s_st.rssi = 0;
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        ESP_LOGI(TAG, "Wi-Fi %s%s%s", net_state_text(st), reason && reason[0] ? ": " : "", reason ? reason : "");
    }
}

static net_wifi_state_t state_get(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const net_wifi_state_t st = s_st.state;
    xSemaphoreGive(s_lock);
    return st;
}

static void reason_text(int r, char *out, size_t len)
{
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
        snprintf(out, len, "Network not found");
        break;
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        snprintf(out, len, "Network found, but its security is not supported");
        break;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        snprintf(out, len, "Wrong password?");
        break;
    case WIFI_REASON_BEACON_TIMEOUT:
        snprintf(out, len, "Signal lost");
        break;
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        snprintf(out, len, "The access point refused the connection");
        break;
    default:
        snprintf(out, len, "Disconnected (reason %d)", r);
        break;
    }
}

/* ---- Events (default event loop task, core 0): forwarded to the net task ---- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        post(EV_STA_START, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        post(EV_DISCONNECTED, ((const wifi_event_sta_disconnected_t *)data)->reason);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        post(EV_SCAN_DONE, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        post(EV_GOT_IP, (int32_t)((const ip_event_got_ip_t *)data)->ip_info.ip.addr);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        post(EV_LOST_IP, 0);
    } else if (base == ESP_HOSTED_EVENT && id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        post(EV_TRANSPORT_FAILED, 0);
    }
}

/* lwIP task: the system clock was just set from NTP. */
static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    post(EV_TIME_SYNCED, 0);
}

/* ---- Wi-Fi (net task) ---- */

static esp_err_t wifi_init_once(void)
{
    esp_err_t err = esp_netif_init();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "netif");
    err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif, ESP_FAIL, TAG, "station netif");
    (void)esp_netif_set_hostname(netif, HOSTNAME);
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG,
                        "events");
    const wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "esp_wifi_init");
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM); /* the settings live in the machine store */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "station mode");
    s_ready = true;
    return ESP_OK;
}

static void retry_later(void)
{
    s_retry_pending = true;
    s_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(s_retry_ms);
    s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
}

static void connect_now(void)
{
    s_retry_pending = false;
    if (!s_started || s_wait_start || s_scan_running || s_set_ssid[0] == '\0') {
        return; /* STA_START or a finished scan connects */
    }
    set_state(NET_WIFI_CONNECTING, NULL);
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        char r[64];
        snprintf(r, sizeof(r), "Could not start connecting (%s)", esp_err_to_name(err));
        set_state(NET_WIFI_RETRY, r);
        retry_later();
    }
}

static void sntp_apply(void)
{
    cfg_t c;
    cfg_get(&c);
    const bool want = c.ntp_on && c.server[0] && state_get() == NET_WIFI_CONNECTED;
    if (s_sntp_running && (!want || strcmp(c.server, s_sntp_server) != 0)) {
        esp_netif_sntp_deinit();
        s_sntp_running = false;
    }
    if (want && !s_sntp_running) {
        strlcpy(s_sntp_server, c.server, sizeof(s_sntp_server));
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_sntp_server);
        sc.sync_cb = time_sync_cb;
        const esp_err_t err = esp_netif_sntp_init(&sc);
        if (err == ESP_OK) {
            s_sntp_running = true;
            ESP_LOGI(TAG, "asking %s for the time", s_sntp_server);
        } else {
            ESP_LOGE(TAG, "NTP start failed: %s", esp_err_to_name(err));
        }
    }
}

/* Make Wi-Fi match the settings. */
static void apply(bool rejoin)
{
    cfg_t c;
    cfg_get(&c);
    if (!c.wifi_on) {
        s_retry_pending = false;
        if (s_started) {
            (void)esp_wifi_stop();
            s_started = false;
            s_wait_start = false;
            s_scan_running = false;
            s_set_ssid[0] = '\0';
        }
        set_state(NET_WIFI_OFF, "");
        sntp_apply();
        return;
    }
    if (!s_ready) {
        set_state(NET_WIFI_STARTING, "");
        const esp_err_t err = wifi_init_once();
        if (err != ESP_OK) {
            char r[64];
            snprintf(r, sizeof(r), "Wi-Fi module (ESP32-C6) did not start: %s", esp_err_to_name(err));
            set_state(NET_WIFI_FAILED, r);
            return;
        }
    }
    if (!s_started) {
        const esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            char r[64];
            snprintf(r, sizeof(r), "Wi-Fi did not start: %s", esp_err_to_name(err));
            set_state(NET_WIFI_FAILED, r);
            return;
        }
        s_started = true;
        s_wait_start = true;
    }
    if (c.ssid[0] == '\0') {
        if (s_set_ssid[0]) {
            (void)esp_wifi_disconnect();
            s_set_ssid[0] = '\0';
        }
        s_retry_pending = false;
        set_state(NET_WIFI_IDLE, "");
        sntp_apply();
        return;
    }
    const bool changed = strcmp(c.ssid, s_set_ssid) != 0 || strcmp(c.pass, s_set_pass) != 0;
    if (changed || rejoin) {
        if (state_get() == NET_WIFI_CONNECTED || state_get() == NET_WIFI_CONNECTING) {
            (void)esp_wifi_disconnect(); /* its event (ASSOC_LEAVE) is ignored */
        }
        wifi_config_t wc = {0};
        memcpy(wc.sta.ssid, c.ssid, strlen(c.ssid));
        memcpy(wc.sta.password, c.pass, strlen(c.pass));
        wc.sta.threshold.authmode = c.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        wc.sta.pmf_cfg.capable = true;
        wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err != ESP_OK) {
            char r[64];
            snprintf(r, sizeof(r), "Network settings refused: %s", esp_err_to_name(err));
            set_state(NET_WIFI_FAILED, r);
            return;
        }
        strlcpy(s_set_ssid, c.ssid, sizeof(s_set_ssid));
        strlcpy(s_set_pass, c.pass, sizeof(s_set_pass));
        s_retry_ms = RETRY_FIRST_MS;
        set_state(NET_WIFI_CONNECTING, "");
        connect_now();
    } else if (state_get() != NET_WIFI_CONNECTED && state_get() != NET_WIFI_CONNECTING && !s_retry_pending) {
        connect_now();
    }
    sntp_apply();
}

static void on_disconnected(int reason)
{
    if (reason == WIFI_REASON_ASSOC_LEAVE) {
        return; /* we disconnected (new network, scan, off) and already set the state */
    }
    cfg_t c;
    cfg_get(&c);
    if (!c.wifi_on || c.ssid[0] == '\0' || s_scan_running) {
        return;
    }
    char r[64];
    reason_text(reason, r, sizeof(r));
    set_state(NET_WIFI_RETRY, r);
    sntp_apply();
    retry_later();
}

static void scan_start(void)
{
    if (!s_started || s_scan_running) {
        return;
    }
    if (state_get() != NET_WIFI_CONNECTED) {
        s_retry_pending = false; /* a connection attempt blocks the scan */
        (void)esp_wifi_disconnect();
    }
    const wifi_scan_config_t sc = {.show_hidden = false};
    const esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.scan_gen++;
        xSemaphoreGive(s_lock);
        if (state_get() != NET_WIFI_CONNECTED) {
            connect_now();
        }
        return;
    }
    s_scan_running = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.scanning = true;
    xSemaphoreGive(s_lock);
}

static void scan_done(void)
{
    uint16_t n = NET_SCAN_MAX * 2;
    wifi_ap_record_t *rec = heap_caps_malloc(n * sizeof(*rec), MALLOC_CAP_SPIRAM);
    net_ap_t *aps = heap_caps_malloc(NET_SCAN_MAX * sizeof(*aps), MALLOC_CAP_SPIRAM);
    int k = 0;
    if (rec && aps && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            const char *ssid = (const char *)rec[i].ssid;
            if (ssid[0] == '\0') {
                continue; /* hidden */
            }
            int j = 0;
            while (j < k && strcmp(aps[j].ssid, ssid) != 0) {
                j++;
            }
            if (j < k) {
                if (rec[i].rssi > aps[j].rssi) {
                    aps[j].rssi = rec[i].rssi; /* the same network from another access point */
                }
                continue;
            }
            if (k == NET_SCAN_MAX) {
                continue;
            }
            strlcpy(aps[k].ssid, ssid, sizeof(aps[k].ssid));
            aps[k].rssi = rec[i].rssi;
            aps[k].secured = rec[i].authmode != WIFI_AUTH_OPEN;
            k++;
        }
        for (int i = 1; i < k; i++) { /* strongest first */
            const net_ap_t a = aps[i];
            int j = i - 1;
            while (j >= 0 && aps[j].rssi < a.rssi) {
                aps[j + 1] = aps[j];
                j--;
            }
            aps[j + 1] = a;
        }
    } else {
        (void)esp_wifi_clear_ap_list();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (aps) {
        memcpy(s_aps, aps, (size_t)k * sizeof(*aps));
    }
    s_n_aps = k;
    s_st.scanning = false;
    s_st.scan_gen++;
    xSemaphoreGive(s_lock);
    heap_caps_free(rec);
    heap_caps_free(aps);
    s_scan_running = false;
    ESP_LOGI(TAG, "scan: %d network(s)", k);
    cfg_t c;
    cfg_get(&c);
    if (c.wifi_on && c.ssid[0] && state_get() != NET_WIFI_CONNECTED) {
        connect_now();
    }
}

static void time_synced(void)
{
    const esp_err_t err = machine_clock_save_to_rtc();
    const time_t now = time(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.last_sync = now;
    s_st.ntp_error[0] = '\0';
    xSemaphoreGive(s_lock);
    struct tm local;
    localtime_r(&now, &local);
    char text[32];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    ESP_LOGI(TAG, "clock set from %s: %s (local)%s", s_sntp_server, text,
             err == ESP_OK ? ", RTC updated" : ", RTC not written");
}

static void sync_now(void)
{
    const char *e = "";
    cfg_t c;
    cfg_get(&c);
    if (!c.ntp_on) {
        e = "Internet time is off";
    } else if (!s_sntp_running) {
        e = "Not connected to a network";
    } else if (esp_netif_sntp_start() != ESP_OK) {
        e = "Could not ask the server";
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.ntp_error, e, sizeof(s_st.ntp_error));
    xSemaphoreGive(s_lock);
}

static void rssi_poll(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.rssi = ap.rssi;
        xSemaphoreGive(s_lock);
    }
}

static void handle(const msg_t *m)
{
    switch (m->type) {
    case REQ_APPLY:
        cfg_save();
        apply(false);
        break;
    case REQ_JOIN:
        cfg_save();
        apply(true);
        break;
    case REQ_SCAN:
        scan_start();
        break;
    case REQ_SYNC:
        sync_now();
        break;
    case EV_STA_START:
        s_wait_start = false;
        connect_now();
        break;
    case EV_DISCONNECTED:
        on_disconnected((int)m->arg);
        break;
    case EV_GOT_IP: {
        const uint32_t a = (uint32_t)m->arg; /* network byte order: first octet in the low byte */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_st.state = NET_WIFI_CONNECTED;
        s_st.reason[0] = '\0';
        for (int i = 0; i < 4; i++) {
            s_st.ip[i] = (uint8_t)(a >> (8 * i));
        }
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Wi-Fi connected to %s, IP %u.%u.%u.%u", s_set_ssid, (unsigned)(a & 0xFF),
                 (unsigned)((a >> 8) & 0xFF), (unsigned)((a >> 16) & 0xFF), (unsigned)(a >> 24));
        s_retry_ms = RETRY_FIRST_MS;
        s_retry_pending = false;
        rssi_poll();
        s_rssi_at = xTaskGetTickCount() + pdMS_TO_TICKS(RSSI_POLL_MS);
        sntp_apply();
        break;
    }
    case EV_LOST_IP:
        if (state_get() == NET_WIFI_CONNECTED) {
            set_state(NET_WIFI_RETRY, "IP address lost");
            sntp_apply();
        }
        break;
    case EV_SCAN_DONE:
        scan_done();
        break;
    case EV_TIME_SYNCED:
        time_synced();
        break;
    case EV_TRANSPORT_FAILED:
        s_started = false;
        s_wait_start = false;
        s_scan_running = false;
        s_retry_pending = false;
        set_state(NET_WIFI_FAILED, "Wi-Fi module (ESP32-C6) stopped answering");
        sntp_apply();
        break;
    }
}

/* Ticks until `at`, 0 if it has passed. */
static TickType_t until(TickType_t at)
{
    const int32_t d = (int32_t)(at - xTaskGetTickCount());
    return d > 0 ? (TickType_t)d : 0;
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        const bool connected = state_get() == NET_WIFI_CONNECTED;
        TickType_t wait = s_retry_pending ? until(s_retry_at) : portMAX_DELAY;
        if (connected && until(s_rssi_at) < wait) {
            wait = until(s_rssi_at);
        }
        msg_t m;
        if (xQueueReceive(s_q, &m, wait) == pdTRUE) {
            handle(&m);
            continue;
        }
        if (s_retry_pending && until(s_retry_at) == 0) {
            connect_now();
        }
        if (connected && until(s_rssi_at) == 0) {
            rssi_poll();
            s_rssi_at = xTaskGetTickCount() + pdMS_TO_TICKS(RSSI_POLL_MS);
        }
    }
}

/* ---- Public ---- */

esp_err_t net_start(void)
{
    ESP_RETURN_ON_FALSE(s_q == NULL, ESP_ERR_INVALID_STATE, TAG, "started twice");
    s_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    ESP_RETURN_ON_FALSE(s_lock && s_q, ESP_ERR_NO_MEM, TAG, "queue");

    cfg_t c;
    if (machine_store_load(STORE_KEY, &c, sizeof(c)) == ESP_OK && c.version == BLOB_VERSION) {
        c.ssid[NET_SSID_MAX] = c.pass[NET_PASS_MAX] = c.server[NET_SERVER_MAX] = '\0';
    } else {
        c = (cfg_t){.version = BLOB_VERSION, .wifi_on = false, .ntp_on = true};
        strlcpy(c.server, NET_NTP_DEFAULT, sizeof(c.server));
    }
    s_cfg = c;
    s_st.state = NET_WIFI_OFF;
    ESP_LOGI(TAG, "Wi-Fi %s%s%s, internet time %s (%s)", c.wifi_on ? "on" : "off", c.ssid[0] ? ", network " : "",
             c.ssid, c.ntp_on ? "on" : "off", c.server);

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(net_task, "net", TASK_STACK, NULL, TASK_PRIO, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    post(REQ_APPLY, 0); /* brings Wi-Fi up in the task if it is on */
    return ESP_OK;
}

void net_status(net_status_t *out)
{
    if (s_lock == NULL) {
        *out = (net_status_t){.state = NET_WIFI_OFF};
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    out->wifi_on = s_cfg.wifi_on;
    strlcpy(out->ssid, s_cfg.ssid, sizeof(out->ssid));
    out->ntp_on = s_cfg.ntp_on;
    strlcpy(out->ntp_server, s_cfg.server, sizeof(out->ntp_server));
    xSemaphoreGive(s_lock);
}

int net_scan_results(net_ap_t *out, int max)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = s_n_aps < max ? s_n_aps : max;
    memcpy(out, s_aps, (size_t)n * sizeof(*out));
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t net_set_wifi(bool on)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.wifi_on = on;
    xSemaphoreGive(s_lock);
    post(REQ_APPLY, 0);
    return ESP_OK;
}

esp_err_t net_join(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    if (password == NULL) {
        password = "";
    }
    const size_t pl = strlen(password);
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > NET_SSID_MAX || pl > NET_PASS_MAX || (pl > 0 && pl < 8)) {
        return ESP_ERR_INVALID_ARG; /* WPA2 passphrases are 8..63 characters */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg.ssid, ssid, sizeof(s_cfg.ssid));
    strlcpy(s_cfg.pass, password, sizeof(s_cfg.pass));
    s_cfg.wifi_on = true;
    s_st.reason[0] = '\0';
    xSemaphoreGive(s_lock);
    post(REQ_JOIN, 0);
    return ESP_OK;
}

esp_err_t net_forget(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.ssid[0] = '\0';
    s_cfg.pass[0] = '\0';
    xSemaphoreGive(s_lock);
    post(REQ_APPLY, 0);
    return ESP_OK;
}

esp_err_t net_scan(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool on = s_cfg.wifi_on;
    xSemaphoreGive(s_lock);
    ESP_RETURN_ON_FALSE(on, ESP_ERR_INVALID_STATE, TAG, "Wi-Fi is off");
    post(REQ_SCAN, 0);
    return ESP_OK;
}

esp_err_t net_set_ntp(bool on, const char *server)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    if (server && (server[0] == '\0' || strlen(server) > NET_SERVER_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.ntp_on = on;
    if (server) {
        strlcpy(s_cfg.server, server, sizeof(s_cfg.server));
    }
    s_st.ntp_error[0] = '\0';
    xSemaphoreGive(s_lock);
    post(REQ_APPLY, 0);
    return ESP_OK;
}

esp_err_t net_ntp_sync_now(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not started");
    post(REQ_SYNC, 0);
    return ESP_OK;
}

const char *net_state_text(net_wifi_state_t s)
{
    switch (s) {
    case NET_WIFI_OFF:
        return "Off";
    case NET_WIFI_STARTING:
        return "Starting";
    case NET_WIFI_FAILED:
        return "Not available";
    case NET_WIFI_IDLE:
        return "Not connected";
    case NET_WIFI_CONNECTING:
        return "Connecting";
    case NET_WIFI_CONNECTED:
        return "Connected";
    case NET_WIFI_RETRY:
        return "Not connected, retrying";
    }
    return "?";
}
