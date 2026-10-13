# Camera OV02C10 và ghi hình trên TrueNAS

Camera trên board phát hình MJPEG qua Wi-Fi theo hai cách, chạy được cùng lúc:
- **Đẩy về server (push):** HMI tự mở kết nối ra Internet, gửi hình tới go2rtc trên TrueNAS qua tên DDNS. Nhà xưởng không phải mở port nào. Đây là cách dùng khi NAS ở chỗ khác (mục 5).
- **Xem trong LAN (pull):** HMI chạy một server HTTP; trình duyệt, VLC hoặc go2rtc cùng mạng kéo hình về.

Cài đặt ở **Settings › Camera**.

Code liên quan:
- `firmware/components/camera/`:
  - `camera.c`: chụp ảnh, thu nhỏ, nén JPEG, server HTTP.
  - `camera_push.c`: đẩy hình về server.
- `firmware/components/board/board_camera.c`: nối camera vào `esp_video`.
- `firmware/components/cam_ov02c10/`: driver cảm biến của hãng.
- `firmware/components/hmi/hmi_page_camera.c`: trang Settings › Camera.
- `server/camera/`: các file cài trên TrueNAS.

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

**Settings › Camera** (ô thứ 11 của lưới Settings, quyền Admin). Mọi cài đặt được lưu trong machine store, nên lưu được cả khi EtherCAT đang OP.

### Ô bên trái: camera

- **Công tắc Camera** (key `camera`).
- **Picture size:** kích thước ảnh gửi đi, lưu cùng key `camera`.
  - Đổi khi đang phát thì stream khởi động lại; go2rtc tự nối lại.
  - Xem mục 3a.
- **Thẻ trạng thái:**

  | Thẻ | Ý nghĩa |
  |---|---|
  | Off | Camera tắt. |
  | Starting | Đang tìm cảm biến. |
  | Not available | Không dùng được; dòng bên cạnh ghi lý do. Tắt rồi bật lại công tắc để thử lại. |
  | Waiting for Wi-Fi | Đang chờ Wi-Fi kết nối. |
  | Ready | Server chạy, cảm biến dừng vì chưa có ai xem. |
  | Streaming | Đang phát hình (tới người xem trong LAN, tới server, hoặc cả hai). |

- **Các dòng thông tin:**
  - Stream: `http://<IP Wi-Fi của HMI>/stream`, MJPEG.
  - Snapshot: `http://<IP>/snapshot.jpg`, một ảnh JPEG.
  - Địa chỉ `http://<IP>/` mở một trang xem nhanh.
  - Ngoài ra trang hiện số khung/s, Mbit/s và kích thước một khung.

### Ô bên phải: Send to server

- **Công tắc Send to server** (key `campush`). Chỉ gửi khi camera đang bật và Wi-Fi đã kết nối.
- **Thẻ trạng thái:**

  | Thẻ | Ý nghĩa |
  |---|---|
  | Off | Tắt. |
  | Waiting | Chờ camera, Wi-Fi hoặc địa chỉ server (dòng dưới ghi đang chờ gì). |
  | Connecting | Đang kết nối và kiểm tra đăng nhập. |
  | Sending | Đang gửi; bên cạnh ghi Mbit/s. |
  | Not connected | Lỗi; dòng dưới ghi lý do. Tự thử lại sau 5 s, rồi 10, 20, 40, tối đa 60 s. |

- **Server:** địa chỉ đẩy của go2rtc, ví dụ `https://nha.ddns.net:8556/api/stream.mjpeg?dst=ehmi`. Bấm vào để gõ bằng bàn phím trên màn hình.
- **User, Password:** login của go2rtc nhận hình (Basic auth).
  - Mật khẩu lưu dạng chữ thường, như mật khẩu Wi-Fi, và không bao giờ hiện lại (chỉ ghi "Set").

**Các lỗi trang có thể báo:**

