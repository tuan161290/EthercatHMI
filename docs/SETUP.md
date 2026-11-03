# SETUP.md — Môi trường phát triển, build, nạp và smoke test (Windows 10)

Nguồn và quy ước trích dẫn giống `docs/BOARD.md`.

## 0. Trạng thái hiện tại (đã cài và kiểm tra 2026-10-03)

**Từ 2026-11-03 dự án chỉ build bằng ESP-IDF v6.1 và LVGL 9.6** (lý do ở cuối mục này). IDF v5.5.4 chỉ còn dùng cho các demo của hãng ở mục 5 và 6. Các bảng dưới đây ghi những gì đã cài trên từng máy.

| Thành phần | Trạng thái |
|---|---|
| Git for Windows 2.55 | `C:\Program Files\Git` (cài bằng winget) |
| ESP-IDF Installation Manager (EIM) 0.19 | lệnh `eim` (winget `Espressif.EIM-CLI`) |
| ESP-IDF v5.5.4 (chỉ cho demo của hãng, mục 5 và 6) | `C:\esp\v5.5.4\esp-idf`; tools + Python 3.13 venv ở `C:\Espressif\tools` |
| **ESP-IDF v6.1** (bản dùng cho dự án từ 2026-11-03) | `C:\esp\v6.1\esp-idf` |
| VS Code extension ESP-IDF 2.3.0 | đã cài; dùng bản IDF đang chọn trong EIM |
| Thư mục build | **ngoài ownCloud**: `C:\esp\build\...` |
| Bản copy demo dùng để smoke test | `C:\esp\smoke\{v554,v61}\...` (có thể xoá) |

Thư mục dự án `C:\Users\Tuan\ownCloud\Tuan\EthercatHMI` được ownCloud đồng bộ và `LongPathsEnabled = 0` (MAX_PATH 260).
Vì vậy **luôn build ra `C:\esp\build\...`** bằng `-B`.

**Máy thứ hai (DESKTOP-NHHD9F1, cài 2026-10-10): mọi thứ nằm trên ổ D.**

| Thành phần | Vị trí |
|---|---|
| EIM 0.20 | winget `Espressif.EIM-CLI` |
| ESP-IDF v5.5.4. **Chưa có v6.1**, phải cài trước khi build dự án. | `D:\esp\v5.5.4\esp-idf` |
| Tools (chỉ target esp32p4), Python 3.12 venv, `Microsoft.v5.5.4.PowerShell_profile.ps1` | `D:\Espressif\tools` |
| Danh sách bản IDF của EIM | `C:\Espressif\tools\eim_idf.json` (file nhỏ, để nguyên ở ổ C cho `eim` và VS Code tìm thấy) |
| Thư mục dự án | `D:\OwnCloud\Tuan\EthercatHMI`. `LongPathsEnabled = 1`. `managed_components` là thư mục thường, không phải junction. |
| Thư mục build và sao lưu | `D:\esp\build\ehmi_<app>`, `D:\esp\backup` |

Lệnh cài cho dự án (cấu hình được lưu ở `D:\Espressif\eim_config.toml`; v6.1 chưa cài thử trên máy này, và tên profile `Microsoft.v6.1.PowerShell_profile.ps1` suy ra từ bản v5.5.4):
```powershell
winget install Espressif.EIM-CLI
eim install -p D:\esp -i v6.1 -t esp32p4 -n true --tool-download-folder-name D:\Espressif\dist --tool-install-folder-name D:\Espressif\tools --activation-script-path-override D:\Espressif\tools
```
**Máy thứ ba (DESKTOP-2LDQI5D, 2026-10-12):** `D:\esp` và `D:\Espressif` được chép nguyên từ máy thứ hai, không cài bằng EIM. Máy không có Python, nên venv của IDF (`pyvenv.cfg` trỏ tới `C:\Users\Tuan\...\Python312`) báo `No Python at ...`. Cách sửa: tải Python 3.12.10 bản portable (gói NuGet `python` 3.12.10 của python.org, thư mục `tools`) vào `D:\esp\python312`, rồi đổi `home`/`executable`/`command` trong `D:\Espressif\tools\python\v5.5.4\venv\pyvenv.cfg` sang đường dẫn đó (bản gốc giữ ở `pyvenv.cfg.orig-Tuan`). Phải đúng 3.12.x vì các gói trong venv được build cho cp312. Sau đó `idf.py -B D:\esp\build\ehmi build` chạy được.

**IDF v6.1 trên máy thứ ba (từ 2026-10-30):** `D:\esp\v6.1\esp-idf`, tools ở `D:\esp\tools61` (tách khỏi `D:\Espressif` của EIM, nơi vẫn giữ v5.5.4), Python gốc `D:\esp\python312`. Tải từ GitHub hay bị đứng trên mạng này: dùng `IDF_GITHUB_ASSETS=dl.espressif.com/github_assets` cho tools và `git submodule update --depth 1` cho từng submodule. Kích hoạt:
```powershell
$env:PATH = "D:\esp\python312;" + $env:PATH; $env:IDF_TOOLS_PATH = 'D:\esp\tools61'; . D:\esp\v6.1\esp-idf\export.ps1
```

Các lỗi khác gặp trên máy thứ ba (2026-10-12):
- **`idf.py` treo ngay khi chạy, không in gì, không dùng CPU:** kết nối TCP tới `127.0.0.1` của Windows bị chặn. Lúc đó FortiClient vừa được bật; chưa xác nhận chắc chắn nó là nguyên nhân. `asyncio` của Python tạo cặp socket qua localhost và đứng ở `accept()`. Kiểm tra: một `TcpListener`/`TcpClient` trên loopback trong PowerShell cũng timeout. Cách đi vòng: build bằng `ninja` trong `D:\esp\build\ehmi_general` (thư mục build đã cấu hình), nạp bằng `python -m esptool ... write-flash "@flash_args"` trong thư mục build. `idf.py monitor` và `flash.bat` vẫn cần localhost. Ngày 2026-11-03 loopback chạy được và `flash.ps1 -Action build` build xong qua `idf.py`.
- **`.component_hash ... does not exist`:** `firmware/managed_components` là thư mục thường trong ownCloud và đã được ownCloud tải về từ máy khác. ownCloud không đồng bộ file ẩn (tên bắt đầu bằng dấu chấm), nên mất `.component_hash`. Đã sửa như mục 1: chuyển thư mục cũ sang `D:\esp\managed\ehmi_from_owncloud_20261012` và tạo junction `managed_components` → `D:\esp\managed\ehmi`.
- **Simulator: `cl : Command line error D8050` trên mọi file:** `D:\esp\build\hmi_sim\vsenv.txt` được chép từ máy khác, đặt `TEMP` thành `C:\Users\Tuan\...`. `sim_build.py` giờ tự tạo lại cache khi `USERPROFILE` khác.

### Dự án ra khỏi ownCloud, sao lưu bằng git (2026-10-12)

**Cập nhật:** từ 2026-10-22 bản sao lưu git nằm trên Seafile thay cho ownCloud. Từ 2026-11-03 có thêm repo private trên GitHub: https://github.com/tuan161290/EthercatHMI. `git push` ghi vào Seafile và `D:\esp\backup`; `git push github` cập nhật GitHub. Máy khác có thể `git clone` từ GitHub, sau khi chạy `gh auth login`. App hiển thị biến tần Deye đã tách thành project riêng `D:\Projects\SolarMonitor`. Các dòng về ownCloud bên dưới là lịch sử.

ownCloud làm mất file: file ảo không đọc được khi client tắt, và file ẩn (tên bắt đầu bằng dấu chấm) không bao giờ được đồng bộ. Vì vậy dự án được chuyển ra ngoài ownCloud, và ownCloud chỉ giữ bản sao lưu git.

