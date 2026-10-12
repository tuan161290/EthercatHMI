# Theme HMI: Windows 11 Fluent (sáng)

**Quyết định (2026-10-03):** người dùng chọn theme giống Microsoft Store / Windows 11: nền gradient mờ kiểu Mica, bố cục chuẩn Windows, tối giản, sạch. Theme này thay cho theme tối phẳng trước đó.

Mockup: https://claude.ai/artifact/GgcWLcopMKjc9ibk3oUBFo (riêng tư). Mỗi artboard có tweak `accent` để thử màu nhấn.

## 1. Bố cục 1024×600

| Vùng | Kích thước | Nội dung |
|---|---|---|
| Thanh tiêu đề | 1024×48, trong suốt trên nền | Ô app + tên máy · chip trạng thái ở giữa (chế độ, trạng thái máy, EtherCAT) · giờ trên ngày (như taskbar Windows) · nút người dùng (cấp quyền) |
| Rail điều hướng | 76×552, bên trái | Main, Manual, IO monitor, Alarms (badge số alarm); **Settings ở đáy rail**, như NavigationView của Windows |
| Lớp nội dung | 948×552 | Nền trắng 50 %, viền mảnh trên và trái, bo góc trên-trái 8 px. Padding 14/20/16/24 px |
| Hàng tiêu đề trang | cao 40 | Title 28 px, pill lọc / trang con cùng hàng. Trang con của Settings dùng breadcrumb "Settings › Time" |

- Mục rail đang chọn: nền trắng 70 %, vạch accent 3×16 px ở mép trái, icon màu accent, chữ đậm.
- Trang con của một mục (Teaching, các trang Settings) vẫn chọn mục cha trên rail.
- Nhãn viết hoa đầu câu (sentence case), không viết hoa toàn bộ, không giãn chữ. Chỉ định danh giữ nguyên: `AX1`, `X0.0`, `START_PB`, mã alarm.

## 2. Token màu

| Token | Giá trị |
|---|---|
| Nền | Mockup: gradient hồng cam / xanh mint / xanh lam nhạt trên `#F3F4F4`. **Firmware: xám đặc `#CCCCCC`**, không gradient, không dithering (người dùng chốt 2026-10-10). Mức xám là `CONFIG_HMI_BG_LEVEL` = 80 % của trắng |
| Chữ chính / phụ / disabled | `#1B1B1B` / `#5D5D5D` / `#A0A0A0` |
| Accent | `#107C41` (xanh lá như ảnh tham chiếu); tint = accent 10 % |
| Thẻ (card) | **trắng đặc `#FFFFFF`**, viền 1 px đen 6 %, bo 8 px |
| Control (nút, ô nhập, chip, mục rail đang chọn) | `#FBFBFB` (nhạt hơn thẻ trắng một bậc), viền đen 8 %, mép dưới đậm hơn: đen 16 % (ô nhập 45 %, nút accent 40 %), bo 4 px |
| Đường chia | đen 6 % |
| Success | chữ `#0F7B0F`, nền `#DFF6DD` |
| Caution | chữ `#9D5D00`, nền `#FFF4CE` |
| Critical | `#C42B1C`, nền `#FDE7E9` |
| Nút Start / Stop | nền `#0F7B0F` / `#C42B1C`, chữ trắng; không đổi theo accent |

Màu trạng thái lấy theo bảng màu hệ thống của WinUI (SystemFillColorSuccess/Caution/Critical).

## 3. Chữ

- **Open Sans** (OFL), hai độ đậm 400 và 600.
  - Segoe UI Variable của Windows là font độc quyền, không được nhúng vào firmware.
  - Open Sans và Segoe UI cùng một người thiết kế (Steve Matteson), nên đây là font mở có dáng gần nhất.