| Thông báo | Nguyên nhân thường gặp |
|---|---|
| `Wrong user or password (HTTP 401)` | User/Password trên HMI khác `config/ingest/go2rtc.yaml` |
| `No stream of that name (dst=) on the server (HTTP 404)` | `dst=` trong địa chỉ khác tên stream trong `config/ingest/go2rtc.yaml` |
| `Server not reachable (...)` | Sai tên DDNS hoặc port, router chưa forward, NAS tắt |
| `Server certificate not trusted (needs a public CA)` | HTTPS với chứng chỉ tự ký hoặc hết hạn. Cần chứng chỉ của CA công cộng như Let's Encrypt |
| `The server's proxy refused the stream (HTTP 413/502/504)` | Reverse proxy chưa tắt request buffering (mục 5.4) |
| `No picture from the camera` | Camera không ra hình trong 10 s |

### Console

| Lệnh | Tác dụng |
|---|---|
| `cam` | Trạng thái camera và push, địa chỉ, số người xem, khung/s, kbit/s, kích thước khung |
| `cam on`, `cam off` | Như công tắc Camera (có lưu) |
| `cam size 1280x720` | Đổi kích thước ảnh (có lưu): 1920x1080, 1280x720, 1024x576, 720x480, 640x360 |
| `cam push` | Trạng thái và cài đặt push |
| `cam push on`, `cam push off` | Như công tắc Send to server |
| `cam push url https://nha.ddns.net:8556/api/stream.mjpeg?dst=ehmi` | Đặt địa chỉ (`cam push url ""` để xoá) |
| `cam push login ehmi MatKhau` | Đặt user và mật khẩu (`cam push login ""` để xoá) |
| `wifi tasks` | Core và priority của `isp_task`, `camera`, `cam_http0`, `httpd`. Tất cả phải ở core 0 |
| `mem` | RAM, và stack còn trống của các task camera, kể cả `cam_push` |

**Bảo mật:**
- Server HTTP trong LAN của HMI **không có mật khẩu**: ai trong cùng mạng cũng xem được. Không mở port của HMI ra Internet.
- Push thì ngược lại: chỉ HMI mở kết nối ra ngoài, và phía nhận có mật khẩu.

## 3. Firmware chạy thế nào

**Core:** mọi thứ của camera chạy trên **core 0**. Core 1 vẫn chỉ dành cho EtherCAT.

| Task | Priority | Stack | Ghi chú |
|---|---|---|---|
| `camera` | 2 | 8 KB, PSRAM | Điều khiển cảm biến, thu nhỏ, nén JPEG, bật/tắt server |
| `cam_http0..2` | 2 | 4 KB mỗi task, PSRAM | Gửi stream hoặc snapshot trong LAN, mỗi task một kết nối |
| `cam_push` | 2 | 8 KB, PSRAM | Đẩy hình về server (TLS cần stack lớn hơn) |
| `httpd` | 2 | 4 KB, RAM nội | Server HTTP của ESP-IDF |
| `isp_task` | 11 | 4 KB, PSRAM | Của `esp_video`: auto exposure và white balance |

- **Ép `isp_task` về core 0:** `esp_video` tạo task này mà không chỉ định core. `board_camera.c` dùng `-Wl,--wrap` trên hai hàm tạo task để ép riêng nó về core 0. Kiểm tra lại mỗi khi cập nhật `esp_video` (tên task, cách tạo).
- **Đường đi của ảnh:**
  1. Cảm biến → CSI → ISP. ISP dùng file tinh chỉnh (IPA) của hãng, tự chỉnh phơi sáng và cân bằng trắng.
  2. ISP xuất RGB565 1920x1080 vào 2 buffer.
  3. Nếu chọn kích thước nhỏ hơn (hoặc bật lật ảnh), PPA thu nhỏ ảnh vào một buffer riêng (mục 3a).
  4. Bộ nén JPEG phần cứng nén ảnh (chất lượng 50), tối đa `CONFIG_CAMERA_MAX_FPS` = 5 khung/s.
  5. Ảnh nén nằm trong một trong 5 buffer JPEG.
  6. Mỗi người xem trong LAN và phần push luôn nhận khung **mới nhất**. Ai chậm thì bỏ bớt khung chứ không dồn hàng đợi.
