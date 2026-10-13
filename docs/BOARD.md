# BOARD.md — Guition JC1060P470C_I_W_Y (ESP32-P4 + ESP32-C6, 7" 1024x600)

Tài liệu phần cứng tham chiếu cho dự án **HMI + EtherCAT master**. Mỗi thông tin đều ghi nguồn. Thông tin chưa kiểm chứng trên board thật được đánh dấu **(chưa xác minh)**.

## 0. Quy ước trích dẫn

Mọi đường dẫn đều tương đối so với thư mục gốc dự án `C:\Users\Tuan\ownCloud\Tuan\EthercatHMI`. Gói vendor nằm ở `vendor/JC1060/` và **chỉ được đọc, không được sửa**.

| Viết tắt | File |
|---|---|
| **SCH** | `vendor/JC1060/5-Schematic/JC1060P470C_I_W_Y-V1.0.pdf` p.1 (một tờ A1, ngày 9/06/2025) |
| **SPEC** | `vendor/JC1060/2-Specification/JC1060P470C_I_W_Y Specifications-EN.pdf` |
| **GS** | `vendor/JC1060/6-User_Manual/Getting started JC1060P470C_I_W_Y.pdf` |
| **DS** | `vendor/JC1060/4-Driver_IC_Data_Sheet/ESP32_P4_Chip_Datasheet_CN_v0.4.1_0320 .pdf` (có dấu cách trước `.pdf`) |
| **PANEL** | `vendor/JC1060/4-Driver_IC_Data_Sheet/JC1060M070N_I.pdf` |
| **DTSI** | `vendor/JC1060/4-Driver_IC_Data_Sheet/MTK_JD9165BA_HKC7.0_IPS(QD070AS01-1)_1024x600_MIPI_1+2Dot_G2.2_20240729_andy_2lane.dtsi.txt` |
| **IDF_N** / **IDF_O** | `vendor/JC1060/1-Demo/Demo_IDF/ESP-IDF_5.5.4/JC1060P470C_I_W_Y_New_Panel` / `..._Old_Panel` |
| **BSP-H** | `IDF_N/common_components/espressif__esp32_p4_function_ev_board/include/bsp/esp32_p4_function_ev_board.h`. Bản Old giống hệt từng byte (MD5 0CAC7DA9…). |
| **BSP_N.c** | `IDF_N/common_components/espressif__esp32_p4_function_ev_board/esp32_p4_function_ev_board.c` |
| **BSP_O.c** | `IDF_O/common_components/esp32_p4_function_ev_board/esp32_p4_function_ev_board.c`. Tên thư mục không có tiền tố `espressif__`. |
| **XZ** | `IDF_N/xiaozhi-esp32-main/main/boards/guition-jc1060p470-new/config.h` |
| **ARD** | `vendor/JC1060/1-Demo/Demo_Arduino/JC1060P470C_I_W_Y_New_Panel/<demo>/pins_config.h` |
| **ETH** | `IDF_N/ethernet/sdkconfig` |
| **HOST** | `IDF_N/esp_brookesia_phone/sdkconfig` |
| **BURN** | `vendor/JC1060/8-Burn operation/烧录说明：Burning instructions.txt` |
| **IP101G-DS** | Datasheet IC Plus IP101G-DS-R01. Tài liệu web, **không có trong gói vendor**. |

## 1. Tổng quan và thông số