| Thứ | Vị trí |
|---|---|
| Thư mục làm việc (máy thứ ba) | `D:\Projects\EthercatHMI`. `firmware/managed_components` là thư mục thường, git bỏ qua. |
| Repo sao lưu trong ownCloud (remote `owncloud`) | `D:\ownCloud\Tuan\Backup\EthercatHMI.git` (repo bare, ownCloud đưa lên server) |
| Bản sao thứ hai, cùng ổ, ngoài ownCloud (remote `backup`) | `D:\esp\backup\EthercatHMI.git` |
| Git | MinGit 2.56 ở `D:\esp\git\cmd` (đã thêm vào PATH của user) |
| Bản cũ, không sửa nữa | `D:\ownCloud\Tuan\EthercatHMI` |

- Sau mỗi thay đổi: `git add -A`, `git commit -m "..."`, `git push`. Remote `owncloud` có hai push URL nên một lần `git push` ghi vào cả hai repo sao lưu.
- **Chỉ push từ một máy tại một thời điểm.** Trước khi chuyển sang máy khác, đợi ownCloud báo đồng bộ xong. Hai máy cùng push vào repo bare đang đồng bộ có thể tạo file xung đột trong `refs/`.
- Máy khác lấy code bằng `git clone D:/ownCloud/Tuan/Backup/EthercatHMI.git <thư mục ngoài ownCloud>` (đổi ổ theo máy), không làm việc trực tiếp trong ownCloud nữa.
- Không đưa vào git, chỉ để trong ownCloud: file zip gói của hãng (222 MB), `vendor/JC1060/`, `PGM/`.
- Thư mục build `D:\esp\build\ehmi` được tạo lại cho đường dẫn mới. Từ 2026-10-31 mỗi app có thư mục build riêng `D:\esp\build\ehmi_<app>` (mục 2). Khi localhost hỏng (xem trên), cấu hình bằng:
  ```powershell
  $env:PATH = "D:\esp\python312;" + $env:PATH; $env:IDF_TOOLS_PATH = 'D:\esp\tools61'; . D:\esp\v6.1\esp-idf\export.ps1
  mkdir D:\esp\build\ehmi_general; cd D:\esp\build\ehmi_general
  cmake -G Ninja -DPYTHON_DEPS_CHECKED=1 -DPYTHON=D:\esp\tools61\python_env\idf6.1_py3.12_env\Scripts\python.exe -DESP_PLATFORM=1 -DCCACHE_ENABLE=0 D:\Projects\EthercatHMI\firmware\apps\general
  ninja
  ```

Kết nối tới GitHub có lúc bị đứt giữa chừng (`error decoding response body`). EIM tự thử lại 3 lần, mỗi lần tải lại từ đầu. Lần cài này toolchain `riscv32-esp-elf` (673 MB) thành công ở lần thứ 2.

### Vì sao dùng v6.1 và LVGL 9.6 (từ 2026-11-03)

Đây là các bản phát hành mới nhất tại thời điểm 2026-11-03: ESP-IDF v6.1 (GCC 15.2, esptool 5.6), LVGL 9.6.0~1, esp_lvgl_port 2.9.0. Người dùng đã thử với servo thật rồi chọn chỉ dùng bộ này. `idf_component.yml` yêu cầu `idf >=6.1,<7.0`, nên build bằng IDF 5.5.4 sẽ dừng ngay ở bước cấu hình.

`esp_hosted` vẫn ở 2.11.7, dù registry đã có 3.0.10: C6 trên board đang chạy 2.3.0 và `net.c` sửa một bảng nội bộ của esp_hosted, nên lên bản chính mới phải thử trên board trước.

BSP của hãng vẫn không build được trên 6.x, nhưng firmware này không dùng BSP đó (`components/board` là code của dự án). Kết quả build thử demo của hãng:

| Demo | v5.5.4 | v6.1 |
|---|---|---|
| `ethernet` | build được ngay | cần thêm component `espressif/ip101` + `#include "esp_eth_phy_ip101.h"` (IDF 6 tách driver PHY ra khỏi lõi) |
| `lvgl_demo_v9` | build được sau 2 bản vá ở mục 6 | **lỗi**: BSP 5.2.3 của hãng dùng `pixel_format`, `LCD_COLOR_PIXEL_FORMAT_RGB565`, `use_dma2d` đã bị xoá; `esp-audio-player` thiếu `driver/i2s_std.h` |

## 1. Mở môi trường ESP-IDF

Trong PowerShell:
```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py --version      # ESP-IDF v6.1
```
Cảnh báo `Git version unavailable` là vô hại: IDF được cài từ bản nén, không phải git clone.

Chọn bản IDF mặc định (dùng cho VS Code extension): `eim list`, `eim select v6.1`.

**Khuyến nghị cho ownCloud:** trong ownCloud client → Settings → Edit Ignored Files, thêm `build`, `managed_components`,
`sdkconfig.old`, `.cache`. Firmware đã build ra ngoài, nhưng component manager vẫn tải `managed_components/` vào thư mục dự án khi có phụ thuộc từ registry.

**`firmware/managed_components` là junction** trỏ sang `C:\esp\managed\ehmi` (tạo ngày 2026-10-03), nên LVGL và các driver (vài nghìn file) không nằm trong vùng ownCloud. Component manager luôn tải vào `<project>/managed_components` và không có tuỳ chọn đổi chỗ. Từ 2026-10-31 project là từng app, nên thư mục này là `firmware\apps\<app>\managed_components`. Nếu dự án vẫn nằm trong thư mục đồng bộ, tạo junction trước lần build đầu:
```powershell
New-Item -ItemType Directory -Force C:\esp\managed\ehmi_general
New-Item -ItemType Junction -Path C:\Users\Tuan\ownCloud\Tuan\EthercatHMI\firmware\apps\general\managed_components -Target C:\esp\managed\ehmi_general
```

## 2. Build firmware dự án (`firmware/apps/<app>`)

Từ 2026-10-31 `firmware/` chứa nhiều app, mỗi app là một project ESP-IDF riêng trong `firmware/apps/<app>/`. Phần dùng chung nằm ngoài:
- `firmware/components/`: mọi component, kể cả `app_core` (khởi động EtherCAT, HMI, alarm, Wi-Fi, console; trước đây là `main/`);
- `firmware/sdkconfig.defaults`: cấu hình chung;
- `firmware/partitions.csv`: bảng phân vùng chung, nên nạp app khác thì NVS và dữ liệu máy (`mstore`) vẫn giữ nguyên.

