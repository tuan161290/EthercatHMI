/*
 * On-board Ethernet bring-up: ESP32-P4 EMAC (RMII) + IP101 PHY, no TCP/IP stack.
 *
 * Pins and clocking are verified against the schematic (see board_pins.h). They match the
 * IDF defaults for P4 but are written out so the board never depends on those defaults.
 */

#include "board_eth.h"
#include "board_pins.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy_ip101.h" /* espressif/ip101 from the registry since IDF 6.0 */
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "board_eth";

#define LINK_UP_BIT BIT0

static EventGroupHandle_t s_link_events;

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        xEventGroupSetBits(s_link_events, LINK_UP_BIT);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        xEventGroupClearBits(s_link_events, LINK_UP_BIT);
        ESP_LOGW(TAG, "link down");
        break;
    default:
        break;
    }
}

esp_err_t board_eth_init(uint32_t rx_task_prio, esp_eth_handle_t *out_eth)
{
    ESP_RETURN_ON_FALSE(out_eth, ESP_ERR_INVALID_ARG, TAG, "out_eth is NULL");

    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = BOARD_ETH_MDC_GPIO;
    emac.smi_gpio.mdio_num = BOARD_ETH_MDIO_GPIO;
    emac.interface = EMAC_DATA_INTERFACE_RMII;
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac.clock_config.rmii.clock_gpio = BOARD_ETH_REF_CLK_GPIO; /* an int on the P4 */
    emac.emac_dataif_gpio.rmii.tx_en_num = BOARD_ETH_TX_EN_GPIO;
    emac.emac_dataif_gpio.rmii.txd0_num = BOARD_ETH_TXD0_GPIO;
    emac.emac_dataif_gpio.rmii.txd1_num = BOARD_ETH_TXD1_GPIO;
    emac.emac_dataif_gpio.rmii.crs_dv_num = BOARD_ETH_CRS_DV_GPIO;
    emac.emac_dataif_gpio.rmii.rxd0_num = BOARD_ETH_RXD0_GPIO;
    emac.emac_dataif_gpio.rmii.rxd1_num = BOARD_ETH_RXD1_GPIO;
    emac.mdc_freq_hz = 0; /* <= 2.5 MHz, IP101 needs an MDC period >= 300 ns */

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_prio = rx_task_prio;
    mac_cfg.flags |= ETH_MAC_FLAG_PIN_TO_CORE; /* RX task + ISR stay on the calling core */

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = BOARD_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = BOARD_ETH_PHY_RST_GPIO;
    phy_cfg.post_hw_reset_delay_ms = 10; /* IP101G: RMII clock ready <= 10 ms after reset */

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "EMAC init failed (is the PHY 50 MHz clock running?)");
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (!phy) {
        mac->del(mac);
        ESP_LOGE(TAG, "PHY init failed");
        return ESP_FAIL;
    }

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    cfg.check_link_period_ms = 100;
    esp_eth_handle_t eth = NULL;
    esp_err_t err = esp_eth_driver_install(&cfg, &eth);
    if (err != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
        ESP_LOGE(TAG, "driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    /* EtherCAT needs neither pause frames nor the TCP/IP stack. No netif is attached. */
    bool off = false;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_S_FLOW_CTRL, &off), TAG, "flow ctrl off");

    if (!s_link_events) {
        s_link_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_link_events, ESP_ERR_NO_MEM, TAG, "event group");
        err = esp_event_loop_create_default();
        ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL),
                            TAG, "event handler");
    }

    uint32_t phy_addr = 0;
    esp_eth_ioctl(eth, ETH_CMD_G_PHY_ADDR, &phy_addr);
    ESP_LOGI(TAG, "EMAC + IP101 ready (PHY addr %lu, RX task prio %lu, core %d)",
             (unsigned long)phy_addr, (unsigned long)rx_task_prio, xPortGetCoreID());

    *out_eth = eth;
    return ESP_OK;
}

esp_err_t board_eth_wait_link(esp_eth_handle_t eth, uint32_t timeout_ms, bool *full_100m)
{
    EventBits_t bits = xEventGroupWaitBits(s_link_events, LINK_UP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & LINK_UP_BIT)) {
        return ESP_ERR_TIMEOUT;
    }

    eth_speed_t speed = ETH_SPEED_10M;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    esp_eth_ioctl(eth, ETH_CMD_G_SPEED, &speed);
    esp_eth_ioctl(eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
    ESP_LOGI(TAG, "link up: %s %s", speed == ETH_SPEED_100M ? "100M" : "10M",
             duplex == ETH_DUPLEX_FULL ? "full duplex" : "half duplex");
    if (full_100m) {
        *full_100m = (speed == ETH_SPEED_100M) && (duplex == ETH_DUPLEX_FULL);
    }
    return ESP_OK;
}

bool board_eth_link_is_up(void)
{
    return s_link_events && (xEventGroupGetBits(s_link_events) & LINK_UP_BIT);
}