- **Cảm biến chỉ chạy khi cần:** khi có người xem, có push đang gửi, hoặc có yêu cầu snapshot. Nó dừng sau 5 s không còn ai cần. Khi mới khởi động cảm biến, firmware bỏ 10 khung đầu để phơi sáng kịp ổn định.
- **Tắt camera:** dừng server, kết thúc push, đóng thiết bị và trả lại PSRAM (khoảng 12 MB ở 1920x1080). Riêng `esp_video` và `isp_task` vẫn giữ lại sau lần bật đầu tiên.
- **Kconfig** (menu "Camera stream"):

  | Tuỳ chọn | Mặc định |
  |---|---|
  | `CAMERA_HTTP_PORT` | 80 |
  | `CAMERA_MAX_FPS` | 5 |
  | `CAMERA_JPEG_QUALITY` | 50 |
  | `CAMERA_MAX_VIEWERS` | 2 |
  | `CAMERA_HMIRROR` | n (lật ảnh bằng PPA; chưa thử) |

- **`sdkconfig.defaults`:**
  - Bộ đệm gửi TCP tăng lên 32 KB (mặc định khoảng 5.7 KB) để một kết nối đẩy đủ nhanh.
  - mbedTLS cấp bộ nhớ trong PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`), vì mỗi kết nối TLS cần 16 + 4 KB buffer cộng phần bắt tay.

### 3a. Kích thước ảnh

Cảm biến luôn chạy 1920x1080 (chế độ đã chạy thật). Kích thước nhỏ hơn do PPA (Pixel Processing Accelerator của P4) thu nhỏ trước khi nén, nên:
- **Hệ số thu nhỏ đi theo bước 1/16** (PPA có 4 bit phần lẻ, `PPA_SR_SCAL_X_FRAG_V` = 0xF). Firmware chọn hệ số nhỏ nhất k/16 vẫn phủ đủ ảnh, rồi lấy một vùng ở giữa ảnh cảm biến. Vì vậy các mức nhỏ mất một ít ở mép.
- **Kích thước là bội của 16 x 8:** bộ nén JPEG nhận ảnh YUV422 theo khối 16 x 8 (`DMA2D_MACRO_BLOCK_SIZE_8_16` trong `jpeg_encode.c` của IDF). Do đó không có 960x540 (540 không chia hết cho 8).

| Kích thước | Hệ số | Vùng lấy từ 1920x1080 | Mất ở mép | JPEG đo được (2026-10-13) |
|---|---|---|---|---|
| 1920 x 1080 | 1 (không qua PPA) | toàn bộ | 0 | 64–93 KB |
| 1280 x 720 | 11/16 | 1862 x 1048 | khoảng 3 % | 45 KB |
| 1024 x 576 | 9/16 | 1822 x 1024 | khoảng 5 % | 31 KB, stream 4,8 khung/s, 1,25 Mbit/s |
| 720 x 480 | 8/16 | 1440 x 960 | **25 % chiều ngang** (ảnh 3:2), 11 % chiều dọc | 18 KB |
| 640 x 360 | 6/16 | 1708 x 960 | khoảng 11 % | 15 KB, push 5 khung/s, 0,55–0,63 Mbit/s |

Kích thước khung phụ thuộc cảnh (ảnh chụp trong văn phòng có đèn trần). Với push qua Internet, upload của mạng xưởng phải lớn hơn cột cuối; 640x360 hoặc 1024x576 là đủ để xem.

**Đổi kích thước khi camera đang chạy:**
1. Các stream đang mở và phần push kết thúc.
2. Thiết bị đóng, rồi mở lại với buffer mới (khoảng 0,1 s).
3. Server HTTP vẫn chạy trong suốt quá trình; push tự kết nối lại.

**Lật ảnh:** cũng do PPA làm, nên không đụng tới bit mirror của cảm biến (bit đó làm lệch thứ tự Bayer mà ISP dùng). Ở 1920x1080, lật ảnh tốn thêm một lượt PPA trên toàn ảnh.

### 3b. Push

Mỗi phiên push gồm hai bước:
1. **GET thử tới đúng địa chỉ đã cài.**
   - go2rtc trả 404 (vì không có `src`) khi login đúng, và 401 khi sai.
   - Bước này cần vì với POST, lỗi 401 chỉ lộ ra sau khi đã gửi body.
2. **Một POST không bao giờ kết thúc** tới `/api/stream.mjpeg?dst=<tên stream>`.
   - Cách go2rtc 1.9.14 nhận (`internal/mjpeg/mjpeg.go:130-191`): POST vào đường dẫn đó trở thành nguồn của stream `dst`.
   - Cách go2rtc tách khung (`pkg/magic/mjpeg/producer.go:47`): tìm chỗ một JPEG kết thúc và JPEG sau bắt đầu (`FF D9 FF D8`). Vì vậy firmware gửi các JPEG nối liền, không có header multipart.
   - Độ dài không biết trước nên dùng chunked transfer encoding. Firmware tự đóng khung chunk, vì `esp_http_client` chỉ đặt header rồi gửi byte thô.

Các điểm khác:
- **HTTPS:** chứng chỉ server được kiểm bằng bộ chứng chỉ gốc của ESP-IDF (`esp_crt_bundle`), nên phải là chứng chỉ của CA công cộng.
- **Phiên kết thúc khi:** đổi cài đặt, tắt camera, đổi kích thước ảnh, mất Wi-Fi, hoặc ghi lỗi. Lỗi thì thử lại sau 5 s, gấp đôi mỗi lần, tối đa 60 s.

## 4. Kết quả đo trên board

Đo ngày 2026-10-13, không có slave EtherCAT, Wi-Fi "OpenWrt" ở -61 dBm.

| Mục | Kết quả |
|---|---|
| Bật camera | Thấy cảm biến, chuyển sang Ready ngay khi Wi-Fi có IP |
| Snapshot từ PC | 1920x1080, 66–69 KB. Khoảng 0,9 s khi cảm biến đang dừng, 1,6 s khi đang có một stream khác |
| Stream 25 s, 1 người xem | 4,8 khung/s, khoảng 2–3 Mbit/s, khoảng 64 KB một khung |
| CPU khi stream | Core 0: 4–10 %. Core 1: 0 % |
| PSRAM | Khoảng 12 MB khi camera bật ở 1920x1080 (2 × 4 MB RGB565 + JPEG); ít hơn ở kích thước nhỏ; trả lại khi tắt |
| RAM nội trống | 84,8 KB khi Ready ngay sau khởi động. 56–58 KB trong và sau khi stream 1920x1080, thấp nhất 54,3 KB; 61 KB khi stream 1024x576; 64 KB khi push 640x360 qua HTTP; 76 KB sau một lần thử HTTPS. Khối liền lớn nhất 31 KB ở mọi trạng thái |
| Stack còn trống | `camera` 3,8 KB (của 8 KB), `cam_http*` khoảng 2,3 KB (của 4 KB), `httpd` 3 KB (của 4 KB), `isp_task` 1,1 KB (của 4 KB), `cam_push` 5,3 KB (của 8 KB, sau các lần bắt tay TLS) |

So sánh RAM nội: trước khi có camera, chạy Wi-Fi còn khoảng 105 KB trống (CLAUDE.md, 2026-10-11). Chưa đo lại mức này với camera tắt ngay từ lúc khởi động.

**Thử push trên bàn (2026-10-13)**

Thay cho go2rtc là một script Python trên PC trong cùng LAN. Script làm đúng các phần board dựa vào: Basic auth, GET không có `src` trả 404, nhận body chunked, tách khung tại `FF D9 FF D8`.

| Trường hợp | Kết quả |
|---|---|
| Mật khẩu sai | `Wrong user or password (HTTP 401)`, thử lại sau 5 s, 10 s, ... |
| Đúng | `Sending`, script nhận 5 khung/s, 0,55–0,63 Mbit/s ở 640x360 |
| `dst=` sai tên | `No stream of that name (dst=) on the server (HTTP 404)` |
| HTTPS với chứng chỉ tự ký | `Server certificate not trusted (needs a public CA)` (mbedTLS -0x2700) |
| Đổi địa chỉ khi đang gửi | Phiên cũ kết thúc gọn (chunk cuối), phiên mới bắt đầu ngay |

**Chưa thử:** go2rtc thật, HTTPS với chứng chỉ thật, và đường Internet qua DDNS.

**Sự cố đã gặp**
- Lần chạy đầu, task `camera` với stack 4 KB bị tràn khi khởi tạo cảm biến, `esp_video` và IPA (panic "Stack protection fault"). Đã tăng lên 8 KB. Trên P4, vùng lưu FPU và hardware loop của task cũng được cắt từ chính stack đó.
- PPA từ chối buffer đích không căn theo cache line. Đã sửa bằng cách cấp buffer theo hướng OUTPUT của bộ cấp phát JPEG.

**Chất lượng ảnh**
- Ảnh bị **lật trái/phải**: chữ trên áo đọc ngược.
  - `CONFIG_CAMERA_HMIRROR=y` lật ảnh bằng PPA (mục 3a), nhưng **chưa thử**.
- Màu hơi nhạt và ngả xanh. Đây là file IPA mặc định của hãng, chưa tinh chỉnh.

## 5. Cài trên TrueNAS SCALE (push qua DDNS)

```
HMI (xưởng) --HTTP(S) POST qua Internet--> DDNS:8556 --router--> ingest (go2rtc)
                                                                   | 127.0.0.1 (chung mạng "net")
                                                              go2rtc (LAN) --> recorder --> MP4