App gốc là **General App** (`firmware/apps/general`): chỉ gọi `app_core_start()`, chưa có trình tự máy.

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd C:\Users\Tuan\ownCloud\Tuan\EthercatHMI\firmware\apps\general
idf.py -B C:\esp\build\ehmi_general build
idf.py -B C:\esp\build\ehmi_general -p COM5 flash monitor     # thoát monitor: Ctrl+]
```
- Cấu hình: `firmware/sdkconfig.defaults` cho mọi app, rồi `firmware/apps/<app>/sdkconfig.defaults` cho riêng app đó (ghi đè cái chung). `idf.py -B C:\esp\build\ehmi_general menuconfig` để chỉnh
  (*SOEM EtherCAT master*, *EtherCAT HMI application*, *Board: Guition JC1060P470C_I_W_Y*, *HMI (LVGL)*, *EtherCAT axes*).
- File `sdkconfig` nằm trong thư mục build (`C:\esp\build\ehmi_general\sdkconfig`), không nằm trong thư mục app. Sửa file defaults thì xoá file `sdkconfig` đó để IDF tạo lại; giá trị đã có trong `sdkconfig` không bị defaults ghi đè.
- Không commit `sdkconfig`, `build/`, `managed_components/`. **Có** commit `dependencies.lock` của từng app: nó ghim LVGL 9.6.0~1, esp_lvgl_port 2.9.0, esp_lcd_jd9165 2.0.2, esp_lcd_touch_gt911 1.2.1.
- Bảng phân vùng là `firmware/partitions.csv` (app 6 MB). `nvs` vẫn ở 0x9000, nên topology EtherCAT đã lưu không mất.
- **Nội suy 2 trục (CSP)** mặc định tắt từ 2026-10-31, vì máy chỉ cần Profile Position. Muốn bật lại, thêm `CONFIG_ECAT_PATH=y` vào `sdkconfig.defaults` của app rồi xoá `sdkconfig` trong thư mục build. Khi đó tab Manual › Interpolation và lệnh `path` có lại (mục 7e).

### 2b. Nạp bằng `firmware\flash.bat` (cách nhanh)

Nhấp đúp `firmware\flash.bat`, hoặc chạy `flash.bat COM7` trong terminal để chỉ định cổng. Phần xử lý nằm trong `firmware\tools\flash.ps1`. Không cần mở terminal ESP-IDF trước: script tự nạp môi trường v6.1.

Script tìm trên ổ C: rồi ổ D:: `Microsoft.v6.1.PowerShell_profile.ps1` trong `<ổ>:\Espressif\tools` (cài bằng EIM), hoặc `<ổ>:\esp\v6.1\esp-idf` kèm `<ổ>:\esp\tools61` (cài tay, như máy thứ ba). Không thấy thì script báo lỗi và dừng. Thư mục build và thư mục sao lưu nằm cùng ổ với ESP-IDF: `<ổ>:\esp\build\ehmi_<app>` và `<ổ>:\esp\backup`. Các đường dẫn `C:\esp\...` trong tài liệu này ứng với máy cài trên ổ C; máy cài trên ổ D thì đổi thành `D:\...`.

Script làm việc với một app trong `firmware\apps`. Nếu chỉ có một app (hiện là `general`), script tự chọn nó. Nếu có nhiều app, script hỏi lúc mở, và dòng đầu menu ghi app đang chọn.

| Menu | Việc làm |
|---|---|
| 1 | Build → nạp → monitor |
| 2 | Nạp bản đã build sẵn trong `C:\esp\build\ehmi_<app>` (không build lại) → monitor |
| 3 | Chỉ monitor (thoát bằng `Ctrl+]`, gõ lệnh ở dấu nhắc `ecat>`) |
| 4 | Chỉ build |
| 5 | Thông tin chip: revision, kích thước flash, MAC, đã có bản sao lưu chưa |
| 6 | Sao lưu toàn bộ 16 MB flash |
| 7 | Đặt giờ board (và RTC) theo giờ máy tính |
| 8 | Đổi app (chỉ hiện khi có từ hai app trở lên) |

- **Tìm cổng:** script tự tìm cổng USB-Serial-JTAG của Espressif (VID 303A, PID 1001, là cổng USB2 "Full Speed"). Không thấy thì nó liệt kê các cổng COM và hỏi.
- **Trước khi ghi, script luôn:**
  1. Đọc chip revision bằng esptool. Nếu revision không khớp với bản build (v1.x hay v3.x, xem mục 4) thì dừng, không ghi gì.
  2. Kiểm tra bản sao lưu gốc của board tại `<ổ>:\esp\backup\<MAC>\factory_full_16MB.bin`. Nếu chưa có, script hỏi: sao lưu ngay (mặc định), bỏ qua, hoặc huỷ. Mỗi board có thư mục riêng theo MAC. Bên cạnh file `.bin` có file `.txt` ghi thời gian, SHA-256 và thông tin chip.
- **Nạp** bằng `esptool write-flash @flash_args` trong thư mục build, đúng lệnh mà `idf.py` in ra sau khi build.
- **Đặt giờ sau khi nạp:** mục 1 và 2 chờ board khởi động lại (tối đa 30 s), rồi gửi giờ của máy tính xuống console bằng lệnh `clock`. Firmware ghi luôn vào RTC. Phần này do `firmware\tools\set_clock.py COMx` làm, chạy riêng cũng được (mục 7), board không bị reset.
  - Script đọc múi giờ của board (Settings › Time) từ lệnh `clock`, rồi đổi giờ UTC của máy tính sang múi giờ đó. Máy tính đặt múi giờ nào cũng được.
  - Đặt giờ không được thì script chỉ cảnh báo và in câu trả lời của board, rồi vẫn mở monitor.
- **Tham số dòng lệnh:** `powershell -File firmware\tools\flash.ps1 -Port COM7 -Action build|flash|flash-only|monitor|info|backup|clock [-App general]` chạy một việc rồi thoát (exit 0 là thành công).
- `flash.ps1` phải giữ mã hoá **UTF-8 có BOM**. Thiếu BOM thì Windows PowerShell 5.1 đọc sai chữ tiếng Việt.

### 2c. App mới cho một máy (dựa trên General App)

1. Chép thư mục `firmware\apps\general` thành `firmware\apps\<tên>`. Tên viết thường, không dấu, không khoảng trắng, ví dụ `press2`. Không chép `managed_components` (component manager tự tải lại theo `dependencies.lock`).
2. Trong `firmware\apps\<tên>\CMakeLists.txt`, đổi `project(ehmi_general)` thành `project(ehmi_<tên>)`.
3. Ghi cấu hình riêng của máy vào `firmware\apps\<tên>\sdkconfig.defaults`, ví dụ tên máy trên thanh tiêu đề (`CONFIG_HMI_MACHINE_NAME`), tắt dữ liệu demo (`CONFIG_HMI_DEMO_DATA=n`), hoặc bật nội suy (`CONFIG_ECAT_PATH=y`).
4. Viết trình tự máy trong `firmware\apps\<tên>\main\`, chạy sau `app_core_start()`. Code dùng chung cho nhiều máy thì đưa vào `firmware\components\`, không chép sang từng app.
5. `flash.bat` thấy app mới và hỏi chọn app. Build nằm ở `<ổ>:\esp\build\ehmi_<tên>`.

Sửa lỗi trong phần chung (`components`, `app_core`, `sdkconfig.defaults` chung) thì mọi app đều nhận được ở lần build sau. Hiện `app_core` chưa có chỗ móc riêng cho trình tự máy (gọi trong chu kỳ, trang HMI riêng); những chỗ đó sẽ thêm khi viết app máy đầu tiên.

## 3. Kết nối board, tìm COM port, đọc chip revision, sao lưu flash

1. Cấp nguồn 5 V, tối thiểu ≥ 600 mA (BURN L1). Cắm cáp vào **USB2 "Full Speed" Type-C**. BURN L2 bản chữ Hán ghi đúng "USB2"; dòng tiếng Anh "USB1" là sai. Đây là USB-Serial-JTAG trên GPIO24/25 (SCH).
2. Tìm COM port. Trong Device Manager, mục "Ports (COM & LPT)" sẽ có "USB Serial Device (COMx)" hoặc "USB JTAG/serial debug unit". Hoặc chạy:
   ```powershell
   [System.IO.Ports.SerialPort]::GetPortNames()
   ```
3. Đọc chip và revision. Đây là bước **bắt buộc**:
   ```powershell
   esptool.py --chip esp32p4 -p COM5 flash_id
   ```
   Ghi lại dòng `Chip is ESP32-P4 (revision vX.Y)` và kích thước flash (kỳ vọng 16 MB).
4. **Sao lưu toàn bộ flash gốc** trước mọi thao tác nạp (16 MB, mất vài phút):
   ```powershell
   New-Item -ItemType Directory -Force C:\esp\backup
   esptool.py --chip esp32p4 -p COM5 -b 921600 read_flash 0 0x1000000 C:\esp\backup\factory_full_16MB.bin
   ```
   Sau đó chép file backup vào nơi an toàn.
   `flash.bat` (mục 2b) làm bước 3 và 4 tự động, lưu vào `C:\esp\backup\<MAC>\factory_full_16MB.bin`.
5. Nếu esptool không kết nối được, vào download mode bằng tay: **giữ BOOT, nhấn rồi nhả RESET, sau đó nhả BOOT**. BOOT = GPIO35 = 0 khi reset sẽ vào Joint Download (DS p.33).

## 4. Thiết lập theo chip revision (dễ nhầm)

| esptool báo | sdkconfig cần |
|---|---|
| **v1.x** (vendor giả định: ETH:941,945; `firmware/sdkconfig.defaults` cũng đặt như vậy) | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` (REV_MIN_FULL = 100, REV_MAX_FULL = 199). CPU 360 MHz. |
| **v3.x** | **Bỏ** `SELECTS_REV_LESS_V3`. Revision tối thiểu v3.0/v3.1 (mặc định của 5.5.x là 301). CPU 400 MHz. Mọi file `.bin` của vendor **sẽ không boot**. |

