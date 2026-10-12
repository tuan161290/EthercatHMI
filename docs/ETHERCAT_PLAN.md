# ETHERCAT_PLAN.md — EtherCAT master + HMI trên JC1060P470C_I_W_Y

Nguồn và quy ước trích dẫn giống `docs/BOARD.md`. Các nguồn web được ghi bằng URL.

- **"Ước lượng"**: suy luận kỹ thuật, chưa có số đo.
- **"(chưa xác minh)"**: cần kiểm tra trên board thật hoặc trong mã nguồn ESP-IDF.

## 1. Kết luận khả thi

**KHẢ THI về phần cứng. Phần mềm phải tự port; chưa có ai công bố số đo thời gian thực.**

**Phần cứng**
- Đã đủ, không cần thêm MAC hay PHY.
- EMAC nội của P4 nối với PHY IP101GR/GRI (suy luận) qua RMII, 100 Mbit/s full-duplex, có auto-MDIX (IP101G-DS p45-46; DS p.66-67).
- Ánh xạ chân trùng 1:1 với `ETH_ESP32_EMAC_DEFAULT_CONFIG()` của IDF v5.5.4 (xem BOARD.md mục 3.1).

**Driver**
- Không có điểm chặn nào.
- `esp_eth_update_input_path()` cho đường nhận raw L2, `esp_eth_transmit()` cho đường gửi.
- EtherCAT dùng broadcast FF:FF:FF:FF:FF:FF và frame broadcast đi qua bộ lọc MAC mặc định (suy luận từ việc ARP/DHCP của demo chạy được).
- Flow control tắt sẵn. Có thể bật promiscuous bằng ioctl.

**Stack**
- Chưa có EtherCAT master nào cho ESP32-P4 / ESP-IDF. Tìm trên GitHub "ethercat esp32p4" ra 0 kết quả.
- Repo ESP32 + SOEM + EMAC duy nhất là `qcarver/soem_poe32`, viết cho ESP32 classic, đang thiếu component `soem_port`.
- Phải **tự viết lớp OSAL và NIC**.

**Chu kỳ (ước lượng)**

| Chu kỳ | Đánh giá |
|---|---|
| 1 ms có DC | Mục tiêu đầu tiên |
| 500 µs | Có thể đạt |
| ≤ 250 µs | **Không cam kết** với `esp_eth` gốc, vì mỗi frame nhận phải qua ISR → task notify → `malloc` + `memcpy` → callback |

Chỉ chốt chu kỳ sau khi có histogram jitter đo được ở Phase 3.

**Hạn chế cố định**
- Chỉ có 1 cổng RJ45, nên không làm được cable redundancy.
- Lưu lượng IP phải đi qua Wi-Fi (C6) hoặc EoE.

## 2. So sánh stack và khuyến nghị