```

App gồm một file YAML (`server/camera/ehmi-camera.yaml`) và một thư mục trên NAS. Mọi container dùng image chính thức `alexxit/go2rtc:1.9.14`, không cần build image riêng.

**Bốn service:**
- **`net`:** giữ mạng và các port, không làm gì khác. Ba service còn lại nhập vào mạng của nó (`network_mode: service:net`), nên gọi nhau qua 127.0.0.1.
  - Nó không bao giờ khởi động lại. Vì vậy khi một service khác khởi động lại, các service còn lại không mất mạng.
- **`ingest`:** go2rtc **duy nhất** mở ra Internet.
  - Có user/mật khẩu.
  - Chỉ bật một đường dẫn, `/api/stream.mjpeg` (`allow_paths`). Trang web, phần sửa stream và nguồn `exec` (chạy được lệnh trên NAS) không tồn tại trên cổng này.
- **`go2rtc`:** chỉ trong LAN.
  - Lấy hình từ `ingest` qua 127.0.0.1. go2rtc không đòi login với kết nối loopback (`internal/api/api.go`, `isLoopback`), nên mật khẩu chỉ nằm một chỗ.
  - Có trang xem `http://<NAS>:1984` và RTSP, và đưa H.264 cho bộ ghi.
- **`recorder`:** ghi MP4 10 phút một file.