- Sửa trong `sdkconfig.defaults`, xoá `sdkconfig` rồi build lại; hoặc dùng `idf.py menuconfig` **(đường dẫn menu chưa xác minh)**.
- v1.x và v3.x **không dùng chung được firmware**.
- **Không chạy `idf.py set-target`** trên demo vendor. Lệnh này tạo lại sdkconfig và làm mất thiết lập revision. sdkconfig của vendor đã có sẵn target esp32p4.

## 5. Smoke test #1 — demo Ethernet (vendor) — **đã build được trên v5.5.4**

Demo của hãng không build trên IDF 6 (mục 0), nên mục 5 và 6 vẫn dùng v5.5.4. Firmware dự án thì chỉ build trên v6.1.

```powershell
. C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1
$src = "C:\Users\Tuan\ownCloud\Tuan\EthercatHMI\vendor\JC1060\1-Demo\Demo_IDF\ESP-IDF_5.5.4\JC1060P470C_I_W_Y_New_Panel"
Copy-Item "$src\ethernet" C:\esp\smoke\v554\ethernet -Recurse -Force
cd C:\esp\smoke\v554\ethernet
idf.py build                      # sdkconfig vendor: IP101, MDC31, MDIO52, RST51, ADDR1 (ETH:784-792)
idf.py -p COM5 flash monitor
```

- Demo Ethernet giống hệt nhau ở New và Old.
- Cắm cáp LAN vào router hoặc switch có DHCP.

**Đạt khi**
- Boot log có `chip revision: vX.Y` khớp với bước 3.
- Có log Ethernet Link Up và "Ethernet Got IP Address".
- Đèn RJ45 sáng.

**Lưu ý:** sdkconfig của demo này dùng flash 2 MB, -Og, tick 100 Hz và không bật PSRAM (ETH:727-744, 799, 1126, 1255). Đủ cho smoke test, nhưng **không dùng làm nền cho dự án**.

## 6. Smoke test #2 — `lvgl_demo_v9` (đồng thời xác định New hay Old panel)

Demo cần thư mục anh em `../common_components` (`EXTRA_COMPONENT_DIRS`). Build cần Internet vì component manager tải lvgl 9.5.0, `esp_lcd_jd9165` `"*"`, `esp_lvgl_port` ^2, `esp_lcd_touch_gt911` ^1. Hãng **không kèm `dependencies.lock`**, nên phiên bản tự trôi theo registry.

**Hai bản vá bắt buộc (đã kiểm tra trên v5.5.4, 2026-10-03):**
1. Xoá `common_components\espressif__esp-brookesia` khỏi bản copy. Component này đòi `lvgl <9`, xung đột với lvgl 9.5.0
   (README của hãng cũng bảo chuyển nó sang `esp_brookesia_phone\components`).
2. Thêm `  espressif/esp_lvgl_port: "~2.8.0"` vào `lvgl_demo_v9\main\idf_component.yml`.
   `esp_lvgl_port` 2.9.0 (bản mới nhất) dùng `on_frame_buf_complete`, chưa có trong IDF 5.5.4.

```powershell
$src = "C:\Users\Tuan\ownCloud\Tuan\EthercatHMI\vendor\JC1060\1-Demo\Demo_IDF\ESP-IDF_5.5.4\JC1060P470C_I_W_Y_New_Panel"
Copy-Item "$src\lvgl_demo_v9"      C:\esp\smoke\v554\lvgl_demo_v9 -Recurse -Force
Copy-Item "$src\common_components" C:\esp\smoke\v554\common_components -Recurse -Force
Remove-Item C:\esp\smoke\v554\common_components\espressif__esp-brookesia -Recurse -Force
Add-Content C:\esp\smoke\v554\lvgl_demo_v9\main\idf_component.yml '  espressif/esp_lvgl_port: "~2.8.0"'
cd C:\esp\smoke\v554\lvgl_demo_v9
idf.py build
idf.py -p COM5 flash monitor
```

**Đạt khi**
- Demo LVGL hiển thị toàn màn hình 1024x600.
- Màu và gamma đúng, không nhấp nháy.
- Chạm vào đâu thì đối tượng phản hồi đúng vị trí đó.

