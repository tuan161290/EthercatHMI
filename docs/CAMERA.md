# Camera OV02C10 và ghi hình trên TrueNAS

Camera trên board phát hình MJPEG qua Wi-Fi. Trên TrueNAS, go2rtc nhận stream đó để xem trực tiếp và ghi thành file MP4. Bật hoặc tắt camera ở **Settings › Camera**.

Code liên quan:
- `firmware/components/camera/`: chụp ảnh, nén JPEG, server HTTP.
- `firmware/components/board/board_camera.c`: nối camera vào `esp_video`.
- `firmware/components/cam_ov02c10/`: driver cảm biến của hãng.
- `firmware/components/hmi/hmi_page_camera.c`: trang Settings › Camera.
- `server/camera/`: hai file cài trên TrueNAS.

## 1. Phần cứng

- **Đầu nối:** FPC2, 15 chân kiểu Raspberry Pi, MIPI-CSI (`docs/BOARD.md` 3.8).
- **Cảm biến:** OmniVision OV02C10, 2 MP.
  - Đã xác nhận trên board ngày 2026-10-13: driver đọc được chip ID 0x5602 ở địa chỉ I2C 0x36. Câu hỏi mở số 10 của BOARD.md ("FPC2 có lắp camera không?") vì vậy đã có câu trả lời.
- **SCCB:** dùng chung I2C bus 1 (GPIO7/8) với GT911, ES8311 và RX8025T, chạy ở 100 kHz như demo của hãng (`video_lcd_display/sdkconfig:777`).
- **Reset, power-down, clock:** không có chân reset hay power-down. Clock 24 MHz do module tự tạo.
- **Nguồn D-PHY:** dùng chung LDO_VO3 2500 mV với màn hình.
  - `esp_video` xin cùng kênh và cùng điện áp với `board_display.c`. Driver LDO đếm cả hai người dùng nên không xung đột.
- **Chế độ đang dùng:** 1920x1080, RAW10, 30 fps, 1 lane, giống demo `video_lcd_display` của hãng (`sdkconfig` dòng 2518–2523).
  - Driver còn có 1920x1080 trên 2 lane và 1288x728, nhưng **chưa thử**.
- **Driver:** OV02C10 không có trong `esp_cam_sensor` trên registry của Espressif (đã kiểm tra các bản 1.7.0, 2.1.0 và 2.6.0). Vì vậy dùng bản của hãng, chép nguyên vào `components/cam_ov02c10`, không sửa. Chi tiết trong `VENDOR.md` của component đó.

## 2. Dùng trên HMI

**Settings › Camera** (ô thứ 11 của lưới Settings, quyền Admin):
- **Công tắc Camera:** lưu trong machine store (key `camera`), nên lưu được cả khi EtherCAT đang OP.
- **Picture size:** kích thước ảnh gửi đi, lưu cùng key `camera`. Đổi khi đang phát thì stream khởi động lại; go2rtc tự nối lại. Xem mục 3a.
- **Thẻ trạng thái:**

  | Thẻ | Ý nghĩa |
  |---|---|
  | Off | Camera tắt. |
  | Starting | Đang tìm cảm biến. |
  | Not available | Không dùng được; dòng bên cạnh ghi lý do. Tắt rồi bật lại công tắc để thử lại. |
  | Waiting for Wi-Fi | Đang chờ Wi-Fi kết nối. |
  | Ready | Server chạy, cảm biến dừng vì chưa có ai xem. |
  | Streaming | Đang phát hình. |

- **Các dòng thông tin:**
  - Stream: `http://<IP Wi-Fi của HMI>/stream`, MJPEG, xem được bằng trình duyệt, VLC hoặc go2rtc.
  - Snapshot: `http://<IP>/snapshot.jpg`, lấy một ảnh JPEG.
  - Địa chỉ `http://<IP>/` mở một trang xem nhanh.
  - Ngoài ra trang hiện kích thước ảnh, số khung/s, Mbit/s và kích thước một khung.
- **Ô bên phải:** in sẵn dòng cấu hình cho go2rtc, có điền IP hiện tại.

**Console:**

| Lệnh | Tác dụng |
|---|---|
| `cam` | Trạng thái, địa chỉ, số người xem, khung/s, kbit/s, kích thước khung |
| `cam on`, `cam off` | Như công tắc trên HMI (có lưu) |
| `cam size 1280x720` | Đổi kích thước ảnh (có lưu): 1920x1080, 1280x720, 1024x576, 720x480, 640x360 |
| `wifi tasks` | Core và priority của `isp_task`, `camera`, `cam_http0`, `httpd`. Tất cả phải ở core 0. |
| `mem` | RAM, và stack còn trống của các task camera |