**Thư mục trên NAS** (của người dùng hiện tại: `/mnt/FAMILY/TUAN/ESP32CAM`, SMB `\\192.168.31.254\tuan\ESP32CAM`):

| Đường dẫn | Nội dung |
|---|---|
| `config/ingest/go2rtc.yaml` | Phía Internet, có login (`server/camera/config/ingest/`) |
| `config/lan/go2rtc.yaml` | Phía LAN (`server/camera/config/lan/`) |
| `recordings/` | Video `ehmi-YYYYMMDD-HHMMSS.mp4` |
| `ehmi-camera.yaml` | Bản sao file YAML của app |

Sửa các file cấu hình qua SMB, rồi khởi động lại app.

### 5.1 Chuẩn bị

1. **DDNS:** đã có (ví dụ `nha.ddns.net`).
2. **Router ở nhà:** forward port TCP 8556 tới IP của NAS, port 8556.
   - Chỉ port này. Không forward 1984 hay 8554.
3. **Thư mục:** chép `server/camera/config/` vào thư mục app trên NAS, và tạo thư mục `recordings/`.
4. **Mật khẩu:** đổi `password` trong `config/ingest/go2rtc.yaml` thành một chuỗi dài, chỉ chữ và số.

### 5.2 Cài app

1. Vào **Apps › Discover Apps › ⋮ › Install via YAML**.
2. Tên: `ehmi-camera`.
3. Dán nội dung `server/camera/ehmi-camera.yaml`.
4. Nếu thư mục app khác `/mnt/FAMILY/TUAN/ESP32CAM` thì sửa ba dòng `volumes`. Đổi `TZ`, `KEEP_DAYS` hoặc port 8556 nếu cần.