| Stack | License | Đã chạy trên P4? | DC | CoE | FoE / EoE / SoE | Ghi chú |
|---|---|---|---|---|---|---|
| **SOEM v2.0.0** (2025-07-11) | **GPLv3 hoặc thương mại** (RT-Labs: từ €490/năm + phí đăng ký €3.000) | Không. Phải tự viết `osal` và `oshw`; dùng port `rtk` làm mẫu. | Có (`ec_dc.c`, PI loop trong `ec_sample`) | Có | Có / Có / Có | Trưởng thành nhất. Có ENI → C (`eniconv.py`), giới hạn cấu hình bằng CMake. Master vẫn nhận bugfix tới 2026-06. |
| SOEM v1.4.0 (2019) | GPLv2 + linking exception. **Phải có thêm Beckhoff EtherCAT Master License**, theo ETG FAQ là miễn phí nhưng là thỏa thuận riêng. | Không | Có | Có | EoE qua mailbox | Code cũ, thiếu ENI parser và cyclic mailbox |
| **CherryECAT v2.0.1** | **Apache-2.0**. FoE, EoE và redundancy có license riêng; file `ec_foe.c`/`ec_eoe.c` trong repo công khai dài 0 byte. | Không. Lớp FreeRTOS gọi `taskENTER_CRITICAL()` không có tham số, không hợp với SMP của IDF. | Có ("jitter < 3 µs" trên HPM/STM32H7) | Có | Tách riêng | Yêu cầu CoreMark/MHz > 5. P4 chỉ đạt khoảng 3.46/MHz mỗi nhân (DS p3: 2489.62 khi chạy 2 nhân ở 360 MHz). |
| acontis EC-Master, Koenig KPA | Thương mại | Không có trong danh sách hỗ trợ | Có | Có | Có | Driver cho P4 sẽ là dự án port riêng |
| IgH EtherLab, KickCAT | GPLv2/LGPL, CeCILL-C | Không (chỉ Linux/PikeOS) | — | — | — | Loại |
| EtherCrab (Rust) | MIT/Apache | `esp-hal` có Ethernet cho P4 (PR #5556) | Có | Có | — | Không hợp với dự án C + ESP-IDF + LVGL |
| Hilscher COMX 100CA-RE | Module phần cứng | Không phụ thuộc host (SPI/DPM) | Có | Có | Có | **Phương án B** nếu không đạt timing hoặc vướng license |

**Khuyến nghị**

1. **Dùng SOEM v2.0.0**, theo dõi `master` để nhận bugfix.
2. Tự viết hai lớp:
   - `osal/esp-idf`: `esp_timer` cho thời gian monotonic, mutex FreeRTOS, `xTaskCreatePinnedToCore(...,1)`.
   - `oshw/esp-idf`: chép `nicdrv.c` từ port `rtk`. `oshw_mac_send` gọi `esp_eth_transmit`. `oshw_mac_recv` đọc từ queue do callback `esp_eth_update_input_path` đổ vào.
3. Dùng CMake option của SOEM để thu nhỏ các giới hạn kích thước.
4. **Giữ một lớp mỏng giữa stack và nền tảng** (TX/RX frame, timer chu kỳ, nguồn thời gian). Lớp oshw/netdev của SOEM và CherryECAT cần gần như cùng các primitive, nên vẫn đổi stack được nếu sau này quyết định license khác.

**Hệ quả license** (cần ý kiến pháp lý trước khi bán)

- **SOEM v2 (GPLv3):**
  - SOEM được link tĩnh vào firmware. Nếu bán sản phẩm đóng thì phải **mua license thương mại của RT-Labs** hoặc **công bố toàn bộ firmware theo GPLv3**.
  - GPLv3 §6 "Installation Information" chỉ áp dụng cho "User Product". Thiết bị HMI công nghiệp có thuộc nhóm này không là câu hỏi pháp lý.
- **CherryECAT:** phần lõi (CoE + DC) miễn phí cho thương mại. FoE và EoE phải xin license riêng.
- **Mọi master stack**, kể cả CherryECAT: điều khoản Beckhoff/ETG về EtherCAT Master License được nhắc rõ trong LICENSE của SOEM v1.4.0. Cần xác nhận với ETG (miễn phí theo ETG FAQ 2008).
- **Cách làm đề xuất:** phát triển dưới GPLv3 ngay bây giờ, chốt license **trước khi giao sản phẩm đầu tiên**.

## 3. Kiến trúc hệ thống

### 3.1 Phân chia nhân CPU và task

| | **Core 0 — HMI và mọi thứ khác** | **Core 1 — thời gian thực EtherCAT** |
|---|---|---|
| Task / ISR | LVGL port task (`task_affinity = 0`, prio 4, stack 16 KB). Vendor để `-1`, xem `IDF_N/lvgl_demo_v9/main/main.c:17-25`. | ISR của EMAC: cấp phát trên nhân gọi `esp_eth_driver_install`, nên phải install từ task đã pin vào core 1. |
| | LVGL draw thread: `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1`. Vendor để 2 (`lvgl_demo_v9/sdkconfig:2777`). `lv_freertos.c` tạo thread bằng `xTaskCreate` không pin, nên có thể trôi sang core 1. | Task `emac_rx`: `ETH_MAC_FLAG_PIN_TO_CORE`, prio **20**. Pin theo nhân gọi `esp_eth_mac_new_esp32()`. |
| | ESP-Hosted / Wi-Fi, lwIP tcpip (`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`), task `esp_timer` | ISR GPTimer: `CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM`, tạo từ core 1. Báo `ecat_cycle` bằng task notify. |
| | Worker SOEM cho mailbox/SDO, giám sát state machine, app logic, log, NVS, SD | Task `ecat_cycle` prio **19**, **thấp hơn `emac_rx`**: gửi/nhận PDO, kiểm tra WKC, PI loop DC, cập nhật snapshot. |

**Quy tắc cho core 1**
- Không printf, không malloc, không ghi flash.
- IOmap và các buffer nằm ở RAM nội. Code nóng đặt trong IRAM.
- Không gắn driver nào khác vào core 1.

**Watchdog idle CPU1:** `ecat_cycle` phải block để chờ (notify hoặc queue có timeout), không busy-poll. Nếu không làm được thì tắt `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1` trong lúc đo.

**Sửa `ecx_waitinframe`:** bản rtk của SOEM busy-poll. Hãy đổi thành chờ trên queue hoặc semaphore với timeout bằng `EC_TIMEOUTRET`.

### 3.2 Nhịp chu kỳ

- `FREERTOS_HZ = 1000` (1 ms tick) quá thô để tạo nhịp, nên **không dùng `vTaskDelay`**.
- Dùng GPTimer với alarm tuyệt đối: `next = prev + cycle + DC_offset`.
- `esp_timer` có độ phân giải 1 µs, nhưng tác vụ dispatch tối thiểu khoảng 20–50 µs. Tài liệu IDF khuyến nghị GPTimer.

### 3.3 Trao đổi dữ liệu HMI ↔ process image

- **EtherCAT → HMI:** dùng seqlock (hoặc double buffer) cho snapshot `pi_snapshot_t`: đầu vào, trạng thái slave, WKC, thống kê latency.
  - Phía ghi (core 1): `seq++` thành số lẻ → `memcpy` → `seq++` thành số chẵn. Dùng `__atomic` kèm memory barrier.
  - Phía đọc (core 0): đọc `seq`, copy, đọc lại `seq`; thử lại nếu số lẻ hoặc `seq` đã đổi.
- **HMI → EtherCAT:** queue lệnh FreeRTOS, ví dụ `{slave, offset, value}` hoặc lệnh trạng thái. `ecat_cycle` rút queue với timeout 0 ở đầu mỗi chu kỳ, rồi ghi vào output trong IOmap.
- **Tần suất HMI:** LVGL dùng `lv_timer` 30–60 Hz để đọc snapshot.
- **Tuyệt đối không gọi `ecx_*` từ LVGL.** Lệnh SDO đi qua worker trên core 0.
- **An toàn:** khi mất link hoặc WKC sai N chu kỳ liên tiếp, đưa output về trạng thái an toàn, báo HMI và tự phục hồi theo `fieldbus_check_state` của `simple_ng`.

### 3.4 Đường raw L2 (không có lwIP trên EMAC)

**Không gọi** `esp_netif_new` / `esp_eth_new_netif_glue` / `esp_netif_attach` cho handle EMAC. Demo vendor có gọi ở `IDF_N/ethernet/main/ethernet_example_main.c:85-89`; phải bỏ. Nếu không bỏ:
- DHCP, ARP và IPv6 MLD sẽ phát lên segment EtherCAT;
- input path bị lwIP chiếm.

**Thứ tự gọi**
1. `esp_eth_driver_install`
2. `esp_eth_update_input_path(cb)`. Callback nhận quyền sở hữu buffer và **phải `free()`**.
3. `ETH_CMD_S_FLOW_CTRL = false`
4. `esp_eth_start`

**Truyền:** `esp_eth_transmit()` (driver `memcpy` sang DMA và tự thêm padding và CRC). Bật `CONFIG_ETH_TRANSMIT_MUTEX`, vì SOEM truyền từ nhiều task.

**Link:** giữ autonegotiation. Sau sự kiện `ETHERNET_EVENT_CONNECTED`, kiểm tra 100M/FULL; sai thì không vào OP. IP101G rơi về half-duplex nếu phía kia không autoneg.

**PHY (khuyến nghị):**
- Tắt quảng bá EEE (IP101 MMD 7.60 bit 1, mặc định là 1; IP101G-DS p33).
- Giữ pause advertisement = 0.

### 3.5 Wi-Fi qua C6 (tùy chọn, cho truy cập từ xa)

- lwIP chỉ gắn với netif Wi-Fi: `esp_wifi_remote` + ESP-Hosted, SDIO slot 1, chạy trên core 0.
- Dịch vụ từ xa (web, MQTT hoặc Modbus TCP, chưa quyết định) chỉ đọc snapshot và gửi lệnh qua cùng queue như HMI.
- Phải đo lại jitter khi có lưu lượng Wi-Fi (Phase 3/5).

### 3.6 Topology động: Scan → Xác nhận → Lưu (người dùng chốt 2026-10-03)

**Trạng thái:** đã code ngày 2026-10-03, build sạch, **chưa chạy trên phần cứng**.
- Code nằm trong `firmware/components/ecat/` và `firmware/main/`.
- Chưa có HMI, nên các thao tác SCAN, ACCEPT, RE-CHECK tạm làm bằng lệnh console (`docs/SETUP.md` mục 7).
- Lệnh `selftest` kiểm tra logic so sánh theo các kịch bản của mockup, không cần bus.

**Mục tiêu.** Danh sách trục và IO được tạo từ những gì thực sự có trên bus. Tuy vậy máy **chỉ chạy khi bus khớp với topology đã được người có quyền xác nhận và lưu**. Nếu danh sách tự động thay đổi mỗi lần khởi động thì một drive hỏng hoặc một sợi cáp đổi chỗ có thể khiến "AX3" trỏ sang motor khác, và motor đó sẽ chạy với soft limit và điểm teach của trục khác.

**Bảng thiết bị hỗ trợ** (biên dịch sẵn trong firmware, lập từ ESI trong `vendor/ESI/`, xem `SLAVES_LEADSHINE.md`):
- Khoá tra là Vendor + Product. Mỗi dòng cho biết:
  - loại thiết bị (servo CiA402, stepper CiA402, coupler IO);
  - tên hiển thị;
  - hook cấu hình (remap PDO của drive, hook R1EC);
  - với R1EC: bảng module (ident → số bit, PDO, InitCmd).
- Thêm loại thiết bị mới nghĩa là thêm ESI và một dòng vào bảng. Dòng này có thể sinh bằng script.

**SCAN**
- Chỉ chạy khi máy dừng và người dùng có quyền ENGINEER. Bus về INIT rồi chạy `ecx_config_init`.
- Với mỗi slave, ghi nhận:
  - vị trí trên dây;
  - alias ở thanh ghi 0x0012 (`slavelist[i].aliasadr`, `ec_config.c:209-210`), **chỉ để hiển thị**;
  - Vendor, Product, Revision.
- Tra bảng thiết bị để phân loại: servo, stepper, IO hoặc **Unknown** (thiết bị lạ, không dùng).
- Với R1EC: đọc 0xF050 để biết module ở từng slot, từ đó ra số DI/DO.
- Kết quả hiện trên SETTING › ETHERCAT, **chưa lưu**.

**Nhận diện slave theo vị trí trên dây, không dùng station alias** (người dùng chốt 2026-10-10, thay cho cách dùng alias trước đó)
- Drive thứ n tính từ HMI là **AXn**: drive đầu tiên sau HMI là AX1, drive kế tiếp là AX2, … Coupler IO là RIO1, RIO2, … theo thứ tự trên dây.
- Không cần đặt alias trên drive (L5N P00.23, CL3B 0x2150). Alias có đặt hay không, trùng hay không, đều không ảnh hưởng.
- So sánh với topology đã lưu: slave đã lưu ở vị trí p được so với slave đang ở vị trí p trên bus.
- **Giới hạn:** hai drive **cùng model** đổi chỗ trên dây (hoặc đổi cáp motor/encoder giữa hai drive) thì firmware **không phát hiện được**. AX3 khi đó sẽ điều khiển cơ cấu của AX4, với soft limit và điểm teach của AX3.
  - Khi lắp và bảo trì phải giữ đúng thứ tự dây. Nên dán nhãn số trục lên drive và đầu cáp.
  - Sau khi thay hoặc đi lại dây, chạy thử từng trục ở tốc độ thấp trước khi chạy máy.

**ACCEPT** (quyền ENGINEER, máy dừng)
- Trước khi lưu, hiện danh sách thay đổi so với topology cũ.
- Lưu "topology mong đợi" vào NVS hoặc SD, gồm:
  - mỗi slave: vị trí trên dây, tên trục, Vendor, Product, Revision, loại;
  - rack R1EC: slot, ident;
  - người lưu, thời gian, CRC.
- Sau khi lưu, bus khởi động lại và **mọi trục phải về gốc lại**.

**Mỗi lần khởi động, hoặc khi bấm RE-CHECK:** so bus với topology đã lưu.

| Chênh lệch | Xử lý |
|---|---|
| Khớp hoàn toàn | Vào OP |
| Hai drive cùng model đổi chỗ | **Không phát hiện được** (xem giới hạn ở trên) |
| Hai slave khác model đổi chỗ | Model khác ở cả hai vị trí: giữ PRE-OP |
| Revision khác | Cảnh báo, vẫn vào OP |
| Model khác (Product khác) | Giữ PRE-OP, chặn chuyển động; cần SCAN + ACCEPT |
| Thiếu slave / bus đứt giữa chừng | Giữ PRE-OP. Các slave phía sau dồn lên một vị trí, nên thường báo cả "Wrong model" lẫn "Missing" |
| Thừa slave / thiết bị lạ | Giữ PRE-OP; cần SCAN + ACCEPT (thiết bị lạ không được dùng) |
| Rack R1EC khác (thêm, bớt hoặc đổi chỗ module) | Giữ PRE-OP; cần SCAN + ACCEPT |
| Chưa có topology lưu (lần đầu) | Giữ PRE-OP, mở thẳng màn hình SCAN |

**Dữ liệu gắn theo tên trục, không theo vị trí:** tham số servo, soft limit, homing, điểm teach.
- Trục bị bỏ khỏi topology: dữ liệu vẫn được giữ, chỉ trục đó bị vô hiệu. Lắp lại thì dùng lại được.
- Drive đổi sang model khác: phải kiểm tra lại SERVO SETTING trước khi chạy.

**HMI:** MAIN, MANUAL, SERVO SETTING và IO MONITOR được dựng từ topology đã lưu, với số trục 1..N (N tối đa sẽ chốt, ví dụ 16). Mockup nằm ở artboard "SETTING — EtherCAT".

**Không cắm nóng:** thêm hoặc bớt slave chỉ làm khi máy dừng, sau đó SCAN lại.

**Mất slave khi đang OP** (task `ecat_chk`, `main.c`):
- Slave vẫn giữ station address (chỉ rút cáp, không mất nguồn) được cấu hình lại, nhưng **phải qua kiểm tra danh tính** trước khi lên lại OP:
  - R1EC: hook phải chạy lại và thành công (đúng rack, 0xF030, gán PDO), độ dài SM đúng;
  - slave khác: đọc lại Vendor/Product trong SII EEPROM (`ecx_readeeprom`) và so với topology; hai drive cùng model thì không phân biệt được.
  - Sai thì slave bị giữ ở PRE-OP và log báo lỗi cho tới khi `stop` + `recheck`.
- Slave **mất nguồn** (station address về 0) **không được tự phục hồi**: cần `stop` + `recheck` để quét và so sánh lại đầy đủ.

## 4. Cấu hình esp_eth đã kiểm chứng (ESP-IDF v5.5.x)

Tên field đã đối chiếu với header v5.5.4. Chân và clock theo SCH p1 (hộp alias + U4).

```c
/* board_eth.c — JC1060P470C_I_W_Y. PHY = IP101GR(I) (suy luận), addr 1 do strap R47. */
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_event.h"
#include "esp_check.h"

static esp_eth_handle_t s_eth;

/* Raw L2 RX: esp_eth giao quyền sở hữu buf cho callback -> phải free(). */
static esp_err_t ecat_rx(esp_eth_handle_t h, uint8_t *buf, uint32_t len, void *priv)
{
    if (len >= 16 && buf[12] == 0x88 && buf[13] == 0xA4) {
        /* đẩy {buf,len} vào RX queue của master; bên tiêu thụ free(buf); return ESP_OK nếu đẩy được */
    }
    free(buf);
    return ESP_OK;
}

esp_err_t board_eth_init(void)   /* GỌI TỪ TASK ĐÃ PIN VÀO CORE 1 */
{
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num   = 31;                          /* MDC  -> IP101 pin 22 */
    emac.smi_gpio.mdio_num  = 52;                          /* MDIO -> IP101 pin 23, R41 1.5k */
    emac.interface          = EMAC_DATA_INTERFACE_RMII;
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;   /* PHY tạo 50 MHz từ XTAL 25 MHz (COL/RMII=1, R39) */
    emac.clock_config.rmii.clock_gpio = (emac_rmii_clock_gpio_t)50; /* net RMII_CLK = IP101 pin14 (+pin10) */
    emac.emac_dataif_gpio.rmii.tx_en_num  = 49;            /* IP101 pin 5 */
    emac.emac_dataif_gpio.rmii.txd0_num   = 34;            /* IP101 pin 9  (strap JTAG chỉ khi đốt eFuse) */
    emac.emac_dataif_gpio.rmii.txd1_num   = 35;            /* IP101 pin 8  (cũng là BOOT; R34 10k, SW1) */
    emac.emac_dataif_gpio.rmii.crs_dv_num = 28;            /* IP101 pin 19 */
    emac.emac_dataif_gpio.rmii.rxd0_num   = 29;            /* IP101 pin 18 */
    emac.emac_dataif_gpio.rmii.rxd1_num   = 30;            /* IP101 pin 17 */
    emac.mdc_freq_hz = 0;                                  /* mặc định <= 2.5 MHz; IP101 cần chu kỳ MDC >= 300 ns */

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();   /* mặc định rx_task_prio 15, stack 4096 */
    mac_cfg.rx_task_prio = 20;                             /* > ecat_cycle (19) */
    mac_cfg.flags |= ETH_MAC_FLAG_PIN_TO_CORE;             /* pin emac_rx vào nhân gọi esp_eth_mac_new_esp32() */

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();   /* CHÚ Ý: mặc định reset_gpio_num = 5, addr AUTO */
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = 51;                           /* PHY_RSTN -> IP101 pin 32 */
    phy_cfg.post_hw_reset_delay_ms = 10;                   /* RMII clock sẵn sàng <= 10 ms sau reset (IP101G-DS p59) */

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_FAIL, "eth", "mac/phy create failed");

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);   /* mặc định check_link_period_ms = 2000 */
    cfg.check_link_period_ms = 100;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&cfg, &s_eth), "eth", "install");

    /* KHÔNG esp_netif_new / esp_eth_new_netif_glue / esp_netif_attach trên handle này. */
    ESP_RETURN_ON_ERROR(esp_eth_update_input_path(s_eth, ecat_rx, NULL), "eth", "input path");
    bool off = false;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_S_FLOW_CTRL, &off), "eth", "flow ctrl");
    /* Promiscuous không bắt buộc (EtherCAT trả về dst broadcast); chỉ bật khi bring-up/sniff:
       bool on = true; esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &on); */
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth), "eth", "start");
    /* Ở ETHERNET_EVENT_CONNECTED: yêu cầu ETH_CMD_G_SPEED == ETH_SPEED_100M và
       ETH_CMD_G_DUPLEX_MODE == ETH_DUPLEX_FULL, nếu không thì không vào OP. */
    return ESP_OK;
}
```

**sdkconfig.defaults đề xuất cho phần Ethernet và thời gian thực**

Lấy `IDF_N/lvgl_demo_v9/sdkconfig.defaults` làm nền (PSRAM HEX 200 MHz, flash QIO 16 MB, -O2, HZ 1000), rồi thêm:

```
CONFIG_ETH_USE_ESP32_EMAC=y
CONFIG_ETH_PHY_INTERFACE_RMII=y
CONFIG_ETH_IRAM_OPTIMIZATION=y
CONFIG_ETH_TRANSMIT_MUTEX=y
CONFIG_ETH_DMA_BUFFER_SIZE=512        # bội số 64 trên P4; có thể tăng lên ~1536 để 1 frame/descriptor
CONFIG_ETH_DMA_RX_BUFFER_NUM=20       # không giảm dưới 16
CONFIG_ETH_DMA_TX_BUFFER_NUM=10
CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y
CONFIG_FREERTOS_HZ=1000
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y   # (tên symbol chưa xác minh)
# Chip revision — CHỈ khi esptool báo v1.x (vendor: ETH:941,945):
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

## 5. Lớp port SOEM (đã viết, build sạch trên IDF 5.5.4, **chưa chạy trên board**)

Mã nguồn: `firmware/components/soem/` (chi tiết trong `SOEM_UPSTREAM.md`).

- **SOEM upstream:** commit `88e8ed4` (v2.0.0 + 13 commit sửa lỗi), chép nguyên vẹn vào `SOEM/`, không sửa.
- **`port/osal.c`:** thời gian từ `esp_timer` (1 µs); mutex FreeRTOS; `osal_thread_create_rt` pin vào `CONFIG_SOEM_RT_CORE`.
  `osal_monotonic_sleep()` ngủ bằng `vTaskDelay` cho phần nguyên tick, sau đó **alarm GPTimer** (ISR ở core 1)
  đánh thức task, 30 µs cuối mới spin. Như vậy core 1 không bị busy-wait cả chu kỳ và IDLE1 vẫn chạy.
- **`port/nicdrv.c`:** lấy từ port rt-kernel, chỉ một cổng. Khác bản gốc: tạo mutex một lần (bản rtk tạo hai lần);
  TX lỗi thì trả buffer index; **khi chờ frame thì block trên queue RX** (`oshw_mac_wait_rx`) thay vì spin.
- **`port/oshw.c`:** callback `esp_eth_update_input_path` lọc EtherType 0x88A4 → queue (độ dài `CONFIG_SOEM_RX_QUEUE_LEN`),
  frame khác thì `free()`. TX bằng `esp_eth_transmit` (bật `CONFIG_ETH_TRANSMIT_MUTEX`). Có bộ đếm TX/RX/drop.
- **`port/include/soem/ec_options.h`:** thay header sinh bởi CMake; kích thước lấy từ Kconfig (mặc định 32 slave, 16 buffer, 16 mailbox).
- **API:** `soem_esp_attach(eth)` gọi từ task ở core 1, trước `ecx_init()`.
- **Ưu tiên:** task `emac_rx` = `SOEM_RT_TASK_PRIO + 1` (20) > task `ecat` (19) trên core 1, để frame luôn được nhận.

Ứng dụng thử `firmware/main/main.c` (theo `samples/simple_ng`):
- quét slave, in danh sách; map IO; `ecx_configdc`; SAFE_OP → OP;
- vòng cyclic theo deadline tuyệt đối (`CONFIG_APP_ECAT_CYCLE_US`, mặc định 1000 µs);
- task `ecat_chk` (core 0) giám sát slave như `ecatcheck`, có kiểm tra lại danh tính và không tự phục hồi slave mất nguồn (mục 3.6);
- task `ecat_mon` (core 0) in mỗi giây: WKC, RTT min/avg/max, độ trễ thức dậy lớn nhất, số lần trễ chu kỳ, số frame bị drop.

**Lớp ứng dụng EtherCAT** (`firmware/components/ecat/`, 2026-10-03; build sạch, chưa chạy trên board):
- **Bảng thiết bị:** sinh từ ESI bằng `firmware/tools/esi2catalog.py` ra `ecat_catalog_gen.c`. Hiện có 6 thiết bị Leadshine và 20 module R1EC.
- **Topology Scan → Xác nhận → Lưu** (mục 3.6), lưu trong NVS (namespace `ecat`, có CRC).
- **Cấu hình R1EC** trong hook PO2SO (`ecat_coupler.c`), kích thước lấy từ ESI qua `soem_esp_set_pdo_layout()`. Đây là chỗ duy nhất phải vá SOEM; xem `SOEM_UPSTREAM.md`.
- **Ứng dụng thử** (`main.c`):
  - quét bus, so sánh với topology đã lưu;
  - khớp thì vào OP, không khớp thì giữ ở PRE-OP và chờ lệnh console;
  - lúc ở OP, mỗi giây in trạng thái và byte IO của coupler.
**Điều khiển trục CiA402** (2026-10-10; build sạch, self-test với drive mô phỏng đạt, **chưa chạy với drive thật**):
- **`ecat_drive.c`**: trong hook PO2SO, remap PDO của mọi drive (L5N/L3N/CL3B) về một bố cục (`SLAVES_LEADSHINE.md` mục 6), rồi đọc 0x608F/0x6091/0x6092/0x6098.
- **`ecat_axis.c`**: máy trạng thái CiA402 cho từng trục, chạy trong vòng chu kỳ trên core 1.
  - Chế độ: PP (move, step jog), PV (jog liên tục có dead-man 300 ms) và HM (phương pháp trong drive).
  - Lệnh vào qua queue FreeRTOS; trạng thái ra qua snapshot seqlock (`ecat_axes_read()`).
  - Fail safe: drive lỗi, WKC sai 3 chu kỳ liên tiếp, hoặc drive tự rời Operation enabled thì trục tắt servo và không tự bật lại.
  - Trước khi rời OP, mọi trục dừng và tắt servo trong tối đa 2 s.
  - Trục sắp theo thứ tự trên dây; tên lấy từ topology đã lưu.
- **`ecat_busview.c`**: snapshot topology (đã lưu, lần scan gần nhất, issue, so sánh) cho trang Settings › EtherCAT.
- HMI: **Settings › EtherCAT** (Scan → Accept → Save trên màn hình) và **Manual › Jog**. Console: `axis`.
- Các bước thử ngày đầu với drive thật: `SETUP.md` mục 7c.

**IO, SDO cho HMI và alarm** (2026-10-10; build sạch, mở được trên board không có slave, **chưa chạy với R1EC và drive thật**):
- **`ecat_io.c`**: ảnh IO của các coupler cho HMI.
  - Bind sau khi lên OP. Mỗi chu kỳ copy input/output (tối đa 128 byte mỗi chiều, mỗi coupler) vào snapshot seqlock.
  - Force output: lệnh từ HMI qua queue, áp vào IOmap trong vòng chu kỳ. Tắt force thì các bit đã force về 0.
  - Bố cục (slot, kênh, địa chỉ X/Y) nằm trong PSRAM, chỉ ghi lúc bind/unbind.
- **`ecat_sdo_service.c`**: worker trên core 0 chạy các job SDO của Settings › Servo (đọc/ghi một danh sách object).
  - Worker giữ `s_bus_lock` trong lúc chạy job.
  - Job ghi slave theo vị trí kèm vendor/product; nếu slave ở vị trí đó là thiết bị khác thì job bị từ chối.
  - **Thay đổi khoá:** vòng giữ PRE-OP (`hold_loop`) nhả `s_bus_lock` trong lúc chờ lệnh, để job SDO chạy được cả khi bus chưa vào OP.
- **`main/app_alarms.c`**: task 200 ms trên core 0, chuyển trạng thái bus, trục, IO và đồng hồ thành alarm (`components/machine/machine_alarm.c`). Bảng mã ở `SETUP.md` mục 7d.

Chưa có: DC sync0 + vòng PI (`ec_sync`), soft limit do firmware tự kiểm tra (hiện chỉ có 0x607D trong drive), đo bằng GPIO/scope.

## 6. Kế hoạch bring-up theo phase

| Phase | Nội dung | Tiêu chí nghiệm thu |
|---|---|---|
| **0. Toolchain + baseline** | Cài IDF v5.5.4 (SETUP.md). Sao lưu toàn bộ flash 16 MB. Đọc chip revision. Build và nạp demo `ethernet` và `lvgl_demo_v9` (bản copy, xem SETUP.md). | Có file backup 16 MB. Revision đã ghi lại. Demo Ethernet báo link up và nhận IP DHCP. Màn hình hiển thị đúng màu và cảm ứng đúng toạ độ, từ đó **chốt được New hay Old**. |
| **1. Khung firmware + `board`** | Tạo project `firmware/` và component `board`: DSI/JD9165 theo đúng biến thể, GT911 có điều khiển RST/INT, đèn nền, `board_pins.h`. LVGL pin core 0, 1 draw unit. Ghim phiên bản component và commit `dependencies.lock`. | Build sạch từ đầu. UI 1024x600 đạt ≥ 30 fps (LVGL sysmon). Cảm ứng chính xác. Chạy 1 giờ không WDT hay reset. Log có heap nội và PSRAM. |
| **2. EMAC raw L2** | `board_eth_init()` như mục 4, chạy từ task core 1, không netif. Đọc PHY ID. Cắm 1 slave EtherCAT, hoặc PC chạy Wireshark. | PHY ID = 0x0243/0x0C54. Link 100/Full. Phát hiện cắm/rút ≤ 200 ms. Callback đếm đúng số frame. Wireshark **không thấy** DHCP/ARP/IPv6 nào từ board. |
| **3. Loopback BRD + đo thời gian** | Tự dựng frame BRD đọc ESC reg 0x0000 (EtherType 0x88A4, dst broadcast). Gửi 1 kHz bằng GPTimer trên core 1. Đo RTT bằng cycle counter và xung GPIO32/33 trên scope. Chạy kèm LVGL benchmark + Wi-Fi + ghi SD. | WKC = số slave. 0 frame mất trong ≥ 1 giờ. Có histogram RTT/jitter (p99.9, max). **Chốt chu kỳ mục tiêu** (1 ms / 500 µs). |
| **4. Port SOEM + quét slave** | Thêm SOEM v2 và `osal`/`oshw` cho esp-idf. Chạy `ecx_init` → `ecx_config_init` → tương đương slaveinfo → `ecx_config_map_group` → SAFE-OP. Đọc/ghi SDO. | Tìm đủ slave. Chuyển INIT→PRE-OP→SAFE-OP. SDO 0x1018 (identity) khớp ESI. PDO mapping khớp ESI/ENI (`eniconv.py`). |
| **5. PDO tuần hoàn (OP)** | `ecx_send_processdata`/`ecx_receive_processdata` ở 1 ms. Giám sát WKC và tự phục hồi. Stress: LVGL, Wi-Fi, SD. **Không ghi NVS khi đang OP.** | 24 giờ không lỗi WKC ở 1 ms khi có tải HMI. Rút/cắm cáp (slave không mất nguồn) thì tự phục hồi về OP. Tắt/bật nguồn một slave thì nó bị giữ lại, cần `stop` + `recheck`. Sau đó thử 500 µs. |
| **6. Distributed Clocks** | `ecx_configdc`, `ecx_dcsync0`, PI loop `ec_sync` như `ec_sample`. Drive chạy CSP (nếu dùng CiA402). | Scope thấy SYNC0 ổn định. 24 giờ không lỗi sync hay SM watchdog. Drive chạy CSP êm. |
| **7. Tích hợp HMI** | Snapshot seqlock + queue lệnh. Trang chẩn đoán: trạng thái slave, WKC, histogram latency. Trạng thái an toàn khi mất link. | Lệnh từ UI tới output trong ≤ 2 chu kỳ + chu kỳ UI. Mất link thì output về safe state, UI báo lỗi trong ≤ 3 chu kỳ. |
| **8. Hoàn thiện** | Wi-Fi từ xa (tùy chọn). Workaround esp-idf #19085: dừng EMAC/DMA trước `esp_restart`. Kiểm tra `CONFIG_SPI_FLASH_AUTO_SUSPEND`. Chốt license. | 100 lần soft-restart liên tiếp không hỏng heap. Đo lại jitter khi có Wi-Fi. Đã có quyết định license bằng văn bản. |

## 7. Rủi ro và giảm thiểu

| Rủi ro | Mức | Giảm thiểu |
|---|---|---|
| License SOEM v2 là GPLv3 trong khi sản phẩm đóng mã | Cao | Quyết định sớm: mua license RT-Labs, mở mã GPLv3, hoặc chuyển CherryECAT. Giữ lớp trừu tượng mỏng để đổi được stack. |
| Chưa có số đo latency/jitter nào cho P4; RX của `esp_eth` có malloc + memcpy + task hop | Cao | Phase 3 đo trước khi cam kết chu kỳ. Nếu thiếu: driver zero-copy dưới `esp_eth` (API nội bộ, không hỗ trợ) hoặc phương án B Hilscher. |
| Ghi flash/NVS làm tắt cache, dừng cả hai nhân | Cao | Không ghi NVS/flash khi OP. Code real-time và ISR trong IRAM. Xem xét `SPI_FLASH_AUTO_SUSPEND`. |
| Tranh chấp bus PSRAM / DSI / LVGL, chưa rõ mức độ | Trung bình | IOmap và buffer ở RAM nội. Đo dưới tải. Giảm tốc độ refresh LVGL nếu cần. |
| Chip v1.x hay v3.x (firmware không dùng chung) | Trung bình | Đọc bằng esptool ở Phase 0 rồi đặt `SELECTS_REV_LESS_V3` cho đúng. |
| Mã PHY chưa in trên sơ đồ | Thấp | Đọc PHY ID và chữ in trên chip. |
| REF_CLK kéo 2 tải, không buffer hay điện trở nối tiếp | Trung bình | Theo dõi bộ đếm CRC/symbol error của IP101 (page 1 reg 18, page 11 reg 18). Đo dạng sóng nếu có lỗi. |
| GPIO35 = TXD1 = nút BOOT | Trung bình | Dán nhãn hoặc che nút BOOT trên sản phẩm. Tài liệu vận hành ghi rõ không bấm. |
| PHY_RSTN không có RC; mặc định IDF reset GPIO5 | Trung bình | Luôn đặt `reset_gpio_num = 51`. Nếu GPIO51 bị giữ thấp trước khi install thì EMAC reset timeout. |
| esp-idf #19085: EMAC DMA tiếp tục chạy sau `esp_restart` → hỏng heap | Trung bình | Dừng EMAC và DMA trước khi restart. Kiểm tra bản IDF nào đã sửa. |
| Một cổng duy nhất, không redundancy | Cố định | Chấp nhận. IP đi qua Wi-Fi hoặc EoE. |
| lwIP vô tình gắn vào EMAC | Trung bình | Review code. Assert không có netif ETH. Kiểm tra bằng Wireshark (Phase 2). |
| Busy-poll trong SOEM làm đói IDLE1 (WDT) | Trung bình | Chờ bằng queue có timeout. Cấu hình WDT. |
| Không có port P4 nào để tham khảo | Trung bình | Lấy port rtk làm mẫu. Viết test nhỏ theo từng phase. |
| RS485 (nếu dùng cho thiết bị phụ): RO 5 V vào chân 3.3 V, không có 120 Ω, khoảng mù 0.2–0.5 ms | Thấp/TB | Đo mức điện áp. Lắp trở cuối ngoài. Cấu hình timeout giao thức. |

## 8. Cấu trúc repository đề xuất

**Hiện trạng (2026-10-03):** đã có `firmware/components/soem` (stack + port, thay cho `ethercat/third_party` + `port/` bên dưới),
`firmware/components/board` (`board_pins.h`, `board_eth.c`) và `firmware/main/main.c` (ứng dụng thử).
Logic master (`ecat_master.c`: snapshot, queue lệnh) sẽ tách khỏi `main.c` thành component riêng khi bắt đầu HMI.

```
EthercatHMI/                     (ownCloud; chỉ chứa source và docs)
├─ CLAUDE.md
├─ docs/  BOARD.md  ETHERCAT_PLAN.md  SETUP.md  SLAVES_LEADSHINE.md  REFERENCE_PGM.md
├─ vendor/JC1060/                (READ-ONLY)
├─ vendor/ESI/                   (READ-ONLY; ESI Leadshine + Fastech)
├─ PGM/                          (READ-ONLY; project C# tham khảo)
└─ firmware/                     (ESP-IDF project; build bằng -B ra đường dẫn ngắn ngoài ownCloud)
   ├─ CMakeLists.txt
   ├─ sdkconfig.defaults         (không commit sdkconfig sinh tự động)
   ├─ partitions.csv
   ├─ dependencies.lock          (COMMIT – ghim phiên bản lvgl, esp_lcd_jd9165, esp_lvgl_port, gt911)
   ├─ main/  app_main.c  idf_component.yml
   └─ components/
      ├─ board/      board_pins.h, board_display.c, panel_jd9165_new.c / panel_jd9165_old.c
      │              (chọn bằng Kconfig BOARD_PANEL_NEW/OLD), board_touch.c (GT911 có RST/INT),
      │              board_backlight.c, board_eth.c, board_power.c (ADC pin GPIO53), Kconfig
      ├─ ethercat/   third_party/soem (vendored, có ghi phiên bản),
      │              port/osal_esp_idf.c, port/oshw_esp_idf.c (nicdrv từ rtk),
      │              ecat_master.c/.h (task core 1, GPTimer, seqlock snapshot, queue lệnh), Kconfig
      ├─ hmi/        ui_*.c (LVGL 9), screens, fonts tiếng Việt, ui_bridge.c (đọc snapshot, gửi lệnh)
      └─ app/        ánh xạ process image ↔ tag ứng dụng, state machine máy, alarm, recipe (SD)
```

- Không dùng nguyên BSP EV board của vendor, vì nó kéo theo `esp_video`, `usb`, `esp_codec_dev`, `ek79007`, `ili9881c`, `lt8912b`.
- Component `board` chỉ lấy phần cần từ BSP_N.c:325-673 (hoặc BSP_O.c) làm tài liệu tham khảo.

## 9. Câu hỏi mở về phần EtherCAT

**Đã trả lời (2026-10-03, giai đoạn lập kế hoạch):**
- Máy thật có **khoảng 8 trục Leadshine** (EtherCAT, CiA402) và **remote IO Leadshine R1EC**.
- Đã có ESI trong `vendor/ESI/` (chỉ đọc): L5N, L3N, CL3B-EC, R1EC và Fastech.
- Đã có project tham khảo `PGM/` (C#, máy 6× L5N-400S + 2× CL3B-EC507 + R1EC).
  - **Máy đích không phải máy PGM.** PGM chỉ dùng để học cách kết nối bus và cấu hình servo, remote IO.
- Chi tiết ở `docs/SLAVES_LEADSHINE.md` và `docs/REFERENCE_PGM.md`.
- Phần cứng thật sẽ được bổ sung sau; hiện chỉ lập kế hoạch.
- Danh sách slave được tạo động bằng **Scan → Xác nhận → Lưu**, nhận trục theo vị trí trên dây (mục 3.6).

**Hệ quả cho thiết kế (từ ESI và PGM; chưa thử trên phần cứng):**
- Khoảng 9–10 slave. Với cấu hình kiểu PGM, process image khoảng **68 byte out / 176 byte in** (PDO mặc định). Gọn trong một frame LRW; `CONFIG_SOEM_MAXSLAVE = 32` là dư.
- **Drive:** ESI mặc định đặt CSP (`0x6060 = 8`) và có SYNC0 (0x0300). Drive cũng chấp nhận Free Run.
  - Đề xuất: giai đoạn đầu dùng **PP + HM** với 0x1600 được remap (như PGM, nhưng đổi mode và tốc độ qua PDO).
  - Phase 6 (DC + CSP) chỉ bắt buộc khi có trục cần nội suy.
  - Xem `SLAVES_LEADSHINE.md` mục 6.
- **R1EC** cần hook `PO2SOconfig`: ghi 0xF030 và 0x1C12/0x1C13 theo module.
  - Thiếu hook thì SOEM chỉ map 4 byte Device Status, đúng như PGM đã gặp khi chạy thật.
  - Coupler không trả mapping qua SDO, nên bề rộng module lấy từ ESI. Việc này cần fork nhỏ `ec_config.c` trong `port/`; PGM cũng đã vá SOEM tương tự.
  - Đây là việc phải làm ở **phase 4**, trước khi thấy được IO. Xem `SLAVES_LEADSHINE.md` mục 4.4.
- Drive không có SDO Info và không có Complete Access. HMI không duyệt được object dictionary, nên danh sách tham số SERVO SETTING phải biên dịch sẵn từ ESI.

**Còn mở:**
1. Model drive, số trục, rack module R1EC và thứ tự slave của máy đích (chốt khi có phần cứng).
2. Chu kỳ yêu cầu (1 ms?). Có trục nào cần nội suy cùng nhau (nhóm trục cho TEACHING)?
3. Cần FoE (cập nhật firmware slave) hoặc EoE không? Câu trả lời ảnh hưởng tới lựa chọn CherryECAT.
4. Quyết định license (xem mục 2).
5. Có thiết bị đo không: oscilloscope hoặc logic analyzer, máy chạy Wireshark kèm tap, bộ slave thử (ví dụ EK1100 + EL)?