**Bảo mật:** stream **không có mật khẩu**. Ai trong cùng mạng LAN cũng xem được. Không mở port của HMI ra Internet; muốn xem từ xa thì dùng VPN (mục 5).

## 3. Firmware chạy thế nào

**Core:** mọi thứ của camera chạy trên **core 0**. Core 1 vẫn chỉ dành cho EtherCAT.

| Task | Priority | Stack | Ghi chú |
|---|---|---|---|
| `camera` | 2 | 8 KB, PSRAM | Điều khiển cảm biến, nén JPEG, bật/tắt server |
| `cam_http0..2` | 2 | 4 KB mỗi task, PSRAM | Gửi stream hoặc snapshot, mỗi task một kết nối |
| `httpd` | 2 | 4 KB, RAM nội | Server HTTP của ESP-IDF |
| `isp_task` | 11 | 4 KB, PSRAM | Của `esp_video`: auto exposure và white balance |

- **Ép `isp_task` về core 0:** `esp_video` tạo task này mà không chỉ định core. `board_camera.c` dùng `-Wl,--wrap` trên hai hàm tạo task để ép riêng nó về core 0. Kiểm tra lại mỗi khi cập nhật `esp_video` (tên task, cách tạo).
- **Đường đi của ảnh:**
  1. Cảm biến → CSI → ISP. ISP dùng file tinh chỉnh (IPA) của hãng, tự chỉnh phơi sáng và cân bằng trắng.
  2. ISP xuất RGB565 1920x1080 vào 2 buffer.
  3. Nếu chọn kích thước nhỏ hơn (hoặc bật lật ảnh), PPA thu nhỏ ảnh vào một buffer riêng (mục 3a).
  4. Bộ nén JPEG phần cứng nén ảnh (chất lượng 50), tối đa `CONFIG_CAMERA_MAX_FPS` = 5 khung/s.
  5. Ảnh nén nằm trong một trong 4 buffer JPEG.
  6. Mỗi người xem luôn nhận khung **mới nhất**. Người xem chậm thì bỏ bớt khung chứ không dồn hàng đợi.
- **Cảm biến chỉ chạy khi cần:** khi có người xem hoặc có yêu cầu snapshot. Nó dừng sau 5 s không còn ai xem. Khi mới khởi động cảm biến, firmware bỏ 10 khung đầu để phơi sáng kịp ổn định.
- **Tắt camera:** dừng server, đóng thiết bị và trả lại PSRAM (khoảng 12 MB ở 1920x1080). Riêng `esp_video` và `isp_task` vẫn giữ lại sau lần bật đầu tiên.
- **Kconfig** (menu "Camera stream"):

  | Tuỳ chọn | Mặc định |
  |---|---|
  | `CAMERA_HTTP_PORT` | 80 |
  | `CAMERA_MAX_FPS` | 5 |
  | `CAMERA_JPEG_QUALITY` | 50 |
  | `CAMERA_MAX_VIEWERS` | 2 |
  | `CAMERA_HMIRROR` | n (lật ảnh bằng PPA; chưa thử) |

- **Bộ đệm gửi TCP:** `sdkconfig.defaults` tăng lên 32 KB (mặc định khoảng 5.7 KB) để một kết nối đẩy đủ nhanh.

## 3a. Kích thước ảnh

Cảm biến luôn chạy 1920x1080 (chế độ đã chạy thật). Kích thước nhỏ hơn do PPA (Pixel Processing Accelerator của P4) thu nhỏ trước khi nén, nên:
- **Hệ số thu nhỏ đi theo bước 1/16** (PPA có 4 bit phần lẻ, `PPA_SR_SCAL_X_FRAG_V` = 0xF). Firmware chọn hệ số nhỏ nhất k/16 vẫn phủ đủ ảnh, rồi lấy một vùng ở giữa ảnh cảm biến. Vì vậy các mức nhỏ mất một ít ở mép.
- **Kích thước là bội của 16 x 8:** bộ nén JPEG nhận ảnh YUV422 theo khối 16 x 8 (`DMA2D_MACRO_BLOCK_SIZE_8_16` trong `jpeg_encode.c` của IDF). Do đó không có 960x540 (540 không chia hết cho 8).

