/*
 * Network: Wi-Fi station through the ESP32-C6 (ESP-Hosted over SDIO) and the clock from NTP.
 *
 * Everything runs on core 0 in the "net" task: every esp_wifi_* call is an RPC to the C6 that can
 * block for seconds, so the HMI only posts requests (these functions return at once) and polls
 * net_status(). Settings are kept in the machine store (key "wifi"), so they save also in OP.
 * A Wi-Fi fault never touches EtherCAT: the esp_hosted tasks are pinned to core 0 and the host
 * is never restarted by esp_hosted (sdkconfig.defaults).
 *
 * NTP: once connected, the clock is set from the NTP server and copied into the RTC, then again
 * every hour (CONFIG_LWIP_SNTP_UPDATE_DELAY). Without an RTC backup cell this is how the clock
 * comes back after a power cycle.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NET_SSID_MAX   32 /* bytes, without the terminator */
#define NET_PASS_MAX   63 /* WPA2 passphrase */
#define NET_SERVER_MAX 63
#define NET_SCAN_MAX   20
#define NET_NTP_DEFAULT "pool.ntp.org"

typedef enum {
    NET_WIFI_OFF,          /*!< switched off in the settings */
    NET_WIFI_STARTING,     /*!< waiting for the ESP32-C6 */
    NET_WIFI_FAILED,       /*!< the ESP32-C6 did not start (see reason) */
    NET_WIFI_IDLE,         /*!< on, no network chosen */
    NET_WIFI_CONNECTING,   /*!< joining the saved network */
    NET_WIFI_CONNECTED,    /*!< joined, with an IPv4 address */
    NET_WIFI_RETRY,        /*!< lost or refused (see reason); tries again by itself */
} net_wifi_state_t;

typedef struct {
    char ssid[NET_SSID_MAX + 1];
    int8_t rssi;  /*!< dBm */
    bool secured; /*!< needs a password */
} net_ap_t;

typedef struct {
    /* Settings */
    bool wifi_on;
    char ssid[NET_SSID_MAX + 1]; /*!< saved network, "" = none */
    bool ntp_on;
    char ntp_server[NET_SERVER_MAX + 1];
    /* State */
    net_wifi_state_t state;
    char reason[64];    /*!< why the last attempt failed, "" if none */
    int8_t rssi;        /*!< dBm while connected */
    uint8_t ip[4];      /*!< IPv4 address while connected */
    bool scanning;
    uint32_t scan_gen;  /*!< +1 after every scan, for polling net_scan_results() */
    time_t last_sync;   /*!< last NTP sync (UTC), 0 = never since start-up */
    char ntp_error[48]; /*!< why "Sync now" could not run, "" if none */
} net_status_t;

/** Load the settings and start the net task; Wi-Fi comes up if it is switched on. Core 0. */
esp_err_t net_start(void);

/** Snapshot of the settings and the state. Any task. */
void net_status(net_status_t *out);

/** Copy the last scan, strongest first. @return number copied */
int net_scan_results(net_ap_t *out, int max);

/** Switch Wi-Fi on or off (saved). */
esp_err_t net_set_wifi(bool on);

/** Save the network and join it; `password` NULL or "" for an open network. Switches Wi-Fi on. */
esp_err_t net_join(const char *ssid, const char *password);

/** Drop the saved network and disconnect. */
esp_err_t net_forget(void);

/** Look for networks; results after scan_gen changes. Stops a pending connection attempt meanwhile. */
esp_err_t net_scan(void);

/** Internet time on or off, and the server (NULL keeps it). Saved. */
esp_err_t net_set_ntp(bool on, const char *server);

/** Ask the NTP server now. ESP_ERR_INVALID_STATE when not connected or NTP is off. */
esp_err_t net_ntp_sync_now(void);

/** "Connected", "Connecting", ... */
const char *net_state_text(net_wifi_state_t s);

#ifdef __cplusplus
}
#endif