### 5.3 Trên HMI

1. Bật **Camera**, chọn **Picture size**. Qua Internet nên dùng 1024x576 hoặc 640x360.
2. Ở ô **Send to server**:
   - **Server:** `http://nha.ddns.net:8556/api/stream.mjpeg?dst=ehmi`, hoặc `https://...` nếu có HTTPS (mục 5.4).
   - **User:** `ehmi`.
   - **Password:** mật khẩu trong `config/ingest/go2rtc.yaml`.
3. Bật công tắc. Thẻ phải chuyển sang **Sending**.
4. Mở `http://<NAS>:1984` trong LAN ở nhà: stream `ehmi` là hình trực tiếp. VLC mở được `rtsp://<NAS>:8554/ehmi`.
5. Video nằm ở `recordings/ehmi-YYYYMMDD-HHMMSS.mp4`:
   - Mỗi file 10 phút, bắt đầu đúng mốc 10 phút của đồng hồ.
   - Định dạng MP4 phân mảnh, nên file vẫn xem được khi bị ngắt giữa chừng.
   - File cũ hơn `KEEP_DAYS` bị xoá mỗi giờ.

### 5.4 HTTPS (nên dùng)

Với `http://`, mật khẩu (Basic auth) và hình đi qua Internet **không mã hoá**. Dùng `http://` để thử cho nhanh, rồi chuyển sang HTTPS.

Cách 1 là **reverse proxy có Let's Encrypt** (ví dụ app Nginx Proxy Manager trên TrueNAS, nếu đã dùng nó cho DDNS):
1. Tạo Proxy Host cho tên DDNS, trỏ tới `http://<IP NAS>:8556`, xin chứng chỉ Let's Encrypt.
2. Router forward 443 (hoặc port của proxy) thay cho 8556.
3. Ở tab **Advanced** của Proxy Host, bắt buộc thêm các dòng sau. Thiếu chúng thì nginx giữ lại body vô tận của push, và HMI báo lỗi 413/502/504 hoặc treo ở Sending mà NAS không có hình:
   ```
   proxy_request_buffering off;
   proxy_buffering off;
   client_max_body_size 0;
   proxy_http_version 1.1;
   proxy_read_timeout 1d;
   proxy_send_timeout 1d;
   ```
4. Trên HMI, Server là `https://nha.ddns.net/api/stream.mjpeg?dst=ehmi`.

Cách 2 là để go2rtc tự chạy TLS: trong `config/ingest/go2rtc.yaml` thêm `tls_listen: ":8443"`, `tls_cert` và `tls_key` (file chứng chỉ, ví dụ lấy từ ACME của TrueNAS, chép vào cùng thư mục). Publish 8443 ở service `net` và forward 8443 thay cho 8556.