| Hạng mục | Giá trị | Nguồn |
|---|---|---|
| Module chính | U3 **JC-ESP32P4-M3** (Guition), 80 chân, chứa cả ESP32-P4 và ESP32-C6. Flash và PSRAM nằm trong module. | SCH U3 |
| SoC | ESP32-P4, RISC-V 2 nhân, tối đa 400 MHz. Mọi demo vendor chạy **360 MHz**. Có 768 KB HP L2MEM, 32 KB LP SRAM, 128 KB HP ROM. | SPEC p3; ETH:1151-1152 |
| Mã chip trên nhãn ảnh | "ESP32P4NRW32", ảnh độ phân giải thấp nên chỉ tin cậy trung bình | SPEC p5 |
| Chip revision | Mọi sdkconfig và mọi file .bin của vendor đều nhắm **v1.0–v1.99** (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`). Board của dự án là **v1.3**, flash 16 MB, MAC e8:f6:0a:e7:3a:3e (đã xác minh: esptool `flash_id` ngày 2026-10-10), nên dùng đúng nhánh v1.x. | ETH:941,945; header các .bin trong `8-Burn operation/Burn files`; esptool 2026-10-10 |
| Flash | 16 MB, QIO 80 MHz. Log nạp ghi JEDEC mfr 0x68, dev 0x4018. | SPEC p3; `8-Burn operation/flash_download_tool_3.9.9/flash_download_tool/logs/30EDA0E1A32A.txt` |
| PSRAM | 32 MB. Demo LVGL chạy HEX mode 200 MHz. | SPEC p3; `IDF_N/lvgl_demo_v9/sdkconfig:1459-1484` |
| Wi-Fi/BT | ESP32-P4 **không có** Wi-Fi/BT. Wi-Fi/BT do ESP32-C6 cung cấp qua ESP-Hosted (SDIO). SPEC p3 ghi "tích hợp WiFi" là không chính xác. | GS p2; HOST:2829-2896 |
| Màn hình | 7" TFT IPS 1024x600, 24-bit màu, IC JD9165(BA), MIPI-DSI **2 lane** | SPEC p3-4; PANEL p2; DTSI L23 |
| Cảm ứng | Điện dung, GT911, I2C địa chỉ 0x5D | `Demo_Arduino/.../src/touch/esp_lcd_touch_gt911.h:38`; BSP_N.c:666 |
| Ethernet | 100 Mbit/s, một cổng RJ45. PHY U4 là IP101GR/GRI, **suy luận** vì khớp 33/33 chân. | SPEC p5 mục 13; SCH U4 |
| RS-485 | J4 (MX1.25 4P), MAX485 tự đổi chiều | SPEC p5 mục 11; SCH khối MAX485 |
| UART | J5 "UART1" dùng chung GPIO26/27 với RS485. CN2 "UART" là UART0 console. | SPEC p5 mục 8, 10; SCH |
| I2C | CN3 SH1.0 4P | SPEC p5 mục 12 |
| Âm thanh | Codec ES8311, ampli NS4150, mic MEMS analog. Loa ngoài cắm CN1 (MX1.25 2P). | SCH; SPEC p5 mục 3 |
| Thẻ nhớ | Khe TF, SDMMC 4-bit. Thẻ < 32 GB, FAT32. | SPEC p5; `vendor/JC1060/1-Demo/Instructions.txt` |
| Camera | FPC2 15P, MIPI-CSI 2 lane. Module OV02C10 **có lắp**: chip ID 0x5602 đọc được ở I2C 0x36 (2026-10-13). SPEC không liệt kê camera. | SCH CSI_Camera; `docs/CAMERA.md` |
| USB | USB2 "Full Speed" Type-C dùng để nạp (USB-Serial-JTAG). USB3 "High Speed" Type-C là HS OTG. | SCH; SPEC p5 mục 7, 9 |
| RTC | RX8025T-UC, pin CR1220. Chân INT# không nối vào GPIO nào. Trả lời ở địa chỉ I2C **0x32** trên I2C_NUM_1 (đã kiểm tra 2026-10-10, `board_rtc.c`). Lần khởi động đầu cờ VLF đang bật (RTC đã mất giờ). Sau khi đặt giờ, giờ còn đúng qua một lần reset; **chưa thử tắt nguồn**, nên chưa biết pin CR1220 có lắp hay còn tốt không. | SCH RTC; bản đồ thanh ghi theo tài liệu Epson RX-8025T (datasheet không có trong gói của hãng) |
| Pin Li-ion | IP5306, cổng CN4, nút SW3 | SCH Battery |
| Nút bấm | BOOT (GPIO35), RESET (CHIP_PU), nút pin | SCH; SPEC p5 |
| Header mở rộng | JP1 2x13, bước 2.54 mm | SPEC p5 mục 18; SCH Expand IO |
| Nguồn | 5 V, khoảng 600 mA. Không nhận dải rộng: nguồn 24 V cần bộ hạ áp 5 V bên ngoài. | SPEC p4; BURN L1 |
| Nhiệt độ | Hoạt động −20…70 °C, lưu kho −30…80 °C | SPEC p4 |
| Cơ khí | Kích thước 181.80 x 113.72 mm. 4 lỗ Ø2.00, khoảng cách tâm 166.00 x 99.00 mm. Dày 20.38 mm. Nặng khoảng 300 g. | `vendor/JC1060/3-Structure_Diagram/JC1060P470C_I_W_Y.pdf`; SPEC p4, p6 |

## 2. Bảng GPIO tổng hợp (đã đối chiếu sơ đồ, code và datasheet)

Bảng này lấy từ kết quả của bước kiểm chứng: sơ đồ nguyên lý được ưu tiên hơn code demo.

- Gán chân **giống nhau ở cả New và Old panel**.
- Miền nguồn (power domain) lấy theo DS phụ lục A, p.81-83.
- Mỗi số chân U3 dẫn tới chân module.

| GPIO | Chức năng | Theo sơ đồ (SCH) | Theo code vendor | Kết luận | Ghi chú |
|---|---|---|---|---|---|
| 0 | LCD_RST (JD9165) | U3-76 → FPC1-4 "LCD_RST" | BSP-H:91 = 0; ARD `lvgl_demo_v8/pins_config.h:6` = 5; `video_lcd_display/main/app_lcd.c:539` = 5; XZ:36 = 27 | Xung đột, chọn **GPIO0** | Miền VDD_LP/VDD_BAT; carrier không có pull |
| 1 | Trống | U3-4 → JP1-7 | — | **Trống** | LP GPIO, XTAL_32K_P (không lắp thạch anh 32k) |
| 2 | Trống | U3-5 → JP1-9 | — | **Trống** | Mặc định MTCK; TOUCH_CH0; pull-up yếu sau reset |
| 3 | Trống | U3-6 → JP1-11 | — | **Trống** | MTDI; TOUCH_CH1 |
| 4 | Trống | U3-8 → JP1-13 | — | **Trống** | MTMS; TOUCH_CH2 |
| 5 | Trống | U3-9 → JP1-15 (không đi đâu khác) | ARD và video_lcd_display dùng làm "LCD_RST" — **sai** | **Trống** | MTDO; TOUCH_CH3 |
| 6 | Net "C6_IO2" | U3-10, không lên header | không dùng | Để dành | Có thể nối IO2 của C6 bên trong module (chưa xác minh) |
| 7 | I2C SDA | U3-11 → ES8311, RTC, FPC3-5, FPC2-14, CN3-4, JP1-23 | BSP-H:70; XZ:16; ARD:9 | Xác nhận | I2C_NUM_1, 400 kHz trong BSP |
| 8 | I2C SCL | U3-12 → ES8311 CCLK (qua R82 100R), RTC, FPC3-4, FPC2-13, CN3-3, JP1-25 | BSP-H:69 | Xác nhận | |
| 9 | I2S DSDIN (P4 → codec) | U3-13 | BSP-H:80 | Xác nhận | |
| 10 | I2S LRCK | U3-14 | BSP-H:79 | Xác nhận | |
| 11 | PA_CTRL (bật NS4150) | U3-15, R19 kéo xuống | BSP-H:82 | Xác nhận | |
| 12 | I2S SCLK | U3-19 | BSP-H:77 | Xác nhận | |
| 13 | I2S MCLK | U3-20 | BSP-H:78 | Xác nhận | |
| 14–17 | SDIO D0–D3 → C6 | Nội bộ module | HOST:2878/2882/2880/2881 | Chỉ có trong code | **Không dùng được** |
| 18 | SDIO CLK → C6 | Nội bộ | HOST:2877 | Chỉ có trong code | |
| 19 | SDIO CMD → C6 | Nội bộ | HOST:2876; BSP-H:130 `BSP_USB_NEG` (define thừa) | Chỉ có trong code | |
| 20 | Trống | U3-21 → JP1-17 | BSP-H:129 `BSP_USB_POS` (define thừa, không dùng) | **Trống** | VDD_IO_0. ADC1_CH4 — chân header **duy nhất có ADC** |
| 21 | TOUCH_INT | U3-22 → FPC3-2 | BSP NC; XZ:49 = 21; XZ:22 BOOT = 21 (sai) | Đã nối | BSP để không dùng |
| 22 | TOUCH_RST | U3-23 → FPC3-3 | BSP NC; XZ:48 = 22 | Đã nối | Nên điều khiển để địa chỉ GT911 cố định |
| 23 | LCD_PWM (đèn nền) | U3-24 → R69 0R → MP3202 EN; R72 kéo xuống | BSP-H:90 | Xác nhận | 20 kHz (BSP_N.c:341) / 5 kHz (BSP_O.c:446) |
| 24 / 25 | USB-Serial-JTAG D− / D+ | U3-42/43 USB1_P1_N/P → USB2 Type-C | — | Xác nhận (ánh xạ suy luận; DS p.23, p.82) | **Cổng nạp** |
| 26 | UART1 TX → MAX485 DI, J5-2 | U3-45 "GPIO26 TX1" | `IDF_N/uart_echo_rs485/main/rs485_example.c:28`; XZ:21 dùng làm LED (sai) | RS485 TX | Không định tuyến FS-OTG sang chân này |
| 27 | UART1 RX ← MAX485 RO, J5-3 | U3-46 "GPIO27 RX1" | `rs485_example.c:29`; XZ:36 dùng làm LCD_RST (sai) | RS485 RX | RO cấp nguồn 5 V đi thẳng vào chân 3.3 V |
| 28 | RMII CRS_DV | U3-47 → PHY chân 19 | Giá trị mặc định của IDF | Xác nhận | |
| 29 | RMII RXD0 | U3-48 → PHY 18 | Mặc định IDF | Xác nhận | |
| 30 | RMII RXD1 | U3-49 → PHY 17 | Mặc định IDF | Xác nhận | |
| 31 | MDC | U3-50 → PHY 22 | ETH:789 | Xác nhận | |
| 32 | Trống | U3-51 → JP1-19 | — | **Trống** | VDD_IO_4 (3.3 V) |
| 33 | Trống | U3-52 → JP1-21 | — | **Trống** | VDD_IO_4 |
| 34 | RMII TXD0 | U3-53 → PHY 9 | Mặc định IDF | Xác nhận | Chân strap (chọn JTAG, chỉ khi đốt eFuse; DS p.35) |
| 35 | RMII TXD1 **và** BOOT | U3-54 → PHY 8; SW1 xuống GND; R34 10k kéo lên | Mặc định IDF | Xác nhận | Strap: 1 = SPI boot (DS p.33). **Bấm BOOT khi đang chạy sẽ làm hỏng TX Ethernet.** |
| 36 | Chỉ là strap | U3-55, R44 10k kéo lên | — | Không dùng | Không ra header |
| 37 | UART0 TX (console) | U3-56 → R33 → CN2-2 | Console mặc định | Xác nhận | Strap, không ảnh hưởng chế độ boot |
| 38 | UART0 RX | U3-57 → R35 → CN2-3 | Console mặc định | Xác nhận | |
| 39–44 | SD D0, D1, D2, D3, CLK, CMD | U3-59…64 → J1 | BSP-H:115-120 | Xác nhận | VDD_IO_5 |
| 45 | Trống | U3-65 → JP1-14. R10 (cổng nguồn TF) để NC | — | **Trống** (đo thông mạch trước khi dùng) | Lỗi vẽ ở JP1-14/16/18 |
| 46 | Trống | U3-66 → JP1-12 | — | **Trống** | VDD_IO_5 |
| 47 | Trống | U3-67 → JP1-10 | — | **Trống** | VDD_IO_5 |
| 48 | I2S DIN (net "ES7210_SDOUT") | U3-68 ← ES8311 ASDOUT | BSP-H:81 | Xác nhận | Không có ES7210 trên board |
| 49 | RMII TX_EN | U3-69 → PHY 5 | Mặc định IDF | Xác nhận | VDD_IO_6 |
| 50 | RMII REF_CLK **vào** | U3-70 ← PHY 50M_CLKO (chân 14), nối vòng về 50M_CLKI (chân 10) | Mặc định IDF `EMAC_CLK_EXT_IN` | Xác nhận | |
| 51 | PHY_RSTN | U3-71 → PHY 32; không có điện trở kéo | ETH:791 | Xác nhận | PHY có pull-up nội 222 kΩ (IP101G-DS p12) |
| 52 | MDIO | U3-72 → PHY 23; R41 1.5k kéo lên | ETH:790 | Xác nhận | |
| 53 | Đo áp pin (ADC2_CH4) | U3-73; BAT+ → R52 68k / R57 100k → R55 0R; C61 10 nF | `Demo_Arduino/.../adc_test/adc_test.ino:8,35` | Xác nhận (DS p.23, p.82) | Tỉ lệ chia 0.595 |
| 54 | C6_CHIP_PU | U3-74; R81 10k kéo lên; C64 1 µF; JP1-26 | HOST:2875 (reset slave ESP-Hosted) | **Dành riêng** | Không tự điều khiển JP1-26 |

**Chân chuyên dụng của U3**

| Chân U3 | Chức năng |
|---|---|
| 26–31 | DSI → FPC1 |
| 33–38 | CSI → FPC2 |
| 39/40 | USB HS → USB3 |
| 42/43 | USB FS → USB2 |
| 58 | ESP_LDO_VO4 → TF_VCC |
| 75 | CHIP_PU: R37 10k, C38 1 µF, SW2 RESET |
| 77/78 | ESP_3V3 |
| 2 | RF của C6 → ANT1 |

## 3. Từng khối chức năng

### 3.1 Ethernet (khối quan trọng nhất)

**PHY U4**
- Sơ đồ **không in mã linh kiện**.
- Sơ đồ chân 32 chân + EP khớp 33/33 với IP101GR/GRI (IP101G-DS p16-18). Demo vendor cũng chọn `CONFIG_EXAMPLE_ETH_PHY_IP101=y` (ETH:784).
- Cần kiểm tra bằng cách đọc thanh ghi PHY ID: reg2 = **0x0243**, reg3 = **0x0C54** (IP101G-DS p22).

**Ánh xạ RMII.** Đọc trực tiếp từ hộp alias net trên SCH. Khớp với IO-MUX GMAC (DS p.19, p.82) và với `ETH_ESP32_EMAC_DEFAULT_CONFIG()` của IDF v5.5.4.

| Tín hiệu | GPIO | Chân PHY |
|---|---|---|
| TXD0 | 34 | 9 |
| TXD1 | 35 | 8 |
| TX_EN | 49 | 5 |
| RXD0 | 29 | 18 |
| RXD1 | 30 | 17 |
| CRS_DV | 28 | 19 |
| REF_CLK (vào P4) | 50 | 14 (50M_CLKO) và 10 (50M_CLKI) |
| MDC | 31 | 22 |
| MDIO | 52 | 23 (R41 1.5k kéo lên) |
| PHY_RSTN | 51 | 32 |

Các chân RMII **không** đưa ra JP1. Thông tin ngược lại trước đây đã bị bác bỏ.

**Xung clock**
- Thạch anh X1 25 MHz (C30/C37 15 pF).
- R39 kéo COL/RMII lên, nên PHY chốt chế độ RMII và tự sinh 50 MHz trên 50M_CLKO (IP101G-DS p45).
- Phía P4 phải cấu hình **`EMAC_CLK_EXT_IN` trên GPIO50**.
- Board không có dao động 50 MHz riêng và P4 không xuất clock (GPIO23 dùng cho backlight, GPIO39 dùng cho SD D0).

**Địa chỉ PHY = 1**
- R47 kéo AD0 lên, R49 kéo AD3 xuống.
- Trên bản 32 chân, AD0 = 1 cho địa chỉ 1 bất kể AD3 (IP101G-DS p47 Table 4). Khớp ETH:792.

**Các strap khác**
- TXER: R43 kéo xuống, đúng yêu cầu RMII.
- FX_HEN: chốt 0, tức chế độ cáp xoắn.
- TEST_ON: có pull-down nội.
- ISET: R50 6.19k (đúng theo IP101G-DS p18).

**Từ tính và giắc**
- Biến áp rời L6 (16 chân), **không in mã**, cấp cách ly không rõ.
- Hai center tap chỉ nối tụ 100 nF xuống GND, đúng khuyến nghị "không nối nguồn" (IP101G-DS p7).
- Mạch Bob-Smith: 4 x 75R vào C65 1 nF/2 kV.
- RJ1 là HR6213S, giắc không tích hợp biến áp.
- Đèn LED xanh lá nối AD3/LED3, đèn vàng nối AD0/LED0.
- **Không có PoE.** Không tài liệu nào nói cấp cách ly.

**Nguồn PHY:** PHY_3V3 lấy từ ESP_3V3 qua R38 0R.

**Lưu ý thiết kế**
1. 50M_CLKO cấp cho hai tải mà không có buffer và không có điện trở nối tiếp trên carrier. IP101G-DS Fig.10 (p45) yêu cầu buffer khi kéo 2 tải; checklist Espressif khuyên dùng điện trở nối tiếp. Bên trong module có hay không thì chưa rõ **(chưa xác minh)**. Cần theo dõi bộ đếm lỗi CRC/symbol của IP101.
2. GPIO35 vừa là TXD1 vừa là BOOT.
3. PHY_RSTN không có mạch RC power-on. Luôn đặt `reset_gpio_num = 51`, vì mặc định của IDF là GPIO5.
4. Chỉ có **một cổng**, nên không làm được dự phòng vòng (ring redundancy).

### 3.2 Màn hình

**Đầu nối FPC1** (0.5 mm, 30 chân, SCH DSI_LCD)

| Chân | Tín hiệu |
|---|---|
| 1 | VCOM |
| 2–3 | 3V3 |
| 4 | LCD_RST (GPIO0) |
| 5 | STBYB |
| 7/8 | D0 N/P |
| 10/11 | D1 N/P |
| 13/14 | CLK N/P |
| 22 | AVDD10 |
| 23–24 | LEDK |
| 25 | SHLR |
| 26 | UPDN |
| 27 | VGL |
| 28 | VGH |
| 29–30 | LEDA |

- Lane 2 và 3 để NC. Panel chạy 2 lane (DTSI L23: `0x0B=0x11`).
- Các strap cố định bằng điện trở: STBYB = 1 (R91), UPDN = 1 (R87), SHLR = 0 (R90).

**Cấu hình DSI/DPI trong BSP**

| Tham số | Giá trị |
|---|---|
| bus_id | 0 |
| Số lane | 2 |
| Tốc độ lane | **750 Mbps**, viết cứng ở BSP_N.c:480 và BSP_O.c:598. Macro 1000 trong `display.h:67` không được dùng. |
| DPI | 52 MHz, RGB565, `use_dma2d = true`, `num_fbs = 1` |
| COLMOD | Viết cứng 0x55 (16-bit). **Giữ RGB565.** |

- Nguồn D-PHY: LDO_VO3 2500 mV (`include/bsp/display.h:69-70`).
- Thứ tự khởi tạo (BSP_N.c:467-593): brightness → LDO3 → `esp_lcd_new_dsi_bus` → `panel_io_dbi` → `esp_lcd_new_panel_jd9165` → reset → init.
- Chuỗi log trong BSP ghi "EK79007"/"ST7701", nhưng đó chỉ là chuỗi cũ còn sót lại.

**Đèn nền**
- IC1 MP3202 boost từ VOUT-BAT. EN nhận PWM từ GPIO23 (LEDC low-speed, timer 1, 10-bit).
- Panel cần 27 LED, 9.6 V / 180 mA (PANEL p2, p9).
- Dòng đặt bởi R66 3R9 ∥ R67 2R2. Chưa có datasheet MP3202 nên chưa tính được dòng thực tế.

**Nguồn bias panel:** U10 (không in mã) tạo AVDD10, VGH và VGL bằng bơm điện tích; VCOM chỉnh bằng biến trở R101 (SCH LCD_DC-DC).

**Panel driver:** registry `espressif/esp_lcd_jd9165` với phiên bản `"*"`. Gói vendor không kèm `dependencies.lock`, nên cần **ghim phiên bản** trong dự án.

### 3.3 Cảm ứng

**FPC3** (6 chân)

| Chân | Tín hiệu |
|---|---|
| 1 | 3V3 |
| 2 | TOUCH_INT (GPIO21) |
| 3 | TOUCH_RST (GPIO22) |
| 4 | SCL (GPIO8) |
| 5 | SDA (GPIO7) |
| 6 | GND |

- Điện trở kéo lên R65/R71 5K1. Nguồn: SCH Capacitive touch.
- GT911 nằm trên panel, địa chỉ 0x5D, `x_max` 1024, `y_max` 600 (BSP_N.c:640-666).
- I2C: BSP New đặt 400 kHz (BSP_N.c:663); BSP Old không đặt (BSP_O.c:767-768); Arduino dùng 100 kHz.
- **BSP để INT/RST = NC**, nên địa chỉ GT911 (0x5D hay 0x14) phụ thuộc trạng thái lúc cấp nguồn **(chưa xác minh)**. Firmware của dự án nên điều khiển GPIO22 (RST) và GPIO21 (INT).
- BSP New có callback scale 800x480 → 1024x600 nhưng đang bị comment (BSP_N.c:595-631, 658-659).

### 3.4 ESP32-C6 (Wi-Fi/BT)

**Kết nối SDIO trong module** (HOST:2829-2896)
- Dùng ESP-Hosted 2.11.7 kèm `esp_wifi_remote` (`IDF_N/esp_brookesia_phone/main/idf_component.yml:3-6`).
- SDIO slot 1, 4-bit, 40 MHz. CLK 18, CMD 19, D0–D3 = 14–17.
- Reset slave = GPIO54, mức cao là chạy, trễ 1500 ms.
- Preset "P4 Function EV board" (HOST:2829).

**Lưu ý:** SDMMC slot 0 dùng cho thẻ TF, slot 1 dùng cho C6, nên **cả hai slot SDMMC đều đã bận**.

**Nạp lại C6**
- UART0 của C6 ra JP1-20/22; IO9 (boot, R80 5K1 kéo lên) ra JP1-24; CHIP_PU ra JP1-26.
- Firmware slave: `vendor/JC1060/8-Burn operation/Burn files/JC-C6-slave_v2.3.2.bin` (IDF v5.5, OTA 2 x 1.5 MB).
- Vendor không có hướng dẫn nạp C6 **(chưa xác minh quy trình)**.

**Ăng-ten:** ANT1 CA-C03 chip ceramic (SCH).

### 3.5 RS-485 / UART1

**Mạch**
- U8 "MAX485" lấy nguồn **USB5V_IN (5 V)**.
- RO → R70 0R → GPIO27; GPIO26 → R75 0R → DI.

**Tự đổi chiều**
- U7 74LVC1G132 cùng RC R64 2K2 / C59 100 nF và Q3 8550.
- Driver bật ở bit 0 đầu tiên và giữ khoảng τ = 220 µs (thực tế cỡ 0.2–0.5 ms) sau khi TX về mức 1.
- Trong khoảng đó **receiver bị mù**.

**Bias và bảo vệ**
- R77 10k từ A lên 5 V, R68 10k từ B xuống GND, R73 5K1 giữa A–B.
- **Không có trở cuối 120 Ω.**
- TVS SMAJ6.5CA, cầu chì F1/F2 0.1 A.

**Đầu nối**
- J4: 1 = 5 V, 2 = A, 3 = B, 4 = GND.
- J5 "UART1": 1 = 5 V, 2 = TX1, 3 = RX1, 4 = GND.
- J5 **mắc song song** với MAX485, nên không dùng J5-RX cùng lúc với RS485 được.

**Demo**
- UART1, 115200 8N1, `UART_MODE_RS485_HALF_DUPLEX`, RTS = `UART_PIN_NO_CHANGE` (`rs485_example.c:28-81`).

**Rủi ro:** RO 5 V đi thẳng vào chân 3.3 V. Cần đo mức cao thực tế của RX1, hoặc dùng transceiver 3.3 V hay điện trở nối tiếp.

### 3.6 Âm thanh

| Mục | Giá trị |
|---|---|
| Codec | ES8311 tại 0x18, chân CE kéo thấp bởi R1 |
| I2S | MCLK 13, SCLK 12, LRCK 10, DOUT 9, DIN 48 (BSP-H:77-81) |
| Ampli | NS4150 trên GPIO11, nguồn VOUT-BAT, ra CN1 |
| Mic | MEMS analog MSM381A3729H9CP |
| Cổng I2S | BSP `CONFIG_BSP_I2S_NUM=1` |
| Chế độ mặc định | New: STEREO 44.1 kHz (BSP_N.c:72). Old: MONO 22.05 kHz (BSP_O.c:104). |

### 3.7 Thẻ TF

- SDMMC slot 0 qua IO MUX, 4-bit: D0–D3 = 39–42, CLK 43, CMD 44.
- Nguồn TF_VCC lấy từ LDO_VO4 (U3-58) qua Q1 AO3401. R10 = NC nên **luôn được cấp nguồn**.
- Không có chân card-detect (BSP_N.c:120-143; SCH).
- **Cảnh báo (suy luận):** nếu VDD_IO_5 cũng lấy từ VO4 thì GPIO39–48 và các chân header 45–47 sẽ thay đổi theo VO4. Không cho driver SD chuyển sang 1.8 V (UHS).

### 3.8 Camera

**FPC2** (15 chân, kiểu Raspberry Pi)
- CSI 2 lane.
- I2C dùng chung với GPIO7/8 qua R30/R32 0R.
- CSI_IO0 chỉ có điện trở kéo lên; CSI_IO1 không nối.

**Phần mềm demo**
- OV02C10 RAW10 (`IDF_N/video_lcd_display/sdkconfig:2505`). Demo dùng 1920x1080 30 fps trên **1 lane** (sdkconfig dòng 2518–2523), SCCB 100 kHz (dòng 777).
- Muốn dùng camera phải thay bằng `vendor/JC1060/1-Demo/Demo_IDF/components/esp_cam_sensor` v2.1.0 (`IDF_N/README.md` L1-3).
  - Bản đó là esp_cam_sensor 2.1.0 gốc cộng thêm thư mục `sensors/ov02c10`. Firmware chỉ lấy thư mục này (`components/cam_ov02c10`).

**Đã chạy trên board (2026-10-13):**
- Driver đọc được chip ID 0x5602 ở địa chỉ 0x36 trên I2C bus 1.
- Stream 1920x1080 qua Wi-Fi: Settings › Camera, chi tiết ở `docs/CAMERA.md`.
- Ảnh gốc của module bị lật trái/phải.

### 3.9 USB, nạp firmware và console

**USB2 "Full Speed" Type-C = USB-Serial-JTAG (GPIO24/25). Đây là cổng nạp.**
- BURN L2: chữ Hán ghi "USB2口" là **đúng**; dòng tiếng Anh "USB1 port" là **sai**.
- Log nạp ghi "Download mode: usb".

**USB3 "High Speed" Type-C:** HS OTG, chân chuyên dụng.

**VBUS và CC**
- VBUS của cả hai cổng nối thẳng USB5V_IN.
- CC chỉ có Rd (board chỉ là sink).
- Không có chip cầu USB-UART.

**Console**
- UART0 trên GPIO37/38, ra CN2.
- Demo bật USB-Serial-JTAG làm console phụ (`lvgl_demo_v9/sdkconfig:1561-1562`; ETH:1193-1203), nên `idf.py monitor` qua USB2 vẫn thấy log.

### 3.10 Nguồn

- **Đầu vào 5 V:** VBUS hai cổng USB-C, và CN2-1 qua Q2 AO3401. Tất cả gộp vào USB5V_IN, có TVS SMBJ6.0CA.
- **Đường nguồn:** USB5V_IN → IP5306 (sạc pin, boost) → VOUT-BAT → R22 "1A" → TLV62569 buck → **VCC3V3 = ESP_3V3 = VDDA**, một rail 3.3 V duy nhất.

**Tải theo rail**

| Rail | Tải |
|---|---|
| VOUT-BAT | Đèn nền, ampli, JP1-2/4 |
| USB5V_IN | MAX485, U7, chân 1 của J4/J5 |
| ESP_3V3 | PHY, cảm ứng, RTC, camera |

- **Suy luận:** khi chỉ chạy bằng pin thì USB5V_IN không có điện, nên RS485 cũng mất nguồn.
- **Đo pin:** GPIO53, ADC2_CH4, hệ số chia 0.595. `adc_test.ino:11-12` ánh xạ 2200–2450 mV thành 0–100%.

### 3.11 Nút bấm và LED

**Nút**
- SW1 BOOT: GPIO35 xuống GND. Đây cũng là RMII TXD1.
- SW2 RESET: CHIP_PU.
- SW3: phím IP5306.
- **Vị trí** (SPEC p5, ảnh mặt sau, nhìn vào mạch, RJ45 và hai cổng Type-C ở mép phải):
  - **BOOT (số 1) và RESET (số 2)** là hai nút nhỏ sát nhau ở **mép trên, phía trái**, ngay trên module JC-ESP32P4-M3. BOOT ở ngoài cùng bên trái, RESET ngay bên phải nó.
  - Đi tiếp sang phải theo mép trên: cổng loa (3), khe thẻ TF (4), cổng pin (5), rồi **nút pin (6)**. Nút số 6 là SW3 của IP5306, không phải RESET.
  - Mép phải: Type-C "Full Speed" (7, cổng nạp) ở trên, Type-C "High Speed" (9) ở dưới, ngay cạnh RJ45.

**LED**
- D1 đỏ: báo nguồn 3.3 V.
- Hai LED trên RJ45.
- **Không có LED nào điều khiển bằng GPIO.** XZ:21 "BUILTIN_LED = 26" là sai.

## 4. GPIO còn trống

Tất cả nằm trên JP1 (SCH Expand IO). **Không chân nào là strap**; các chân strap là 34–38 (DS p.32).

| GPIO | Chân JP1 | Miền nguồn | Ghi chú |
|---|---|---|---|
| 1 | 7 | VDD_LP/VDD_BAT | LP GPIO |
| 2 | 9 | VDD_LP/VDD_BAT | Pad JTAG MTCK, chỉ có tác dụng khi đốt eFuse; pull-up yếu |
| 3 | 11 | VDD_LP/VDD_BAT | MTDI |
| 4 | 13 | VDD_LP | MTMS |
| 5 | 15 | VDD_LP | MTDO. **Demo Arduino và video_lcd_display điều khiển chân này.** |
| 20 | 17 | VDD_IO_0 | ADC1_CH4 |
| 32 | 19 | VDD_IO_4 (3.3 V) | Nên dùng cho xung debug hoặc đo jitter |
| 33 | 21 | VDD_IO_4 | Nên dùng cho xung debug hoặc đo jitter |
| 45 | 14 | VDD_IO_5 | **Đo thông mạch JP1-14 với GND/3V3 trước khi dùng** |
| 46 | 12 | VDD_IO_5 | |
| 47 | 10 | VDD_IO_5 | |

**Các chân JP1 khác**

| Chân JP1 | Tín hiệu |
|---|---|
| 1, 3, 18 | 3V3 |
| 2, 4 | VOUT-BAT (khoảng 5 V) |
| 5, 6, 16 | GND |
| 8 | NC |
| 23 / 25 | I2C SDA/SCL. Thêm thiết bị I2C được; đã có pull-up 5K1 ∥ 2K2. |
| 20 / 22 / 24 | UART0 RX/TX và IO9 của C6 |
| 26 | C6_CHIP_PU (dành riêng) |

**Không dùng được**
- GPIO6 (net C6_IO2) và GPIO36 (strap) không ra header.
- GPIO14–19 nằm trong module.
- GPIO24/25 là USB.

**Cảnh báo điện áp:** điện áp các miền VDD_IO_0/5/LP nằm trong module và không có tài liệu. Đo mức cao thực tế trên JP1-10/12/14/17 trước khi nối thiết bị ngoài **(chưa xác minh)**.

**Đề xuất cho dự án:** GPIO32 làm chân "cycle start" và GPIO33 làm "RX done", để đo jitter EtherCAT bằng oscilloscope hoặc logic analyzer.

## 5. New panel và Old panel

**Phần cứng:** chỉ có một sơ đồ V1.0, không đánh dấu biến thể. Đầu nối FPC1 và mọi gán GPIO giống nhau ở cả hai bản. Khác biệt nằm ở tấm kính LCD phía sau cùng IC JD9165.

**Phần mềm**

1. **Bảng init JD9165** (khác biệt quyết định)
   - New: page-1 `0x05 = 0x08`, `0x23 = 0x38`, GIP khác, gamma `0x12 = 3F 25 27 35…`.
   - Old: `0x05 = 0x00`, `0x23 = 0x00`, gamma `36 2C 2E 3C…`.
   - Vị trí: BSP_N.c:404-466 (60 lệnh) và BSP_O.c:520-583 (51 lệnh).
   - Bảng Old trùng khớp DTSI (HKC QD070AS01-1). Vì vậy Old panel nhiều khả năng là kính HKC (suy luận). Không có tài liệu nào cho bảng New.
2. **Timing DPI:** HBP New = 136, Old = 160. HFP 160, VFP 12 và 52 MHz giống nhau. Các thông số khác của Old không thống nhất giữa các nguồn:

   | Nguồn (Old) | HSW / HBP / HFP / VSW / VBP / VFP |
   |---|---|
   | IDF BSP_O.c:629-634 | 24 / 160 / 160 / 2 / 21 / 12 |
   | Arduino Old `esp_lcd_jd9165.h:110-115` | 20 / 160 / 160 / 10 / 23 / 12 |
   | video_lcd_display Old | 20 / 160 / 160 / 10 / 23 / 12 |
   | xiaozhi `-y` | 24 / 160 / 160 / 10 / 21 / 12 |

   Nên bắt đầu từ bộ IDF.
3. **Tốc độ lane:** 750 Mbps ở mọi nguồn, riêng xiaozhi-new dùng 550 Mbps. README xiaozhi gợi ý thử 550/750/900 nếu màn hình tối.
4. **Khác biệt riêng của BSP:** PWM đèn nền 20 kHz (New) / 5 kHz (Old); I2S mặc định; BSP Old có thêm các hàm delete/deinit và camera.
5. **Không khác:** các demo ethernet, RS485, lvgl v8/v9 và draw_bits giống hệt nhau giữa hai bản.

**Cách nhận biết bản của mình**
- **(a)** Nhãn sau vỏ có chữ **"V2"** sau mã vật liệu nghĩa là **New panel** (BURN L3).
- **(b) Bẫy đặt tên:** firmware nhà máy **"V2.x" lại là firmware cho Old panel**.
  - V2.1, V2.4, V2.7 và xiaozhi2.0.4 chỉ chứa bảng gamma Old.
  - `JC1060P470C_I_W_Y-V3.7_New_Panel.bin` chỉ chứa bảng New.
  - `SOFTWARE_VERSION` trong `esp_brookesia_phone/main/main.cpp:21` là v3.7.0 (New) và v2.7.0 (Old).
- **(c)** Không có cách phát hiện lúc chạy: driver đọc thanh ghi ID 0x04 nhưng bỏ qua giá trị.
- **(d) Kiểm tra thực tế:** build `lvgl_demo_v9` với BSP New. Nếu ảnh sai (màu/gamma, nhấp nháy, đen) thì chuyển sang BSP Old.
- **(e) Với firmware của dự án:** không cần build lại. Gõ `panel old` hoặc `panel new` trong console rồi nhấn RESET. Lựa chọn được lưu trong NVS (`SETUP.md` mục 7b).
- **Board của người dùng (2026-10-10):** nhãn module LCD kết thúc bằng "V2", theo (a) là **New panel**. Đây cũng là mặc định của firmware (`CONFIG_BOARD_LCD_PANEL_NEW`). **Chưa kiểm bằng mắt:** nếu lần chạy đầu ảnh sai màu, nhấp nháy hoặc đen thì gõ `panel old`.

## 6. Danh mục demo của vendor

### ESP-IDF (`IDF_N/` và `IDF_O/`)

| Demo | Nội dung | Ghi chú |
|---|---|---|
| `ethernet` | Ví dụ gốc `examples/ethernet/basic` của Espressif: EMAC + IP101 + lwIP DHCP | Giống hệt nhau New/Old. sdkconfig: flash 2 MB, -Og, tick 100 Hz, không bật PSRAM. **Smoke test #1.** |
| `lvgl_demo_v9` | LVGL 9.5.0 + `esp_lvgl_port` ^2, `main.c` 46 dòng | **Template cho HMI**, smoke test #2 |
| `lvgl_sw_rotation` | Như v9 nhưng xoay 90° | |
| `lvgl_demo_v8`, `esp_draw_bits` | LVGL 8.4 | |
| `uart_echo_rs485`, `uart_send_rs485` | RS485 qua UART1 26/27 | |
| `video_lcd_display` | Camera OV02C10 → LCD, MJPEG | Dùng LCD_RST = 5 (sai) |
| `esp_brookesia_phone` | Firmware nhà máy kiểu "điện thoại": Wi-Fi, Ethernet, SD | LVGL < 9 |
| `xiaozhi-esp32-main` | Trợ lý giọng nói AI | config.h **sai** ở GPIO26/27/21 |
| `common_components/` | BSP (Espressif EV board 5.2.3 đã sửa), `bsp_extra` 0.0.2, `espressif__esp-brookesia` 0.4.2 | |

Thư mục `vendor/JC1060/1-Demo/Demo_IDF/components/` chứa bản sao esp-brookesia 0.4.2 và esp_cam_sensor 2.1.0. Không project nào tham chiếu đến thư mục này.

### Arduino (`vendor/JC1060/1-Demo/Demo_Arduino/JC1060P470C_I_W_Y_{New,Old}_Panel/`)

- Gồm `adc_test`, `lvgl_demo_v8`, `lvgl_v9_sw_rotation`, `mp3_player`, `Wifi_scan`.
- Cấu hình: board "ESP32P4 Dev Module", 360 MHz, flash QIO 16 MB, PSRAM bật (`配置.png`).
- **LCD_RST = 5 là sai.**

### Firmware nạp sẵn (`vendor/JC1060/8-Burn operation/Burn files/`)

- `-V2.1`, `-V2.4`, `-V2.7`: Old panel.
- `-V3.7_New_Panel`: New panel.
- `_xiaozhi2.0.4`: Old panel.
- `JC-C6-slave_v2.3.2.bin`: firmware cho ESP32-C6.
- Mọi image P4 đều là image gộp, nạp ở địa chỉ 0x0, và chỉ chạy trên chip rev < 3.0.

## 7. Vị trí tài liệu

| Thư mục | Nội dung |
|---|---|
| `vendor/JC1060/2-Specification/` | Thông số (EN), 6 trang |
| `vendor/JC1060/3-Structure_Diagram/` | Bản vẽ cơ khí `.pdf` và `.dwg` |
| `vendor/JC1060/4-Driver_IC_Data_Sheet/` | Datasheet ESP32-P4 CN v0.4.1 (bản pre-release); panel JC1060M070N_I; DTSI JD9165BA; OV02C10; `描3.jpg` (sơ đồ chân FPC cảm ứng/LCD) |
| `vendor/JC1060/5-Schematic/` | Sơ đồ nguyên lý V1.0 |
| `vendor/JC1060/6-User_Manual/` | Hướng dẫn bắt đầu (chủ yếu cho Arduino), 12 trang |
| `vendor/JC1060/7-Character&Picture_Molding_Tool/` | Công cụ phụ: JPGCompact, driver CH340, sscom… |
| `vendor/JC1060/8-Burn operation/` | Flash Download Tool 3.9.9, ảnh hướng dẫn nạp, file .bin |

**Không có trong gói vendor:** datasheet IP101G, ESP32-P4 TRM, datasheet MP3202 và U10, cùng mã linh kiện của L6, flash và PSRAM.

## 8. Câu hỏi phần cứng còn mở

1. Mã chip PHY U4 và biến áp L6 thực tế là gì (đọc chữ in trên linh kiện)? Cấp cách ly của L6 là bao nhiêu?
2. Revision silicon ESP32-P4 trên board là gì? (Mục 3 của SETUP.md hướng dẫn cách đọc.)
3. JP1-14 (GPIO45) có bị chập với chân 16/18 không?
4. Điện áp VDD_IO_0/5/LP là bao nhiêu? VDD_IO_5 có lấy từ VO4 không?
5. Đường REF_CLK có điện trở nối tiếp bên trong module không?
6. GPIO6 có nối với IO2 của C6 không?
7. Mức điện áp RO của MAX485 vào GPIO27 là bao nhiêu?
8. GT911 lên địa chỉ nào khi không điều khiển INT/RST?
9. Bộ timing nào của Old panel đã được kiểm chứng?
10. ~~Cổng camera FPC2 có được lắp không?~~ Có: module OV02C10 trả lời (2026-10-13, `docs/CAMERA.md`).
