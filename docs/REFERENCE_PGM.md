# REFERENCE_PGM.md — PGM kết nối EtherCAT và cấu hình servo, remote IO như thế nào

**Phạm vi.** Người dùng (2026-10-03) chỉ muốn học từ `PGM/` **cách kết nối bus và cách cấu hình servo, remote IO**.
- Máy đích của firmware này **không phải** máy PGM.
- Vì vậy tài liệu này cố ý bỏ qua phần ứng dụng của PGM: trạng thái máy, alarm, routine, giao diện.

**Nguồn**
- Mã C# trong `PGM/PartronCamTestAuto.Core/Core.Communication/` và `PGM/HISTORY.md`.
- Hai DLL trong `PGM/PartronCamTestAuto.Wpf/bin/Debug/net48/`:
  - `EtherCatMaster.dll` (C#, build 2026-08-20): đọc kiểu, hằng số và chuỗi log bằng reflection.
  - `EtherCatMaster.Native.dll` (C++/CLI bọc SOEM, build 2026-08-21): đọc chuỗi log và tên hàm.
- Mã nguồn thư viện `EtherCatMaster` **không có** trong `PGM/`, nên mọi thứ lấy từ DLL đều là **suy ra từ tên hoặc chuỗi**, không phải đọc code.
- `PGM/` và `vendor/` chỉ đọc. Dữ liệu ESI nằm ở `docs/SLAVES_LEADSHINE.md`.

## 1. Các lớp

```
EtherCatAxis / EtherCatIoDevice    (PGM, Core.Communication/Real) — mỗi trục / IO là một object
        │
EtherCatBusManager                 chủ sở hữu DUY NHẤT của bus; EnsureConnected() idempotent
        │
EtherCatMasterService              (EtherCatMaster.dll) ServoOn/Move/Home/Jog/Stop, SetSlotOutput, poll trạng thái
        │
IEtherCatBus ── SoemEtherCatBus    (EtherCatMaster.Native.dll, SOEM; nạp bằng reflection)
             └─ SimulatedEtherCatBus (mô phỏng; chỉ dùng khi chọn rõ "SIMULATED")
```

- **Một bus, nhiều "kết nối".** Mỗi trục gọi `Connect()` riêng, nhưng chỉ lần gọi đầu thực sự mở bus; các lần sau chỉ trả kết quả của slot đó (`Real/EtherCatBusManager.cs:15-27, 81-98`).
- **Không âm thầm rơi về mô phỏng** khi DLL native thiếu, vì PGM từng báo "OK" trên bus giả trong lúc cáp thật đã rút (`Real/NativeBusFactory.cs:8-17`, `Real/EtherCatBusManager.cs:129-132`).
- **Chu kỳ PDO:**
  - Native chạy bằng `timeSetEvent` + `timeBeginPeriod` (multimedia timer của Windows), đo jitter bằng `QueryPerformanceCounter`. Có `CyclicStats` gồm jitter và scan time min/avg/max.
  - Phía C# chỉ đọc trạng thái đã chụp sẵn mỗi 20 ms (`Communication/MotionConnect.cs:196-248`).
- **API `IEtherCatBus`** (danh sách hàm trong DLL) là mẫu tốt cho lớp `ecat_master` của firmware. Các hàm gồm:
  - `Open`, `ScanSlaves`, `ConnectAllAxis`, `Close`, `CyclicExchange`;
  - `ReadSdo`/`WriteSdo`/`TryReadSdo`;
  - `SetControlword`, `SetModeOfOperation`, `SetTargetPosition`, `SetProfileVelocity`/`Acceleration`/`Deceleration`;
  - `GetStatusword`, `GetActualPosition`, `GetErrorCode`;
  - `GetDetectedModuleIdents`, `GetIoSlotCount`, `GetIoSlotIdent`, `GetIoSlotInput/OutputBitCount`, `GetIoSlotInputRegister`, `SetIoSlotOutputRegister`;
  - `GetIoModuleDeviceStatus`, `SetModuleCatalog`, `SetSyncManagerCatalog`.

## 2. Trình tự kết nối

### 2.1 Scan (trang Setting)

1. `ScanSlaves(adapter)` trả về danh sách slave (vị trí, Vendor, Product, Revision).
2. Với mỗi drive, đọc thêm **0x608F:1** (encoder increments) và **0x608F:2** (motor revolutions) để ra số xung/vòng (`DiscoveredSlaveInfo.PulsesPerRotation`).
3. Từ đó tính **CountsPerUnit = Pulses/Rev ÷ Travel/Rev** (`Wpf/ViewModels/AxisConnectRowVm.cs:55-63`, `SettingViewModel.cs:155-173`).

### 2.2 Connect All

Các bước dưới đây được dựng lại từ C# và chuỗi log của DLL.

1. **Dựng lại cấu hình từ settings mỗi lần** (không cache), để sửa tham số xong không cần khởi động lại app.
   - Kèm **catalog ESI** gồm module R1EC và SM, đọc từ `DriverInfo/*.xml` cạnh exe (`Real/EtherCatBusManager.cs:135-204`).
   - Không có ESI thì R1EC không lộ IO, và PGM ghi cảnh báo.
2. Native: `ecx_init` → `ecx_config_init`. Nếu không lên được PRE-OP: "EtherCAT bus did not reach PRE-OP (check adapter name / cabling / power)".
3. **Kiểm tra định danh từng slot** theo cấu hình: Vendor và Product phải khớp; Revision chỉ hiển thị, không ép (`Communication/AxisConfig.cs:53-70`).
   - Kết quả mỗi slot là `AxisConnectStatus`: `Ok`, `WrongDriver`, `NoResponse` hoặc `PdoUnsupported`, kèm Vendor/Product/Revision thật và số bit vào/ra.
   - Ví dụ log: "Expected Vendor 0x… Product 0x…, found Vendor 0x… Product 0x…", "No slave found at position {0}".
4. **R1EC:** hook PRE-OP → SAFE-OP (mục 4.2).
5. `ecx_config_map_group` → `ecx_configdc` → OP. Nếu không vào được: "All slots matched, but the bus did not reach OPERATIONAL state".
   - DLL **không gọi `ecx_dcsync0`**, nên PGM **không bật SYNC0**. Drive chạy Free Run / SM-sync với PP.
6. **Tìm vị trí object trong PDO của drive** từ mapping thật, không hard-code offset: ra 6040/607A, vào 603F/6041/6061/6064.
   - Thiếu object nào thì slot đó là `PdoUnsupported` ("default PDO does not map one of the objects … (mode other than pp/csp?)").
7. **Ghi tham số xuống từng drive** (mục 3.2).
8. Bắt đầu trao đổi tuần hoàn. Trang Setting tô màu từng slot xanh/đỏ theo kết quả (`AxisConnectRowVm.cs:77-103`).
9. **Kết nối lại bus thì máy phải về gốc lại**, vì encoder tương đối (`Wpf/ViewModels/SettingViewModel.cs:184-186`).

## 3. Cấu hình servo (L5N-400S, CL3B-EC507)

### 3.1 Tham số mỗi trục

Nguồn: `Communication/AxisConfig.cs`; lưu trong `system.json`.

| Tham số | Ý nghĩa | Đi xuống drive? |
|---|---|---|
| `SlavePosition` | Vị trí trên bus | — |
| `ExpectedVendorId/ProductCode/Revision/DriverName` | Định danh để kiểm tra lúc Connect | — |
| `Unit` (mm/deg), `CountsPerUnit` | Đổi counts ↔ đơn vị. Trục có hộp số phải gộp sẵn tỉ số. | Không (master tự đổi) |
| `TravelPerRotation` | Hành trình mỗi vòng motor | **0x6092 Feed constant** (null = giữ giá trị trong drive) |
| `SoftLimitNeg/Pos` | Giới hạn mềm | **0x607D:1/2** (counts), có **đọc lại** để xác nhận |
| `HomingMethod`, `HomingVelocity` | Phương pháp và tốc độ về gốc | **0x6098**, **0x6099** (lúc bắt đầu Home) |
| `DefaultVelocity`, `DefaultAccelTimeS/DecelTimeS` | Profile mặc định | **0x6081/0x6083/0x6084** (mỗi lệnh move) |
| `JogSpeed`, `HomeTimeoutMs`, `MoveTimeoutMs`, `HomeDir`, `HomeReverseTolerance`, `HomeOffset`, `StandbyPos` | Dùng phía master | — |

**Không** thấy dấu hiệu PGM ghi gear ratio (0x6091) hay tham số Pxx.xx (0x2xxx): không có chuỗi log hay hằng nào.
- Như vậy gain, tỉ số điện tử và chiều quay **nằm sẵn trong drive** (cài bằng phần mềm Leadshine).
- Đây là suy luận từ việc *không thấy*.

### 3.2 Ghi tham số lúc Connect (`WriteDriveParameters`)

Theo chuỗi log trong `EtherCatMaster.dll`:
- "[Axis] Software limit (negative/positive) -> {units} units ({counts} counts); read back from drive: {counts} counts (confirmed / **MISMATCH!**)".
  - Ghi 0x607D bằng SDO **rồi đọc lại** để xác nhận. Nên làm giống, vì drive có thể kẹp giá trị.
- "[Axis] Travel/rotation -> {value} {unit}": ghi 0x6092.

### 3.3 Lệnh CiA402

Hằng trong `EtherCatMaster.Constants.CiA402Registers`; trình tự suy từ tên hàm và chuỗi log.

| Lệnh | Cách làm |
|---|---|
| **Servo ON** | Controlword 0x06 Shutdown → 0x07 Switch on → 0x0F Enable operation. Chờ statusword "Operation enabled" (`WaitForOperationEnabled`). Lỗi thì in đủ các bit trạng thái và nhắc **"check E-stop/STO/24V enable input on the drive"**. |
| **Servo OFF** | Controlword 0x00 / 0x06. Phía app xoá cờ đã-về-gốc. |
| **Fault reset** | Controlword 0x80. Mã lỗi đọc từ 0x603F trong PDO, báo lên alarm dạng `0x%04X`. |
| **Đổi mode** | **0x6060 qua SDO**, vì PDO mặc định không có 0x6060. Dùng PP = 1, HM = 6. |
| **Move abs/rel (PP)** | SDO 0x6081 tốc độ, 0x6083/0x6084 gia tốc (tính từ thời gian tăng/giảm tốc). Target 0x607A qua PDO. Xung bit 4 "new set-point" dài `NewSetpointPulseMs` rồi xoá. Xong khi `TargetReached` và sai số ≤ `PositionTolerance`. Huỷ nếu Fault hoặc EMG. |
| **Jog** | PP tới ±`JogReachUnits` (1 000 000 đơn vị) hoặc tới soft limit; dừng bằng Halt. Lý do: PDO mặc định không có 0x60FF (`HISTORY.md:81-88`). |
| **Stop** | Bit Halt 0x100. |
| **Emergency stop** | Quick stop (controlword 0x02). |
| **Home** | Kiểm tra sẵn sàng; SDO 0x6098 method, 0x6099 tốc độ (counts/s); mode 6; bit 4 bắt đầu. Xong = statusword bit 12; lỗi = bit 13. |
| **MoveLinear (2 trục)** | Hai lệnh PP với tốc độ chia theo tỉ lệ Vx/Vy (`LinearMoveProfile`). Không phải nội suy thật. |
| **Vận tốc, torque thực** | 0x606C/0x6077 không có trong PDO mặc định, nên phải đọc bằng SDO. Mặc định tắt (`PollVelocityTorque = false`, chia nhịp 250). |

**Bẫy CiA402 PGM đã gặp:**
- Bit 10 `TargetReached` còn "dính" từ lần Homing trước, làm lệnh move sau báo xong ngay lập tức (`HISTORY.md:186-194`).
- Sau khi đổi mode hoặc gửi set-point mới, phải chờ drive xác nhận (bit 12 ở PP) rồi mới tin bit 10.

## 4. Cấu hình remote IO R1EC

### 4.1 Catalog ESI

- `EsiCatalog.LoadModulesFromFolder` đọc `<Modules>` của ESI: ident, class, số bit vào/ra, PDO mặc định và PDO thay thế (`EsiModuleInfo`).
- `LoadSyncManagersFromFolder` đọc `<Sm>`: địa chỉ, control byte, enable.
- Cả hai được nạp **một lần** cho mỗi tiến trình và đẩy xuống native bằng `SetModuleCatalog`/`SetSyncManagerCatalog` (`Real/EtherCatBusManager.cs:135-162`).

### 4.2 Hook `PreOpToSafeOpConfig` (native)

Trình tự dựng lại từ chuỗi log. Tên hàm: `PreOpToSafeOpConfig`, `LookupModuleIndex`, `LookupModuleWidth`, `DownloadSlotMapping`, `DownloadPdoMapping`, `LookupSyncManager`.

1. Đọc **0xF050** (module thật đang cắm). Rỗng thì log "0xF050 reported no modules - nothing to configure."
2. Ghi **0xF030** (cấu hình module) rồi đọc lại: "{n} module(s) detected, {k}/{n} written to 0xF030".
3. Tra **bề rộng bit từng slot từ ESI**. Thiếu thì log "ESI widths missing for {k}/{n} slot(s) - leaving SM assignment at the coupler default". Đây chính là lỗi lần chạy thật 2026-08-20, khi `DriverInfo` chưa có cạnh exe.
4. Ghi mapping cho các PDO không cố định ("map:0x%04X(%d entries)"); PDO `Fixed` thì bỏ qua ("fixed, not written"). Slot được dời index theo `slotIndexOffset`/`slotPdoIncrement`.
5. Gán **0x1C13 / 0x1C12**: "assigned SM3={n} PDO(s) (wkc=…), SM2={m} PDO(s) (wkc=…)".
6. **Sửa SM:** SM nào chưa có cấu hình thì lấy địa chỉ và control byte từ catalog ESI ("SM{n} from ESI @0x… ctrl=0x…"), rồi đọc lại state/AL status.
7. Gom mọi lỗi SDO vào một dòng log cho slave ("Slave {n} SDO errors: …").

**Tính kích thước:** DLL có symbol `ecx_userExtraIbits`/`ecx_userExtraObits`, không có trong SOEM gốc. Như vậy PGM đã **vá SOEM** để cộng bề rộng module từ ESI vào `Ibits`/`Obits`, vì coupler không trả mapping qua SDO (mục 4.4).

### 4.3 Process image và ánh xạ bit

- SM3 = 4 byte **Device Status 0xF100:01** trước, rồi các slot theo thứ tự. SM2 = các slot DO.
  - Native giữ offset byte riêng cho từng slot (`s_slotInByteOffset`, `s_slotOutByteOffset`, `s_couplerStatusBytes`).
- Lọc entry theo dải index (0x6000–0x6FFF vào, 0x7000–0x7FFF ra, bỏ 0xFxxx). Lỗi cũ từng biến Device Status thành DI 0–31 (`HISTORY.md:36-43`).
- **Mỗi slot là một thanh ghi 32 bit riêng** (`IoSlotState.InputRegister/OutputRegister`).
  - Phía app nối kênh các slot theo thứ tự `SlotIndex` để ra chỉ số DI/DO toàn cục (`Real/EtherCatIoDevice.cs:31-37, 127-145`).
  - `SetSlotOutput` đọc–sửa–ghi thanh ghi của slot đó.
- **Device Status ≠ 0 thì raise alarm** "IO disconnected" kèm giá trị hex (`Real/EtherCatBusManager.cs:54-55`).
- Đã sửa: 64 DI không vừa `uint32`, cần `uint64` hoặc mảng; trần 32 entry PDO bị tràn (`HISTORY.md:45-54`).

### 4.4 Đặc tính R1EC mà PGM phát hiện

Lấy từ chuỗi của công cụ chẩn đoán; chi tiết ở `SLAVES_LEADSHINE.md` mục 4.4.

- Coupler **không trả object PDO mapping qua SDO**, nên SOEM không tự tính được kích thước và cần bề rộng từ ESI.
- **0xF030 chỉ nhận một lần mỗi lần bật nguồn.** Kết nối lại mà không tắt nguồn coupler thì dữ liệu module có thể không vào process image.
- Sau khi ghi 0xF030, coupler tự đặt lại độ dài SM, nên PGM **ghi lại độ dài SM2/SM3 theo rack**.
- Chưa rõ firmware R1EC dùng biến thể PDO bit (0x1A00) hay word (0x1A01).
- Có thể đọc **0xF000** để lấy khoảng cách index slot và số module tối đa.
- Có thể kênh của slot **không đọc được qua SDO**.
- Đường cuối cùng có chế độ **"direct SM access"**: đọc/ghi trực tiếp SM3/SM2 bằng FPRD/FPWR. Kết quả trên máy thật không được ghi lại.

### 4.5 Công cụ chẩn đoán R1EC trong DLL (`--iotest`)

Các bước suy từ chuỗi; nên làm lại thành lệnh console trên ESP32 cho ngày đầu có phần cứng:
1. **BEFORE:** in Ibits/Obits của SOEM; trạng thái 0xF030 (thiết bị còn "trinh" trong lần bật nguồn này không); 0x1C00, 0xF000, gán PDO mặc định và mapping của nó.
2. Dump thanh ghi SM (0x0800+) và FMMU (0x0600+) của ESC; object dictionary qua SDO Info.
3. Ghi 0xF030, đọc lại ("DID NOT STICK" nếu không ăn); in độ dài SM ngay sau khi ghi.
4. **SM LENGTH CORRECTION:** so độ dài SOEM lập trình với độ dài theo rack, rồi ghi lại.
5. **Lấy mẫu trực tiếp** ở SAFE-OP rồi OP trong lúc người vận hành bật/tắt một đầu vào thật. So ba nguồn:
   - kênh qua SDO (biến thể bit và word);
   - process image;
   - SM3 thô đọc bằng FPRD.
6. **Kết luận tự động:**
   - SM3 thô có đổi mà image không đổi: lỗi phía master, cần bề rộng từ ESI.
   - Không có gì đổi: lỗi phía coupler/module (kiểm tra cắm module, nguồn field, thử rack 1 module, liên hệ Leadshine).

## 5. Áp dụng cho firmware ESP32-P4 (SOEM v2)

| Việc PGM làm | Cách làm trên firmware |
|---|---|
| Bảng slave mong đợi; kiểm tra Vendor/Product, không ép Revision | Giữ nguyên. Kiểm tra sau `ecx_config_init`; sai thì không vào OP và hiện ở SETTING › ETHERCAT. |
| Kết quả từng slot (Ok / WrongDriver / NoResponse / PdoUnsupported) | Giữ nguyên enum; đưa vào snapshot cho HMI. |
| Đọc 0x608F lúc scan, tính CountsPerUnit | Giữ nguyên (SDO trên worker core 0, lúc PRE-OP). |
| Ghi 0x607D (đọc lại), 0x6092, 0x6098/0x6099 bằng SDO | Ghi ở PRE-OP trong hook hoặc worker; **không** làm trong task chu kỳ trên core 1. |
| Mode và tốc độ bằng SDO mỗi lệnh move; xung new set-point tính bằng ms | **Remap 0x1600** để 0x6060, 0x6081, 0x6083 và 0x6084 đi qua PDO (`SLAVES_LEADSHINE.md` mục 6). Handshake new set-point làm trong task chu kỳ: 1 chu kỳ lên, chờ bit 12 ack, rồi xoá. |
| Tìm offset object từ mapping thật | Giữ nguyên (drive trả mapping qua SDO được). Nếu remap thì layout cố định và biết trước. |
| Không bật SYNC0 (PP Free Run) | Giai đoạn đầu giống vậy; DC/SYNC0 khi cần CSP. |
| R1EC: hook PO2SO + bề rộng từ ESI + vá SOEM (`ecx_userExtra*`) | Hook PO2SO và bảng module biên dịch sẵn từ ESI, kèm fork nhỏ `ec_config.c` trong `port/` (`SLAVES_LEADSHINE.md` mục 4.4 (a)). |
| Device Status ≠ 0 thì alarm | Thêm: mất slave IO hoặc WKC sai cũng coi là lỗi an toàn. |
| Công cụ `--iotest` | Làm lệnh console `r1ec_diag` cho ngày bring-up. |
| Kết nối lại thì bắt về gốc | Giữ nguyên. |

## 6. Chưa xác minh

- PGM chưa ghi lại kết quả cuối cùng trên phần cứng: hook R1EC và chế độ "direct SM access" có chạy ổn không; `HISTORY.md` dừng ở P3a.
- Drive L5N/CL3B ở PP không có SYNC0 có ổn định không: PGM chưa nghiệm thu P3b với drive thật.
- Ý nghĩa `0x8000:x = 0xFFFF` (trạng thái DO khi mất link): cần manual R1EC.