Cả hai cách đều **chưa thử**. HMI chỉ chấp nhận chứng chỉ của CA công cộng; chứng chỉ tự ký thì bị báo `Server certificate not trusted`.

### 5.5 Khi NAS ở cùng LAN với HMI

Không cần `ingest` và push:
1. Trong `config/lan/go2rtc.yaml`, cho dòng `ehmi:` lấy thẳng từ HMI (dòng Stream ở Settings › Camera).
2. Có thể xoá service `ingest` khỏi YAML, và bỏ port 8556 ở `net`.
3. Đặt IP cố định (DHCP reservation) cho HMI.
   - Hostname là `ethercat-hmi`.
   - MAC là MAC Wi-Fi của chip ESP32-C6, khác MAC Ethernet của P4. Xem trong danh sách DHCP của router.

### 5.6 Ghi chú

- **Chạy liên tục khi có ghi hình:** service `recorder` lúc nào cũng xem. Khi push đang bật thì HMI vốn đã gửi liên tục.
- **Tải trên NAS:** go2rtc chuyển MJPEG sang H.264 bằng CPU của NAS (ffmpeg, libx264). Chưa đo tải CPU của NAS, cũng chưa đo dung lượng video mỗi ngày.
- **Khi không có hình:** camera tắt, push tắt hoặc mất kết nối thì `recorder` ghi log "stream not available" và thử lại sau 10 s.
- **Image:** `alexxit/go2rtc:1.9.14` (bản phát hành 2026-01-19 trên Docker Hub), có sẵn ffmpeg. Entrypoint là tini, lệnh mặc định `go2rtc -config /config/go2rtc.yaml` (`docker/Dockerfile:51-55`).
  - Đã kiểm trong mã nguồn bản này: nhận POST MJPEG, stream rỗng (`internal/streams/stream.go:43`), `allow_paths` và Basic auth (`internal/api/api.go`), loopback không cần login, và listener tắt khi địa chỉ rỗng (`rtsp.go`, `srtp.go`, `webrtc.go`).
- **Chưa xác nhận:** custom app của TrueNAS có chạy `build:` trong YAML không. App này không cần build nên tránh được câu hỏi đó.

## 6. Còn mở

1. **Ảnh hưởng tới EtherCAT khi stream:** chưa đo khi bus ở OP với 10 slave. Cần chạy `cam on`, mở stream hoặc push, rồi theo dõi overrun, wake late và RTT như các lần đo Wi-Fi trước (CLAUDE.md).
   - Rủi ro thứ nhất: lưu lượng Wi-Fi qua các task esp_hosted ở priority 23 trên core 0.
   - Rủi ro thứ hai: băng thông PSRAM. Theo ước tính, ISP ghi khoảng 124 MB/s ở 1080p 30 fps (1920 × 1080 × 2 byte × 30), cạnh màn hình và code XIP từ PSRAM.
2. **RAM nội** giảm khoảng 27 KB trong lúc stream 1920x1080 (khoảng 24 KB ở 1024x576). Nghi là buffer TCP và hàng đợi gửi của esp_hosted, chưa xác minh. Nếu cần lấy lại RAM, có thể giảm `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` hoặc `CAMERA_MAX_VIEWERS`.
3. **Màn hình HMI khi camera chạy:** chưa xem màn hình có nháy hay giật không.
4. **Lật ảnh và màu:** chưa thử `CAMERA_HMIRROR`, chưa tinh chỉnh IPA.
5. **Phân mảnh PSRAM:** sau vài lần đổi kích thước, khối PSRAM liền lớn nhất còn 8,9 MB (trống 18,9 MB). 1920x1080 cần hai khối 4 MB của `esp_video` cùng các khối JPEG. Chưa thấy lỗi, nhưng nên theo dõi `mem` nếu đổi kích thước nhiều lần.
6. **Phía TrueNAS** chưa chạy thật: go2rtc, HTTPS qua proxy, DDNS (mục 5).