- **Thang chữ cho màn chạm** (người dùng yêu cầu 2026-10-11: chữ trên màn 7" khoảng 170 ppi quá nhỏ). Đơn vị px, dùng token trong `hmi_internal.h`, không viết số cứng:

  | Token | px | Dùng cho |
  |---|---|---|
  | `HMI_FS_TINY` | 13 | số trên badge |
  | `HMI_FS_SMALL` | 15 | chú thích, đơn vị, tag, tiêu đề cột |
  | `HMI_FS_BODY` | 18 | nhãn, giá trị, chữ trên nút, dòng danh sách |
  | `HMI_FS_LARGE` | 20 | giá trị cần nhấn mạnh, tên tile |
  | `HMI_FS_HEAD` | 24 | tiêu đề hộp thoại, giá trị lớn, phím bàn phím |
  | `HMI_TITLE_PX` | 28 | tiêu đề trang |
  | `HMI_FS_BIG` | 34 | KPI |
  | `HMI_FS_HUGE` | 44 | vị trí trục trên trang Jog |

  Trước đây thang chữ là Caption 12, Body 14, tiêu đề trang 24.
- **Kích thước chạm:**
  - `HMI_TOUCH_H` = 52 px (khoảng 8 mm) cho nút, ô nhập và segmented control;
  - `HMI_ROW_H` = 56 px cho dòng danh sách;
  - icon trong nút 20 px;
  - công tắc 52×28 px, núm thanh trượt 28 px;
  - nút Jog và Start/Stop lớn hơn nữa.
- Chọn trục (trang Jog, trang Servo) là một ô lớn mở danh sách cuộn, không phải lưới nhiều nút nhỏ.
- Settings là lưới tile 4×3.
- Số luôn dùng chữ số đều bề rộng (tabular). Chữ số mặc định của Open Sans đã đều bề rộng, nên LVGL không cần bật OpenType feature.
- Kiểm tra bố cục bằng bản giả lập trên PC (`firmware/tools/hmi_sim`, `SETUP.md` mục 7b) trước khi nạp.
- Bộ ký tự cần cho lv_font_conv: ASCII cộng `· … — › − ° µ ±`.

## 4. Icon

- Mockup dùng icon nét 1,5 px vẽ tay.
- Firmware sẽ dùng **Fluent UI System Icons** (MIT, `github.com/microsoft/fluentui-system-icons`):
  - chuyển thành font icon 20 và 24 px bằng lv_font_conv;
  - kiểu Regular cho mục thường, Filled cho mục đang chọn.

## 5. Chuyển sang LVGL 9 (ESP32-P4, RGB565)

- **Ba lớp như Windows 11** (người dùng chốt 2026-10-10):
  1. **Nền:** một màu xám đặc `#CCCCCC` cho cả title bar, rail và vùng trang.
  2. **Thẻ:** trắng đặc.
  3. **Control:** `#FBFBFB`, có viền và mép dưới đậm hơn.
- **Lịch sử nền (2026-10-10):**
  - Trước đây nền là ảnh gradient tính lúc khởi động, dithering Bayer xuống RGB565.
  - Người dùng thấy vùng xám bị nhấp nháy, còn thẻ trắng thì không. Giả thuyết (chưa kiểm chứng): pattern dithering 1 pixel gặp kiểu đảo cực tính pixel của LCD.
  - Nay đã bỏ hẳn ảnh nền và dithering, tiết kiệm 1,2 MB PSRAM và khoảng 1 s lúc khởi động. Vẽ lại cũng nhẹ hơn: tô một màu thay vì chép ảnh từ PSRAM.
- **Thẻ và control đặc** (`bg_opa` `LV_OPA_COVER`), nên vẽ lại không phải trộn alpha với nền.
- **Viền:** `border_width` 1 với `border_opa`.
- **Mép dưới của control** (ControlElevationBorder của Windows 11) do `hmi_elevation()` (`hmi_theme.c`) vẽ:
  - Là một nét 1 px ở đoạn thẳng của cạnh dưới, vẽ bên trong object trong `LV_EVENT_DRAW_MAIN_END`, nên khung cha không cắt mất như shadow hay outline.
  - Không vẽ khi đang nhấn (control phẳng xuống) và khi nền object trong suốt (segment chưa chọn).
  - Độ đậm: nút và control 16 %, nút accent 40 %, ô nhập 45 % (`hmi_field_style()`, dùng ở Settings › Servo và Time).
- **Không dùng shadow** (tốn CPU trong LVGL). Dialog có thể dùng shadow nhỏ (width ≤ 16).
- **Bo góc:** 4 px cho control, 8 px cho thẻ, `LV_RADIUS_CIRCLE` cho pill và chip.
- **Vùng chạm:** tối thiểu 40 px; 44 px trở lên cho jog, Start/Stop và chọn trục.
- **Huỷ chạm khi trượt ra** (như Windows, người dùng yêu cầu 2026-10-11; `hmi_touch_slide_cancel()` trong `hmi_widgets.c`):
  - Chạm nhầm một nút rồi kéo ngón tay ra xa quá 12 px thì nhấc tay ra **không** bấm nút đó, và cũng không bấm nút mà ngón tay trượt vào.
  - Nút giữ-để-chạy (jog, Go to) dừng ngay khi ngón tay ra khỏi nút.
  - Không áp dụng cho slider và khi đang cuộn danh sách.
  - Lý do cần làm riêng: LVGL mặc định bật `LV_OBJ_FLAG_PRESS_LOCK`, nên nút vẫn được bấm khi nhấc tay ở bất cứ đâu.
- **Dim và tắt màn hình** (người dùng yêu cầu 2026-10-11; `hmi_display.c`):
  - Sau một khoảng không chạm, đèn nền giảm còn 20 % độ sáng đã đặt (`CONFIG_HMI_DIM_LEVEL`). Sau một khoảng nữa thì tắt hẳn.
  - Hai thời gian **Dim after** và **Turn off after** đặt ở Settings › Display, thẻ Screen. Cả hai đều tính từ lần chạm cuối. Các lựa chọn: Never, 30 s, 1/2/5/10/15/30 min, 1 hour.
  - Mặc định là 2 min và 10 min (`CONFIG_HMI_DIM_AFTER_S`, `CONFIG_HMI_OFF_AFTER_S`). Người dùng đã đặt 5 min và 15 min trên máy ngày 2026-10-11.
  - Chạm vào màn hình đang dim hoặc đang tắt chỉ làm màn hình sáng lại, **không bấm** nút nằm dưới ngón tay. Cách làm: hàm đọc cảm ứng được bọc lại và báo "nhả" cho LVGL cho đến khi nhấc tay.
  - Alarm mới chưa xác nhận cũng làm màn hình sáng lại.
  - Khi màn hình tắt, giao diện **tạm dừng** (người dùng yêu cầu 2026-10-11):
    - Mọi timer của LVGL dừng lại: cập nhật trang, thanh tiêu đề, touch log, animation. Vì không có gì thay đổi nên LVGL không vẽ, và task LVGL gần như ngủ.
    - Chỉ timer màn hình (đếm giờ và kiểm tra alarm, 4 lần/giây) và phần đọc cảm ứng (ngắt GT911) còn chạy.
    - Khi sáng lại, các timer chạy tiếp ngay và cả màn hình được vẽ lại.
    - Phần này chưa chạy thử trên board.
  - Nếu không tìm thấy bộ điều khiển cảm ứng lúc khởi động thì màn hình không bao giờ dim.
  - Đã chạy trên board ngày 2026-10-11:
    - màn hình dim đúng 5 s sau lần chạm cuối;
    - lần chạm làm sáng lại không xuất hiện trong `touchlog`, và lần chạm sau đó bấm nút bình thường.
- **Theme:** tắt theme mặc định (Material) của LVGL (`CONFIG_LV_USE_THEME_DEFAULT=n`). Mọi object đều được style trong `components/hmi` bằng các style dùng chung theo token ở mục 2.

## 6. Firmware (code từ 2026-10-03; khởi động trên board không lỗi từ 2026-10-10, chưa ai kiểm tra hình và cảm ứng trên màn hình)

| File | Nội dung |
|---|---|
| `components/board/board_display.c` | Màn hình DSI + JD9165 (bảng init New và Old chép từ BSP của hãng), đèn nền LEDC, cảm ứng GT911 có điều khiển RST/INT |
| `components/hmi/hmi.c` | Khởi động esp_lvgl_port (task LVGL ở core 0, 1 luồng vẽ, bộ đệm 48 dòng trong RAM nội); snapshot trạng thái EtherCAT kiểu seqlock |
| `hmi_theme.c` | Font Open Sans qua TinyTTF, style dùng chung, hàm tạo widget |
| `hmi_icon.c` | Icon nét vẽ bằng API vẽ của LVGL, theo toạ độ SVG của mockup |
| `hmi_shell.c` | Thanh tiêu đề, rail điều hướng, vùng trang |
| `hmi_page_main.c` | Trang Main |
| `hmi_page_settings.c` | Trang Settings: lưới 10 tile, mỗi tile có icon, tên, mô tả ngắn và cấp người dùng (thay cho các dòng của mockup `Setting.dc.html`, 2026-10-11), kèm breadcrumb "Settings › …". Trang **Display** (trước đây tên là System): thanh trượt độ sáng và cân bằng màu R/G/B (thẻ Display); thời gian Dim after và Turn off after (thẻ Screen); ô mẫu màu (thang xám, RGB/CMY). Các mục chưa làm mở khung tạm. |
| `hmi_display.c` | Độ sáng đèn nền, cân bằng màu, dim và tắt màn hình (mục 5). Lưu trong machine store, khoá `display` (9 byte, phiên bản 3), ghi được cả khi bus đang OP. Mỗi kênh màu có một bảng tra RGB565, áp lên từng dải 48 dòng tại `LV_EVENT_FLUSH_START`. Khi R/G/B khác 100 %, mỗi khung toàn màn hình tốn thêm khoảng 30 ms (ước lượng từ log khởi động 2026-10-10, chưa đo riêng). |
| `hmi_page_ethercat.c` | Settings › EtherCAT theo mockup `EthercatSetting.dc.html`: bảng topology (đã lưu so với bus, hoặc kết quả scan), banner trạng thái, Scan bus / Re-check / Accept, có hộp thoại xác nhận liệt kê thay đổi. Khung bên phải có thêm thời gian chu kỳ: Cycle time (kèm số overrun), Jitter (trễ lớn nhất của một chu kỳ so với hạn, trong 1 s gần nhất), Frame round trip (trung bình và lớn nhất). Hiển thị theo µs đến 100 µs, lớn hơn thì theo ms (`hmi_fmt_us()`) |
| `hmi_page_manual.c` | Manual › Jog theo mockup `Manual.dc.html`: chọn trục, vị trí (rev) và trạng thái drive, Servo on/off, Home, Alarm reset, Jog Step/Continuous (giữ để chạy), ba mức tốc độ (High lấy từ Settings › Servo), Move to (bàn phím số). Pill Jog / Teaching / Interpolation chuyển view. Actuators, Homing chưa có (pill mờ) |
| `hmi_page_teach.c` | Manual › Teaching theo `Teaching.dc.html`: bảng điểm của nhóm 3 trục, điểm và vị trí hiện tại, Teach (xác nhận), Go to (giữ để chạy), Add point, Save points, jog từng bước |
| `hmi_page_interp.c` | Manual › Interpolation (2026-10-12, không có mockup): chọn trục X/Y bằng `hmi_dropdown()`, đường Line / Arc CW / Arc CCW, Speed và Ramp, Start/Stop, đồ thị XY trên `lv_canvas` 288 px (đường dự kiến màu xám, vị trí thật màu accent), kết quả contour error và lag |
| `hmi_page_io.c` | IO monitor theo `IoMonitor.dc.html`: Digital I/O (đèn từng kênh X/Y), Analog (giá trị INT thô), EtherCAT (module và Device Status của coupler), Force mode có xác nhận |
| `hmi_page_alarms.c` | Alarms theo `Alarm.dc.html`: Active / History, dòng alarm với ô mức độ, giờ, trạng thái, Acknowledge; khung chi tiết với nguyên nhân và cách xử lý; Acknowledge all, Reset. Export (SD) chưa có |
| `hmi_page_time.c` | Settings › Time theo `TimeSetting.dc.html`: timer T01–T08 (bàn phím số, Save), ngày giờ và múi giờ (bộ chọn kiểu cuộn), nguồn đồng hồ |
| `hmi_page_servo.c` | Settings › Servo, nội dung theo `ServoSetting.dc.html` nhưng bố cục là **một danh sách dọc cuộn được** (người dùng chốt 2026-10-10, thay cho 4 thẻ 2×2): tên bên trái; object, ô giá trị và đơn vị bên phải; đường kẻ ngăn giữa các nhóm Motion (Max speed, Acc/Dec time, Jog high) / Limits / Homing / Mechanical (Pulse/Rev, Distance/Rev, encoder, chiều quay). Đọc/ghi object qua job SDO, Read / Write to drive / Save |
| `hmi_axis.c` | Đơn vị hiển thị của từng trục theo Unit (mm, deg) và Distance/Rev: mm và mm/s, hoặc deg và deg/s; chưa đặt thì rev và rpm. Gửi lệnh trục với Max speed và Acc/Dec time của Settings › Servo |
| `hmi_widgets.c` | Segmented control, pill, tag, đèn trạng thái, công tắc, hộp thoại xác nhận, bàn phím số, bộ chọn kiểu cuộn (danh sách, ngày giờ), **ComboBox thả xuống** `hmi_dropdown()` (`lv_dropdown`, cao `HMI_TOUCH_H`, mũi tên chevron, danh sách mở ngay dưới với mỗi dòng 52 px, tối đa 6 dòng rồi cuộn, dòng đang chọn tô màu accent), thẻ thông tin, **bàn phím chữ** `hmi_text_input()` (ký tự ASCII in được; phím đặc biệt ghi bằng chữ Del, Shift, #+=, abc vì Open Sans không có ký hiệu mũi tên; Shift giữ cho một chữ; phím gõ khi nhấc tay nên trượt ra là huỷ; ô mật khẩu có nút Show) |
| `hmi_page_info.c` | Settings › System info: RAM nội, PSRAM (trống, thấp nhất từ lúc khởi động, khối liền lớn nhất) và kho cấu hình trên flash, cùng firmware, chip, MAC, uptime, Wi-Fi, EtherCAT; tự cập nhật mỗi giây |
| `hmi_page_network.c` | Settings › Network (chưa có mockup, làm theo kiểu Settings › Time, 2026-10-11). Thẻ Wi-Fi bên trái: công tắc, trạng thái (đã kết nối kèm IP và cường độ sóng, đang kết nối, lỗi và lý do), danh sách mạng quét được (mạnh nhất lên đầu), chạm vào một mạng để nhập mật khẩu, nút Hidden network và Forget network. Thẻ Internet time bên phải: công tắc, máy chủ NTP, lần đồng bộ gần nhất, giờ hiện tại, nút Sync now. Mỗi lần mở trang thì tự quét mạng một lần |
| `hmi_screenshot.c` | Lệnh console `screenshot`: in frame buffer (nén PackBits, base64) để `tools/screenshot.py` ghép thành ảnh PNG |
| `hmi_page_todo.c` | Khung tạm cho Settings › Recipe, User & password, Maintenance, Backup / restore (chưa có mockup) |
| `hmi_lv_mem.c` | Heap của LVGL đặt trong PSRAM trước, để RAM nội cho EtherCAT |

- **Khác với mục 4:** icon chưa dùng font Fluent UI System Icons (cần công cụ chuyển font chạy bằng Node.js, máy chưa có). Hiện icon được vẽ bằng đường và cung tròn, cùng hình dạng như trong mockup.
- **Dữ liệu mẫu:** chưa có logic máy, nên trang Main hiện số liệu của mockup và có nhãn "demo data" (tắt bằng `CONFIG_HMI_DEMO_DATA`). Chip EtherCAT trên thanh tiêu đề là trạng thái thật.
- **Chưa có:** đăng nhập (cấp người dùng chỉ là chữ), Settings › Recipe / User & password / Maintenance / Backup / restore, Manual › Actuators / Homing.
- **RAM:** trạng thái các trang nằm trong PSRAM (`EXT_RAM_BSS_ATTR` hoặc `lv_malloc`). Bộ đệm vẽ 96 KB cần một khối RAM nội liên tục, nên HMI khởi động trước các task EtherCAT (`main.c`).
- **Font:** Open Sans không có mũi tên (U+2192), nên chữ trên màn hình dùng "to" hoặc "was" thay cho "→". Các ký tự −, —, ·, ±, … và › đều có (đã kiểm tra bảng cmap của file TTF ngày 2026-10-10).
- **Không đặt code LVGL vào IRAM:** tuỳ chọn `LV_ATTRIBUTE_FAST_MEM_USE_IRAM` của demo làm tràn RAM nội khoảng 32 KB.
- Hướng dẫn thử trên board: `SETUP.md` mục 7b.

## 7. Lưu ý

- **Accent xanh lá gần màu "OK / đang chạy"** (`#107C41` so với `#0F7B0F`). Vì vậy trạng thái luôn có icon hoặc chữ đi kèm, không chỉ dựa vào màu. Nếu muốn tách hẳn, đổi accent sang xanh dương mặc định của Windows `#005FB8` (có sẵn trong tweak).
- Nền sáng chói hơn theme tối khi xưởng thiếu sáng. Có thể giảm độ sáng đèn nền ở Settings › Display hoặc bằng lệnh console `display bright <5-100>`.