**Nếu màu hoặc gamma sai, màn hình nhấp nháy hoặc tối:** lặp lại với `..._Old_Panel` (copy vào thư mục khác, ví dụ `C:\esp\smoke\old\`). Thư mục BSP của bản Old tên là `esp32_p4_function_ev_board`, không có tiền tố `espressif__`. Biến thể cho ảnh đúng chính là panel của bạn. Đối chiếu thêm với nhãn "V2" sau vỏ: có "V2" nghĩa là New (BURN L3).

## 7. Smoke test #3 — EtherCAT master (`firmware/`)

Console (log và lệnh) chạy trên cổng **USB2 "Full Speed"**, chính là cổng dùng để nạp. Gõ lệnh ngay trong `idf.py -B C:\esp\build\ehmi_general -p COMx monitor` (chạy trong `firmware\apps\general`) hoặc mục 3 của `flash.bat`, dấu nhắc là `ecat>`.

1. Build và nạp theo mục 2.
2. **Trước khi cắm phần cứng:** gõ `selftest`. Lệnh kiểm tra logic so sánh topology và logic điều khiển trục (với drive mô phỏng) mà không cần bus; mong đợi `selftest PASS (... checks)` và `axis self-test: all checks passed`.
3. **Không cần đặt station alias.** Trục được đánh số theo thứ tự trên dây: drive đầu tiên sau board là AX1, drive kế tiếp là AX2, … (`ETHERCAT_PLAN.md` mục 3.6).
   - Nối dây đúng thứ tự trục mong muốn và dán nhãn. Hai drive cùng model đổi chỗ thì firmware không phát hiện được.
4. Cắm cáp từ RJ45 của board vào cổng **IN** của slave đầu tiên. Không cắm qua switch hay router.
5. **Lần khởi động đầu** (chưa có topology lưu):
   - Log in danh sách slave, bảng "Bus (last scan…)" và các issue, rồi `Bus held in PRE-OP`.
   - Kiểm tra bảng, đặc biệt thứ tự AX1, AX2, …, model và module R1EC, rồi gõ `accept`.
   - Bus khởi động lại, so sánh khớp và vào OP.
6. **Log mong đợi khi chạy:**
   - `board_eth: link up: 100M full duplex`;
   - `ecat_cpl: slave N R1EC: … 0xF030 written|already set (sub 0 = …), InitCmd errors 0, PDO assign ok, in X bits, out Y bits`. Ghi lại giá trị `sub 0`;
   - `Topology OK`, `all slaves OPERATIONAL`;
   - mỗi giây một dòng `cycles … | WKC x/x err 0 | …`;
   - dòng `coupler slave N: status ok | in … | out …`. Bật/tắt một đầu vào thật và xem byte nào đổi.
7. **Các lệnh:**

   | Lệnh | Tác dụng |
   |---|---|
   | `topo` | In topology đã lưu, lần scan gần nhất và kết quả so sánh |
   | `stop` | Rời OP, giữ ở PRE-OP |
   | `scan` | Quét lại (chỉ khi không ở OP) |
   | `accept` | Lưu lần scan gần nhất làm topology mong đợi |
   | `recheck` | Quét lại, so sánh, vào OP nếu khớp |
   | `forget yes` | Xoá topology đã lưu |
   | `sdo <pos> <index> [sub]` | Đọc một object CoE của slave ở vị trí `pos`, index viết hex: `sdo 10 F050 0`. Lỗi thì in mã abort SDO |
   | `sdo <pos> rack` | In `0xF000`, `0xF050`, `0xF030`, `0x1C00`, `0x1C12`, `0x1C13` của một coupler dạng module (R1EC) |
   | `store` | Dữ liệu máy đã lưu (timer, cài đặt trục, điểm teach, múi giờ, màn hình): danh sách key và dung lượng còn trống |
   | `store test` | Lưu thử 10 lần, mỗi lần 1 KB, rồi xoá; in thời gian. Xem `overruns` và `wake late` ở dòng `cycles …` kế tiếp để biết việc lưu ảnh hưởng tới chu kỳ EtherCAT thế nào |
   | `help` | Danh sách lệnh |
8. **Lỗi thường gặp:**
   - `0xF030 NOT TAKEN (power-cycle the coupler)`: tắt rồi bật nguồn R1EC, sau đó `recheck`.
   - `0xF030 WRITE REFUSED`, `InitCmd errors N` hoặc `PDO assign FAILED`: các dòng `SDO 0x….:… abort 0x…` ngay sau đó cho biết lý do. Ghi lại log và báo lại.
   - `ESC SM… length …, expected …`: coupler tự đổi độ dài SM. Ghi lại log và báo lại; đây là chỗ chưa xác minh.
   - `Rack unreadable` / `coupler module list 0xF050 is not readable`: dòng `ecat_cpl: slave N: SDO read 0xF050:… failed (recorded state …)` và các dòng `SDO … abort …` ngay sau đó cho biết lý do. Gõ `sdo N rack` để đọc thử bằng tay. Lần đầu gặp ngày 2026-10-11 là do lỗi firmware, đã sửa (`SLAVES_LEADSHINE.md` mục 4.5).
   - `Topology MISMATCH`: xem bảng so sánh, sửa phần cứng rồi `recheck`, hoặc `scan` + `accept` nếu thay đổi là cố ý.
   - `slave N lost`: nếu slave đó đã mất nguồn thì nó không tự quay lại OP. Dừng máy, gõ `stop` rồi `recheck`.
   - `held in PRE-OP after a failed identity check`: slave được cấu hình lại nhưng không còn khớp topology (rack khác, model khác ở vị trí đó). Dừng máy, `stop`, `recheck`.
   - `too many slaves`: build hiện xử lý tối đa 31 slave (`CONFIG_SOEM_MAXSLAVE` − 1).
9. **Phép thử an toàn ngày đầu** (chỉ khi các đầu ra chưa nối van hay cơ cấu chấp hành): bật vài đầu ra R1EC, rút cáp ở cổng IN của R1EC, ghi lại đầu ra giữ nguyên hay tắt. Kết quả này quyết định giá trị `0x8000:x` "state when link lost" (`SLAVES_LEADSHINE.md` mục 4.2). Firmware hiện chưa có lệnh bật đầu ra; phép thử này làm khi đã có lệnh đó.
10. Không có slave: log `no slaves found, retrying` mỗi 2 s, kèm số frame TX/RX.
11. Chỉ muốn quét và so sánh, không vào OP: tắt *EtherCAT HMI application → Bring slaves to OPERATIONAL*.

## 7b. Smoke test #4 — Màn hình HMI (LVGL, `firmware/`)

Cùng firmware với mục 7. HMI khởi động cùng EtherCAT, không cần cắm slave. Giao diện theo mockup Windows 11 Fluent (`HMI_THEME.md`).
- Đã có: khung chung (thanh tiêu đề, rail điều hướng), **Main**, **Manual › Jog** và **Teaching** (mục 7c), **IO monitor**, **Alarms**, **Settings › Time / Servo / EtherCAT / System** (mục 7d).
- Settings › Recipe, User & password, Maintenance, Backup / restore chưa có mockup nên chỉ hiện khung tạm.
- Lệnh console `page <tên>` mở một trang mà không cần chạm màn hình: `main`, `manual`, `teach`, `io`, `alarms`, `settings`, `time`, `servo`, `ethercat`, `system`.
- **Chụp màn hình qua cổng USB** (bản 2026-10-11): đóng monitor hoặc chương trình nào đang giữ cổng COM, rồi trong môi trường ESP-IDF chạy
  `python firmware\tools\screenshot.py COM3 anh.png`.
  - Board không bị reset, EtherCAT vẫn chạy.
  - Ảnh lấy thẳng từ frame buffer nên đúng như trên màn hình, kể cả hộp thoại và cân bằng màu, và kể cả khi màn hình đang dim hay tắt.
  - Lệnh console tương ứng là `screenshot`; nó in ra các dòng `@S …` mà script ghép lại.
- **Wi-Fi và giờ Internet** (bản 2026-10-11, đã chạy trên board với mạng OpenWrt): Settings › Network.
  - Bật công tắc Wi-Fi. Trang tự quét mạng; chạm vào một mạng, nhập mật khẩu bằng bàn phím trên màn hình, rồi bấm OK. Mạng ẩn thì dùng nút Hidden network.
  - Thẻ Internet time bật sẵn với máy chủ `pool.ntp.org`. Khi Wi-Fi có kết nối, đồng hồ được đặt theo máy chủ rồi ghi vào RTC. Việc này lặp lại mỗi giờ và mỗi lần kết nối lại. Vì vậy khi RTC chưa có pin, đồng hồ vẫn tự đúng lại sau mỗi lần mất điện, miễn là có Wi-Fi ra Internet.
  - Nếu mạng của xưởng không ra được Internet, đặt máy chủ NTP nội bộ ở ô Server.
  - Mật khẩu Wi-Fi được lưu dạng chữ thường trong flash (machine store, khoá `wifi`).
  - Console:
    - `wifi` xem trạng thái; `wifi on` / `wifi off`; `wifi scan`;
    - `wifi join "Ten mang" matkhau` để kết nối; `wifi forget` để quên mạng;
    - `wifi tasks` in core và priority của các task mạng. Các task của esp_hosted phải ở core 0;
    - `ntp`, `ntp on|off`, `ntp server <tên>`, `ntp sync`.
  - Lỗi C6 (chip Wi-Fi) chỉ làm Wi-Fi báo "Not available", **không** khởi động lại board.
- **Giả lập giao diện trên PC** (không cần board): trong `firmware` chạy
  `python tools\hmi_sim\sim_build.py run`.
  - Ảnh PNG của mọi trang và hộp thoại nằm ở `D:\esp\build\hmi_sim\shots`.
  - Chỉ vẽ vài trang: `python tools\hmi_sim\sim_build.py run main settings keyboard`.
  - Cần Visual Studio có bộ C++ (MSVC). Phải build firmware trước một lần, vì bản giả lập lấy cấu hình LVGL từ bản build đó.
  - Dữ liệu máy là dữ liệu mẫu: bus thử 10 slave ở OP, vài alarm, Wi-Fi "OpenWrt".
- **RAM**: Settings › System info, hoặc lệnh console `mem`. Lệnh này in RAM trống, mức thấp nhất từ lúc khởi động, khối liền lớn nhất, và phần stack chưa từng dùng của từng task. Stack còn dưới khoảng 1 KB thì nên nâng.
- **Dim và tắt màn hình**: đặt ở Settings › Display, thẻ Screen. Trên console:
  - `display dim <giây>` và `display off <giây>` đặt thời gian (0 = không bao giờ);
  - `display wake` làm sáng màn hình;
  - `display` in trạng thái `on`, `dimmed` hoặc `off`.

1. **Chọn panel New/Old** khi chưa biết board là bản nào (`BOARD.md` mục 5):
   - Mặc định là **New** (menuconfig *Board → Default LCD panel variant*).
   - Màn hình sai màu hoặc gamma, nhấp nháy hay đen: gõ `panel old` trong console, rồi nhấn RESET. Không cần build lại.
   - `panel` in ra panel đang dùng; `panel new` để quay lại.
   - Cả hai đều đen: thử *Board → MIPI-DSI lane bit rate* = 550 hoặc 900, build lại.
   - Bản nào cho ảnh đúng thì ghi vào `BOARD.md` mục 5.
2. **Log mong đợi:**
   - `board_lcd: New panel, 2 lanes at 750 Mbps, DPI 52 MHz, HBP 136`;
   - `hmi_bg: background 1024x600 generated in N ms`;
   - `hmi: HMI up: New panel, free internal … B, free PSRAM … B`. Ghi lại các con số này và báo lại.
3. **Cảm ứng:**
   - Chạm các mục trên rail trái để chuyển trang; chạm Settings ở góc dưới trái để kiểm tra toạ độ.
   - Nếu chạm bị lệch hoặc ngược trục thì báo lại. BSP của hãng có sẵn một hàm đổi toạ độ 800×480 → 1024×600 nhưng đang tắt, nên **chưa xác minh** toạ độ.
   - Log `touch controller not found`: kiểm tra cáp FPC3.
4. **Thanh tiêu đề:**
   - Chip EtherCAT lấy trạng thái thật: `EtherCAT no link` khi chưa cắm, `EtherCAT PRE-OP` khi bus đang bị giữ, `EtherCAT OP · 1.0 ms` khi chạy.
   - Đồng hồ lấy từ RTC RX8025T. Nếu RTC mất giờ thì hiện `--:--` / `Clock not set` kèm cảnh báo W001; đặt giờ ở Settings › Time hoặc bằng `clock 2026-10-10 14:30`.
   - Badge đỏ trên mục Alarms là số alarm trong danh sách hiện tại (màu cam nếu chỉ có cảnh báo).
5. **Trang Main** có nhãn "demo data": số liệu lấy từ mockup, còn Auto/Manual và Start/Stop chỉ đổi phần hiển thị, **không điều khiển máy**. Tắt bằng *HMI (LVGL) → Show the mockup's demo values*.
6. **So với mockup:** đặt màn hình cạnh mockup. Ghi lại cỡ chữ có dễ đọc không, viền thẻ có thấy rõ không, màu nền xám và màu xanh nhấn trông thế nào.

## 7c. Smoke test #5 — Motion cơ bản với drive Leadshine (code 2026-10-10, chưa chạy với drive thật)

**An toàn trước khi thử**
- Motor chưa nối cơ khí, hoặc cơ cấu còn đủ hành trình theo cả hai chiều.
- Có nút E-stop hoặc STO thật trong tầm tay.
- Firmware **chưa có soft limit**. Home dùng phương pháp đang cài trong drive (0x6098), firmware không đổi phương pháp này.
- Tốc độ có ba mức: Low 10, Mid 60 và High. High là "Jog speed (high)" của từng trục ở Settings › Servo, mặc định 300 rpm.
- Gia tốc và giảm tốc lấy ở Settings › Servo, mặc định 1000 rpm/s (300 rpm trong 0,3 s).

**Các bước**
1. `selftest`. Mong đợi `selftest PASS (23 checks)` (board 2026-10-12) và `axis self-test: all checks passed` (22 kiểm tra với một drive mô phỏng; thêm 14 cho đường 2 trục khi bật `CONFIG_ECAT_PATH`, xem 7e). Lệnh không cần bus.
2. Cắm drive vào board theo thứ tự trục (mục 7 bước 3). Không cần đặt alias.
3. Trên màn hình, mở **Settings › EtherCAT**. Lần đầu, bảng hiện "Scan result" cùng trạng thái của từng slave.
   - Bấm **Accept** → xem danh sách thay đổi → **Save topology**. Có thể gõ `accept` trong console thay cho bước này.
   - Bus khởi động lại. Banner chuyển sang "Topology OK", chip trên thanh tiêu đề thành `EtherCAT OP`.
4. **Log mong đợi:**
   - `ecat_drv: slave N: PDO remapped (PP/PV/HM, 0x503F); encoder …, gear …, feed …, homing method …`. Ghi lại các con số này.
   - `ecat_axis: AX1: slave N ready, … units/rev`.
   - Nếu log báo `PDO remap refused` hoặc `mapped … bits, expected …`: trục hiện "Unavailable". Gửi lại các dòng `SDO 0x….:… abort` đi kèm.
5. **Trang Manual › Jog:**
   - Chọn trục, rồi bấm **Servo on**: đèn "Servo on" xanh, dòng trạng thái "Ready".
   - Chế độ **Step**, bước 0.1 rev, tốc độ Low: mỗi lần chạm `AX1 +` hoặc `AX1 −` trục quay 0,1 vòng.
   - Chế độ **Continuous**: giữ nút thì chạy, nhả thì dừng.
   - **Move to**: chạm ô số, nhập vị trí (rev), bấm **Go**. Trong lúc chạy, nút Go thành **Stop**.
   - **Home axis**, nút **Stop** màu đỏ (dừng có giảm tốc mọi chuyển động của trục), và **Alarm reset** khi drive báo lỗi.
6. **Console** (thay cho màn hình):

   | Lệnh | Tác dụng |
   |---|---|
   | `axis` | Danh sách trục: trạng thái CiA402, vị trí, statusword/controlword, mã lỗi |
   | `axis 1 on` / `off` / `reset` | Servo on / servo off / reset lỗi |
   | `axis 1 stop` / `qstop` | Dừng có giảm tốc / quick stop rồi tắt servo |
   | `axis 1 home` | Về gốc theo phương pháp trong drive |
   | `axis 1 move 2.5r 30` | Đến vị trí 2,5 vòng với 30 rpm (bỏ `r` thì đơn vị là units của drive) |
   | `axis 1 rel -0.5r` | Đi tương đối −0,5 vòng |
   | `axis 1 jog + 30 2000` | Jog liên tục chiều + với 30 rpm trong 2000 ms |

7. **Cần ghi lại và báo lại**, vì đây là các điểm **(chưa xác minh)**:
   - Số units/rev mà drive báo có đúng không: đánh dấu trục motor, `axis 1 rel 1r`, xem có đúng một vòng không.
   - Tốc độ hiển thị (rpm) có khớp thực tế không. Đơn vị của 0x606C trên L5N/CL3B chưa rõ.
   - Move có kết thúc đúng không (bắt tay set-point qua statusword bit 12). Home có chạy không, dùng phương pháp nào.
   - Khi rút cáp EtherCAT lúc servo đang on: log báo `EtherCAT error: motion stopped, servo off`, drive dừng. Cắm lại thì servo **không** tự bật.
8. **Lỗi thường gặp:**
   - `Servo on timed out`: kiểm tra STO, 24 V và nguồn động lực của drive.
   - `Drive did not change the operation mode` hoặc `Drive did not accept the target`: gửi log kèm `axis` (statusword, mode).
   - `Drive switched the servo off`: drive tự tắt servo (STO, mất nguồn động lực). Bấm Servo on lại sau khi xử lý.
   - `Jog stopped: no refresh from the panel`: màn hình không gửi lệnh jog đều 100 ms một lần. Báo lại nếu hiện tượng này xảy ra khi đang giữ nút.
   - Drive báo `Drive fault 0x821B` ngay sau khi board khởi động lại (nạp firmware, RESET) trong lúc bus đang OP: đây là phản ứng của drive khi mất giao tiếp. Bấm **Alarm reset** rồi **Servo on** (CL3B, 2026-10-10).
   - **Alarm reset** chỉ tắt servo khi drive đang báo lỗi; nếu không có lỗi thì chỉ xoá dòng thông báo lỗi.

## 7d. Smoke test #6 — Các trang IO monitor, Alarms, Teaching, Settings › Time / Servo (code 2026-10-10)

Đã thử trên board không có slave (2026-10-10):
- mở cả 10 trang bằng `page …`, không lỗi;
- `selftest` vẫn đạt;
- RTC: `clock …` ghi giờ, sau reset đọc lại đúng, W001 tự chuyển sang Cleared.

Phần cần drive và R1EC thật thì **chưa thử**.

1. **Alarms**
   - Rút cáp EtherCAT: sau vài giây có `A001 EtherCAT cable not connected`, badge trên rail đổi số.
   - Chạm một dòng để xem nguyên nhân và cách xử lý ở khung bên phải. Nút Acknowledge chuyển trạng thái Active → Acknowledged.
   - Khi điều kiện hết và đã acknowledge, alarm chuyển sang History. History chỉ nằm trong RAM, mất khi reset.
   - **Reset** gửi lệnh reset lỗi tới mọi trục đang lỗi.
   - Export (SD) chưa có vì chưa có driver thẻ nhớ.
   - Console: `alarms`, `alarms ack`.

   | Mã | Ý nghĩa |
   |---|---|
   | A001 | Không có link Ethernet tới slave đầu tiên |
   | A002 | Bus bị giữ ở PRE-OP (chưa Accept, hoặc khác topology đã lưu) |
   | A003 | Lỗi working counter khi đang OP (còn hiện 2 s sau lỗi cuối) |
   | A004 | Slave về lại sau sự cố nhưng không phải thiết bị đã Accept |
   | A101–A116 | Drive của trục 1–16 báo lỗi (mã 0x603F / 0x503F) |
   | A201–A216 | Trục bị dừng: servo on quá thời gian, mất bắt tay, homing lỗi, drive tự tắt servo… |
   | A301–A316 | Trục không dùng được (bố cục PDO không đúng) |
   | A401–A404 | R1EC báo Device Status ≠ 0 |
   | W001 | Đồng hồ chưa đặt (RTC mất giờ hoặc không trả lời) |

2. **IO monitor**
   - Cần R1EC trong topology đã Accept và bus ở OP. Địa chỉ đánh số liên tục theo thứ tự module: X0.0 là DI đầu tiên, Y0.0 là DO đầu tiên. Tên tín hiệu chưa có (chưa có danh sách IO của máy), nên cột tên ghi "Slot s · ch n".
   - **Force mode** cần máy ở Manual. Ở bản demo, trang Main bật sẵn Auto, nên phải chuyển sang Manual ở trang Main trước. Khi bật cần xác nhận.
   - Khi force đang bật, chạm một output để đảo trạng thái. Tắt force hoặc rời trang thì mọi output đã force về off, vì chưa có logic máy điều khiển output.
   - Cần kiểm tra **(chưa xác minh)**:
     - kênh 0 của module là bit thấp nhất (theo thứ tự entry trong ESI);
     - giá trị analog SC-A0400-IV / SC-A0004-IV là số INT thô, thang đo tuỳ cấu hình kênh của module;
     - tab EtherCAT có hiện Device Status OK không.
3. **Manual › Teaching**
   - Nhóm trục: lấy lần lượt 3 trục một nhóm (AX1–AX3, AX4–AX6, …). Chạm nút "Group …" để đổi nhóm.
   - Mỗi nhóm có tối đa 32 điểm, lưu trong NVS theo tên trục, vị trí tính bằng units của drive.
   - Các thao tác:
     - **Add point**: thêm điểm tại vị trí hiện tại.
     - **Teach**: ghi vị trí hiện tại vào điểm đang chọn, có hộp xác nhận.
     - **Go to (hold)**: giữ nút thì các trục chạy PP tới điểm (không nội suy), nhả nút thì dừng.
     - Ô **Go-to speed**: phần trăm của "Jog speed (high)".
     - **Save points**: lưu điểm. Nếu bus đang OP thì việc ghi flash đợi tới khi rời OP.
   - Nút jog: mỗi lần chạm đi một bước, theo bước và tốc độ đang chọn ở trang Jog.
   - Chưa có: đổi tên và xoá điểm, gắn điểm với recipe.
4. **Settings › Servo**
   - Chọn trục. Trang tự đọc 16 object qua SDO ở cả PRE-OP lẫn OP. Ô "n/a" là object drive không có; CL3B không có 0x607D, 0x6072, 0x6065, 0x6091.
   - Chạm một giá trị để sửa (bàn phím số; Direction là danh sách). Ô đã sửa có màu vàng.
   - **Write to drive** ghi các ô đã sửa rồi đọc lại. Thao tác này cần servo của trục đang off.
   - **Save** lưu các thiết lập HMI vào NVS rồi gửi 0x1010:1 = "save" để drive lưu tham số. **Chưa xác minh** drive Leadshine có lưu các object 0x6xxx theo cách này không: tắt nguồn drive rồi đọc lại để kiểm tra.
   - **Quy đổi và tăng/giảm tốc** (thêm 2026-10-10 theo yêu cầu):
     - **Pulse/Rev** (thẻ Mechanical): số xung lệnh cho một vòng motor, ghi vào drive ở 0x6092:1 (0x6092:2 = 1). CL3B mặc định 10000. Bấm Write to drive khi servo off. Trục dùng giá trị mới ngay, không cần Re-check. Điểm teach đã lưu giữ nguyên số xung, nên phải teach lại.
     - **Unit** (nhóm Mechanical): đơn vị hiển thị của trục.
       - **mm** cho trục thẳng, **deg** cho trục quay, **rev** là vòng motor.
       - Chỉ đổi cách hiển thị. Drive và các điểm teach vẫn lưu theo xung, nên đổi Unit không làm sai điểm đã teach.
     - **Distance/Rev** (nhóm Mechanical): quãng đường tải đi được trong một vòng motor, tính theo Unit.
       - Với mm: bước vít me, ví dụ 20 mm.
       - Với deg: 360 nếu tải nối thẳng với motor, hoặc 360 / tỉ số hộp số (ví dụ hộp số 1:10 thì nhập 36).
       - Khi chọn deg, giá trị tự đặt sẵn 360. Khi chọn mm thì phải tự nhập.
     - Khi Unit là mm hoặc deg và Distance/Rev khác 0, mọi trang (Jog, Teaching, Servo) hiển thị mm và mm/s, hoặc deg và deg/s. Ngược lại thì hiển thị vòng (rev) và rpm.
     - Vị trí theo deg không quay vòng về 0 sau 360: 720 deg nghĩa là tải đã quay 2 vòng.
     - Bấm **Save** để áp dụng.
     - **Max speed**: tốc độ tối đa của mọi lệnh chạy và jog từ HMI.
     - **Acc time / Dec time** (ms): thời gian tăng từ 0 lên Max speed và giảm từ Max speed về 0. Mặc định 1000 ms với Max speed 1000 rpm.
     - **Jog speed (high)**: mức High của trang Jog. Các mức Low/Mid (10 và 60 rpm) không vượt quá mức này.
   - Các thiết lập ghi "HMI" ở cột Object không phải object của drive: 0x6081/0x6083/0x6084 nằm trong RxPDO nên HMI gửi theo mỗi lệnh chạy. Bấm **Save** để lưu.
5. **Settings › Time**
   - Timer T01–T08: tên tạm "Timer 1…8" vì chưa có trình tự máy. Sửa bằng bàn phím số; ô đã sửa có màu vàng cho tới khi bấm Save.
   - Date / Time / **Set clock** mở bộ chọn ngày giờ kiểu Windows (cuộn từng cột). Giờ ghi ngay vào RTC.
   - Time zone: mặc định UTC+07:00, bước 15 phút.
   - Đồng bộ NTP qua Wi-Fi chưa có (ESP32-C6 chưa được dùng).

## 7e. Smoke test #7 — Nội suy 2 trục (CSP, code 2026-10-12, chưa chạy với drive thật)

**Tắt mặc định từ 2026-10-31** (máy chỉ cần Profile Position). Muốn thử phần này, bật `CONFIG_ECAT_PATH=y` cho app (mục 2) rồi build lại. Khi tắt, Manual không có tab Interpolation, console không có lệnh `path`, và code CSP không nằm trong vòng chu kỳ.

Trang **Manual › Interpolation** chạy một đường thẳng hoặc một cung tròn trên hai trục, rồi đo xem hai trục có bám đường không.
- Hai drive chạy ở **CSP** (0x6060 = 8). Mỗi chu kỳ 1 ms, task `ecat` tính vị trí đích của cả hai trục từ cùng một biên dạng tốc độ (hình thang) dọc theo đường, nên đây là nội suy thật. Cách PP chia tỉ lệ tốc độ của PGM không phải nội suy.
- Bus chạy **không có DC** (Free Run). Drive L5N/CL3B có nhận CSP như vậy không thì **chưa xác minh**. Xem `docs/SLAVES_LEADSHINE.md` mục 6.
- Hết đường hoặc khi dừng, hai trục giữ vị trí cuối 50 ms rồi trở về PP. Jog và Move dùng lại được ngay.

**An toàn trước khi thử**
- Motor chưa nối cơ khí, hoặc cả hai trục còn đủ hành trình theo mọi hướng của đường. Đường tính từ vị trí lúc bấm Start, và firmware **chưa có soft limit**.
- Có E-stop hoặc STO thật trong tầm tay. Lần đầu chạy chậm: 0,5–1 vòng/s, đường ngắn.

**Các bước**
1. `selftest`: mong đợi `axis self-test: all checks passed` (36 kiểm tra: 22 cho một trục, 14 cho đường 2 trục với hai drive mô phỏng ở CSP).
2. **Settings › Servo**: đặt cùng một Unit và Distance/Rev cho hai trục (ví dụ mm). Nếu hai trục khác đơn vị, trang báo nhắc. Max speed và Acc time của từng trục giới hạn tốc độ và gia tốc của đường.
3. **Manual › Interpolation**:
   - Chọn trục X và Y bằng hai **ComboBox thả xuống** ở cột trái. Bấm **Servo on** (bật cả hai trục).
   - Cột phải, chọn kiểu đường:
     - **Line**: X distance, Y distance.
     - **Arc CW / Arc CCW**: tâm Centre X (I), Centre Y (J) tính từ điểm đầu, và Angle là góc quay. 360 là cả vòng tròn; điểm cuối tự tính ra.
   - **Speed** là tốc độ dọc theo đường (đơn vị/s). **Ramp** (ms) là thời gian tăng từ 0 lên tốc độ đó. Nếu trục không cho phép (Max speed, Acc time, gia tốc hướng tâm v²/r của cung), dòng ghi chú báo giá trị thật sẽ dùng.
   - **Start** chạy. **Stop** giảm tốc dọc theo đường rồi dừng; servo vẫn on. Nút Stop của trang Jog cũng dừng đường.
   - Đồ thị: đường xám là đường dự kiến, đường xanh là vị trí thật (lấy mẫu mỗi 40 ms), chấm đậm là điểm đầu.
   - Kết quả ở cột trái:
     - **Contour error**: khoảng cách lớn nhất từ vị trí thật tới đường, tức sai số nội suy. Task `ecat` đo mỗi chu kỳ.
     - **Lag (max)**: khoảng cách lớn nhất giữa đích vừa gửi và vị trí thật. Gồm cả trễ truyền khoảng 2 chu kỳ, nên tăng theo tốc độ.
4. **Console** (đơn vị là vòng motor; không áp Max speed, chỉ để thử trên bàn):

   | Lệnh | Tác dụng |
   |---|---|
   | `path line 1 2 2 1 0.5` | AX1/AX2 đi thẳng (+2, +1) vòng, 0,5 vòng/s |
   | `path ccw 1 2 1 0 360 0.5` | Vòng tròn ngược chiều kim đồng hồ, tâm (+1, 0) vòng |
   | `path cw 1 2 1 0 90 0.5 300` | Một phần tư vòng theo chiều kim đồng hồ, ramp 300 ms |
   | `path` | Trạng thái và kết quả (contour error, lag, thời gian) |
   | `path stop` | Giảm tốc dọc đường rồi dừng |

5. **Cần ghi lại và báo lại** (các điểm **chưa xác minh**):
   - Drive có vào CSP không. Nếu trang báo `Drive did not change the operation mode` thì drive cần DC SYNC0 để chạy CSP; gửi lại log và kết quả `axis` (cột mode).
   - Motor có kêu, rung hoặc báo lỗi bám (following error) khi chạy CSP không có DC không.
   - Contour error và Lag ở vài tốc độ, ví dụ 0,5, 1 và 2 vòng/s.
   - Sau khi chạy xong, Jog và Move to ở trang Jog có chạy bình thường không (trục đã trở về PP).
6. **Lỗi thường gặp:**
   - `Path stopped: the other axis stopped`: trục kia bị tắt servo, báo lỗi hoặc dừng nhanh. Lỗi của trục đó hiện ở dòng ghi chú.
   - `Path not valid (axes, size or position range)`: hai ô chọn cùng một trục, đường ngắn hơn một xung, hoặc vượt quá phạm vi vị trí 32 bit.
   - Mất bus (WKC sai 3 chu kỳ): cả hai servo tắt, giống trang Jog.

## 8. (Tùy chọn) Smoke test RS485

Áp dụng cho `uart_echo_rs485`: UART1 TX = 26, RX = 27, 115200 8N1 (`IDF_N/uart_echo_rs485/main/rs485_example.c:28-81`).

- Nối J4: chân 2 = A, chân 3 = B, chân 4 = GND, qua USB-RS485 vào PC.
- Board không có trở cuối 120 Ω; lắp thêm bên ngoài nếu dây dài.
- Gửi chuỗi từ PC và nhận lại echo.

## 9. Nạp lại firmware nhà máy (khi cần)

Firmware nằm ở `vendor\JC1060\8-Burn operation\Burn files\`.
- **New panel:** `JC1060P470C_I_W_Y-V3.7_New_Panel.bin`.
- **Old panel:** `-V2.7.bin`. **Bẫy:** tên "V2.x" là firmware cho **Old**.
- Mọi image đều là image gộp, nạp ở **0x0**, và chỉ chạy trên chip **rev < 3.0**.
- Cách chắc chắn nhất là nạp lại **bản backup của chính board** ở mục 3.

**Cách 1 — Flash Download Tool** (theo `8-Burn operation/Burn operation instructions/Burn operation-1..3.png`):
1. Copy `vendor\JC1060\8-Burn operation\flash_download_tool_3.9.9` sang `C:\esp\tools\fdt`. Tool tự ghi log vào thư mục của nó, nên không chạy trong `vendor`.
2. Chạy `flash_download_tool_3.9.9_R2.exe` và chọn ChipType **ESP32-P4**, WorkMode **Develop**, LoadMode **USB**.
3. Tick file `.bin` ở địa chỉ **0x0000**, SPI SPEED **80 MHz**, SPI MODE **QIO**, tick **DoNotChgBin**, chọn COM, rồi bấm **START**.
4. Nạp xong thì nhấn RST hoặc cắm lại nguồn.

**Cách 2 — esptool** (tương đương):
```powershell
esptool.py --chip esp32p4 -p COM5 -b 921600 write_flash 0x0 "C:\esp\backup\factory_full_16MB.bin"
```

**ESP32-C6:** vendor không có hướng dẫn. Chỉ nạp lại `JC-C6-slave_v2.3.2.bin` qua JP1-20/22/24/26 nếu Wi-Fi hỏng **(chưa xác minh quy trình)**.

## 10. Xử lý sự cố

| Triệu chứng | Nguyên nhân hoặc cách xử lý |
|---|---|
| Ảnh không boot, log báo revision không hợp lệ | Thiết lập `SELECTS_REV_LESS_V3` không khớp chip (mục 4) |
| Lỗi "path too long", file không tìm thấy khi build | Build bằng `-B C:\esp\build\...`, smoke test trên bản copy ở `C:\esp\smoke\...` |
| `Version solving failed ... lvgl` | brookesia còn trong `common_components` (mục 6, bản vá 1) |
| `has no member named 'on_frame_buf_complete'` | `esp_lvgl_port` 2.9.0 trên IDF 5.5.4 (mục 6, bản vá 2) |
| COM port không xuất hiện | Thử cáp khác (phải là cáp có dữ liệu). Đúng cổng USB2. |
| Không vào được download mode | Giữ BOOT, nhấn RESET, nhả BOOT. Nguồn phải ≥ 600 mA. |
| Màn hình tối, có `task_wdt` | Thử lane rate 550/750/900 (`IDF_N/xiaozhi-esp32-main/main/boards/guition-jc1060p470-new/README.md`). Thử BSP của biến thể còn lại. |
| Ethernet "reset timeout" / "EMAC init failed" | PHY chưa cấp REF_CLK. Kiểm tra `reset_gpio_num = 51` và nguồn cấp. |
| Ethernet chập chờn khi bấm nút | Nút BOOT là GPIO35 = RMII TXD1. Không bấm khi đang chạy. |
| `no slaves found` dù có link | Cáp phải vào cổng IN của slave đầu; không qua switch. Xem số frame RX trong log: RX = 0 nghĩa là frame không quay về. |