| Kích thước | Hệ số | Vùng lấy từ 1920x1080 | Mất ở mép | JPEG đo được (2026-10-13) |
|---|---|---|---|---|
| 1920 x 1080 | 1 (không qua PPA) | toàn bộ | 0 | 64–93 KB |
| 1280 x 720 | 11/16 | 1862 x 1048 | khoảng 3 % | 45 KB |
| 1024 x 576 | 9/16 | 1822 x 1024 | khoảng 5 % | 31 KB, stream 4,8 khung/s, 1,25 Mbit/s |
| 720 x 480 | 8/16 | 1440 x 960 | **25 % chiều ngang** (ảnh 3:2), 11 % chiều dọc | 18 KB |
| 640 x 360 | 6/16 | 1708 x 960 | khoảng 11 % | 15 KB |

Kích thước khung phụ thuộc cảnh (ảnh chụp trong văn phòng có đèn trần). Stream 1024x576 cho một người xem: RAM nội trống 61 KB (1920x1080: 54–58 KB).

Đổi kích thước khi camera đang chạy:
1. Các stream đang mở kết thúc.
2. Thiết bị đóng, rồi mở lại với buffer mới (khoảng 0,1 s).
3. Server HTTP vẫn chạy trong suốt quá trình.

**Lật ảnh:** cũng do PPA làm, nên không đụng tới bit mirror của cảm biến (bit đó làm lệch thứ tự Bayer mà ISP dùng). Ở 1920x1080, lật ảnh tốn thêm một lượt PPA trên toàn ảnh.

## 4. Kết quả đo trên board

Đo ngày 2026-10-13, không có slave EtherCAT, Wi-Fi "OpenWrt" ở -61 dBm.

| Mục | Kết quả |
|---|---|
| Bật camera | Thấy cảm biến, chuyển sang Ready ngay khi Wi-Fi có IP |
| Snapshot từ PC | 1920x1080, 66–69 KB. Khoảng 0,9 s khi cảm biến đang dừng, 1,6 s khi đang có một stream khác |
| Stream 25 s, 1 người xem | 4,8 khung/s, khoảng 2–3 Mbit/s, khoảng 64 KB một khung |
| CPU khi stream | Core 0: 4–10 %. Core 1: 0 % |
| PSRAM | Khoảng 12 MB khi camera bật ở 1920x1080 (2 × 4 MB RGB565 + 4 × 1 MB JPEG); ít hơn ở kích thước nhỏ; trả lại khi tắt |
| RAM nội trống | 84,8 KB khi Ready ngay sau khởi động. 56–58 KB trong và sau khi stream 1920x1080, thấp nhất 54,3 KB; 61 KB khi stream 1024x576. Khối liền lớn nhất 31 KB ở mọi trạng thái |
| Stack còn trống khi stream | `camera` 3,9 KB (của 8 KB), `cam_http*` khoảng 2,3 KB (của 4 KB), `httpd` 3 KB (của 4 KB), `isp_task` 1,1 KB (của 4 KB) |

So sánh RAM nội: trước khi có camera, chạy Wi-Fi còn khoảng 105 KB trống (CLAUDE.md, 2026-10-11). Chưa đo lại mức này với camera tắt ngay từ lúc khởi động.

Sự cố đã gặp: lần chạy đầu, task `camera` với stack 4 KB bị tràn khi khởi tạo cảm biến, `esp_video` và IPA (panic "Stack protection fault"). Đã tăng lên 8 KB. Trên P4, vùng lưu FPU và hardware loop của task cũng được cắt từ chính stack đó.

**Chất lượng ảnh**
- Ảnh bị **lật trái/phải**: chữ trên áo đọc ngược.
  - `CONFIG_CAMERA_HMIRROR=y` lật ảnh bằng PPA (mục 3a), nhưng **chưa thử**.
- Màu hơi nhạt và ngả xanh. Đây là file IPA mặc định của hãng, chưa tinh chỉnh.

## 5. Cài trên TrueNAS SCALE

**Hai trường hợp:**
- **HMI và NAS cùng mạng LAN:** go2rtc kéo stream thẳng từ IP của HMI. Làm theo các bước bên dưới.
- **HMI ở chỗ khác** (xưởng) và NAS ở nhà: go2rtc không kéo qua Internet được nếu không có đường riêng.
  - Cách nên dùng: VPN giữa hai nơi, ví dụ Tailscale trên TrueNAS (có trong Apps) và Tailscale làm subnet router trên router OpenWrt ở xưởng, hoặc WireGuard site-to-site. Sau đó go2rtc dùng đúng IP LAN của HMI như khi ở cùng mạng.
  - Không mở port forward tới HMI, vì stream không có mật khẩu.
  - Nếu không làm được VPN thì cần cho firmware tự **đẩy** hình lên NAS. Phần này chưa làm.

