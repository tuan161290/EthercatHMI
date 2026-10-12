/*
 * SOEM hardware abstraction for ESP-IDF esp_eth.
 *
 * RX: esp_eth hands every received frame (malloc'd, ownership transferred) to
 * soem_eth_input() in the EMAC RX task. EtherCAT frames are queued for SOEM,
 * everything else is freed. TX: frames go straight to esp_eth_transmit(), which
 * copies them into the DMA ring.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file SOEM/LICENSE.md distributed with this software for
 * full license information.
 */

#include <stdlib.h>
#include <string.h>
#include "oshw.h"
#include "osal_esp.h"
#include "soem_esp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/queue.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static const char *TAG = "soem_esp";

typedef struct
{
   uint8_t *buf;
   uint32_t len;
} rx_frame_t;

static esp_eth_handle_t s_eth;
static QueueHandle_t s_rxq;
static soem_esp_stats_t s_stats;

static void flush_rx_queue(void)
{
   rx_frame_t f;
   while (s_rxq && xQueueReceive(s_rxq, &f, 0) == pdTRUE)
   {
      free(f.buf);
   }
}

static esp_err_t soem_eth_input(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *priv)
{
   /* EtherType at offset 12, big endian */
   if (len >= ETH_HEADERSIZE && buf[12] == (ETH_P_ECAT >> 8) && buf[13] == (ETH_P_ECAT & 0xff))
   {
      rx_frame_t f = {.buf = buf, .len = len};
      if (xQueueSend(s_rxq, &f, 0) == pdTRUE)
      {
         s_stats.rx_frames++;
         return ESP_OK;
      }
      s_stats.rx_dropped++;
   }
   else
   {
      s_stats.rx_non_ecat++;
   }
   free(buf);
   return ESP_OK;
}

esp_err_t soem_esp_attach(esp_eth_handle_t eth)
{
   ESP_RETURN_ON_FALSE(eth, ESP_ERR_INVALID_ARG, TAG, "no eth handle");
   if (!s_rxq)
   {
      s_rxq = xQueueCreate(CONFIG_SOEM_RX_QUEUE_LEN, sizeof(rx_frame_t));
      ESP_RETURN_ON_FALSE(s_rxq, ESP_ERR_NO_MEM, TAG, "rx queue");
   }
   ESP_RETURN_ON_ERROR(osal_esp_init(), TAG, "osal timer");
   flush_rx_queue();
   memset(&s_stats, 0, sizeof(s_stats));
   ESP_RETURN_ON_ERROR(esp_eth_update_input_path(eth, soem_eth_input, NULL), TAG, "input path");
   s_eth = eth;
   return ESP_OK;
}

void soem_esp_detach(void)
{
   if (s_eth)
   {
      esp_eth_update_input_path(s_eth, NULL, NULL);
      s_eth = NULL;
   }
   flush_rx_queue();
}

void soem_esp_get_stats(soem_esp_stats_t *out)
{
   *out = s_stats;
}

int oshw_mac_init(const uint8_t *mac_address)
{
   if (!s_eth)
   {
      ESP_LOGE(TAG, "soem_esp_attach() must be called before ecx_init()");
      return -1;
   }
   flush_rx_queue();
   return 0;
}

int oshw_mac_send(const void *payload, size_t tot_len)
{
   if (esp_eth_transmit(s_eth, (void *)payload, tot_len) != ESP_OK)
   {
      s_stats.tx_errors++;
      return -1;
   }
   s_stats.tx_frames++;
   return (int)tot_len;
}

int oshw_mac_recv(void *buffer, size_t buffer_length)
{
   rx_frame_t f;
   size_t n;

   if (xQueueReceive(s_rxq, &f, 0) != pdTRUE)
   {
      return 0;
   }
   n = MIN(f.len, buffer_length);
   memcpy(buffer, f.buf, n);
   free(f.buf);
   return (int)n;
}

int oshw_mac_wait_rx(uint32_t timeout_us)
{
   rx_frame_t f;
   const uint32_t tick_us = portTICK_PERIOD_MS * 1000;
   /* A queue wait returns as soon as a frame arrives; the timeout only bounds a lost frame. */
   TickType_t ticks = (timeout_us + tick_us - 1) / tick_us;

   return xQueuePeek(s_rxq, &f, ticks) == pdTRUE;
}

/**
 * Host to Network byte order (i.e. to big endian).
 *
 * Note that Ethercat uses little endian byte order, except for the Ethernet
 * header which is big endian as usual.
 */
uint16 oshw_htons(uint16 host)
{
   return __builtin_bswap16(host);
}

/**
 * Network (i.e. big endian) to Host byte order.
 *
 * Note that Ethercat uses little endian byte order, except for the Ethernet
 * header which is big endian as usual.
 */
uint16 oshw_ntohs(uint16 network)
{
   return __builtin_bswap16(network);
}

/** Create list over available network adapters: the single on-chip EMAC.
 * @return First element in linked list of adapters
 */
ec_adaptert *oshw_find_adapters(void)
{
   ec_adaptert *adapter = calloc(1, sizeof(*adapter));

   if (adapter)
   {
      strncpy(adapter->name, "eth0", EC_MAXLEN_ADAPTERNAME - 1);
      strncpy(adapter->desc, "ESP32-P4 EMAC", EC_MAXLEN_ADAPTERNAME - 1);
   }
   return adapter;
}

/** Free allocated memory used by adapter collection.
 * @param[in] adapter = First element in linked list of adapters
 * EC_NOFRAME.
 */
void oshw_free_adapters(ec_adaptert *adapter)
{
   while (adapter)
   {
      ec_adaptert *next = adapter->next;
      free(adapter);
      adapter = next;
   }
}