**Các bước** (TrueNAS SCALE 24.10 trở lên, app chạy bằng Docker):
1. Trong router, đặt IP cố định (DHCP reservation) cho HMI, để địa chỉ stream không đổi.
   - Hostname là `ethercat-hmi`.
   - MAC là MAC Wi-Fi của chip ESP32-C6, khác MAC Ethernet của P4. Xem trong danh sách DHCP của router.
2. Tạo hai dataset, đổi `tank` thành tên pool của mình:
   - `tank/apps/go2rtc`: chứa cấu hình.
   - `tank/camera`: chứa video.
3. Chép `server/camera/go2rtc.yaml` vào dataset `tank/apps/go2rtc`, qua SMB share hoặc **System › Shell**. Sửa dòng `ehmi: http://192.168.1.50/stream` thành dòng Stream ở Settings › Camera.
4. Vào **Apps › Discover Apps › ⋮ › Install via YAML**:
   - Tên: `ehmi-camera`.
   - Dán nội dung `server/camera/compose.yaml`.
   - Sửa hai đường dẫn `/mnt/tank/...`, `TZ` và `KEEP_DAYS` (số ngày giữ video, mặc định 14).
5. Mở `http://<NAS>:1984`: stream `ehmi` là hình trực tiếp. VLC mở được `rtsp://<NAS>:8554/ehmi`.
6. Video nằm ở `/mnt/tank/camera/ehmi-YYYYMMDD-HHMMSS.mp4`.
   - Mỗi file 10 phút, bắt đầu đúng mốc 10 phút của đồng hồ.
   - Định dạng MP4 phân mảnh, nên file vẫn xem được khi bị ngắt giữa chừng.
   - File cũ hơn `KEEP_DAYS` bị xoá mỗi giờ.

**Ghi chú**
- **Chạy liên tục khi có ghi hình:** service `recorder` lúc nào cũng xem, nên HMI phát liên tục khi camera bật. Chỉ muốn xem trực tiếp thì xoá service `recorder`; khi đó HMI chỉ phát lúc có người mở go2rtc.
- **Tải trên NAS:** go2rtc chuyển MJPEG sang H.264 bằng CPU của NAS (ffmpeg, libx264), 1080p ở 5 khung/s. Chưa đo tải CPU của NAS, cũng chưa đo dung lượng video mỗi ngày.
- **Khi HMI không phát:** camera tắt hoặc không tới được HMI thì `recorder` ghi log "stream not available" và thử lại sau 10 s.
- **Chưa chạy thử:** stack trên TrueNAS chưa được chạy thật, vì máy làm việc không có Docker.
  - Image: `alexxit/go2rtc:1.9.14` (bản phát hành 2026-01-19 trên Docker Hub). Image này có sẵn ffmpeg.

## 6. Còn mở

1. **Ảnh hưởng tới EtherCAT khi stream:** chưa đo khi bus ở OP với 10 slave. Cần chạy `cam on`, mở stream, rồi theo dõi overrun, wake late và RTT như các lần đo Wi-Fi trước (CLAUDE.md).
   - Rủi ro thứ nhất: lưu lượng Wi-Fi qua các task esp_hosted ở priority 23 trên core 0.
   - Rủi ro thứ hai: băng thông PSRAM. Theo ước tính, ISP ghi khoảng 124 MB/s ở 1080p 30 fps (1920 × 1080 × 2 byte × 30), cạnh màn hình và code XIP từ PSRAM.
2. **RAM nội** giảm khoảng 27 KB trong lúc stream 1920x1080 (khoảng 24 KB ở 1024x576). Nghi là buffer TCP và hàng đợi gửi của esp_hosted, chưa xác minh. Nếu cần lấy lại RAM, có thể giảm `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` hoặc `CAMERA_MAX_VIEWERS`.
3. **Màn hình HMI khi camera chạy:** chưa xem màn hình có nháy hay giật không.
4. **Lật ảnh và màu:** chưa thử `CAMERA_HMIRROR`, chưa tinh chỉnh IPA.
5. **Phân mảnh PSRAM:** sau vài lần đổi kích thước, khối PSRAM liền lớn nhất còn 8,9 MB (trống 18,9 MB). 1920x1080 cần hai khối 4 MB của `esp_video` cùng 4 khối 1 MB. Chưa thấy lỗi, nhưng nên theo dõi `mem` nếu đổi kích thước nhiều lần.
6. **Stack trên TrueNAS:** chưa chạy thật (mục 5).
