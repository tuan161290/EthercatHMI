# SLAVES_LEADSHINE.md — Slave EtherCAT theo ESI (Leadshine, Fastech)

Tổng hợp từ các file ESI trong `vendor/ESI/` (chỉ đọc), đối chiếu với project tham khảo `PGM/`
(xem `docs/REFERENCE_PGM.md`). Mọi số liệu dưới đây lấy từ ESI và ghi rõ file + dòng. Những gì ESI
không nói và chưa thử trên phần cứng đều đánh dấu **(chưa xác minh)**.

Viết tắt file:
- `L5N` = `vendor/ESI/Leadshine/L5N_V1.01.xml`
- `L3N` = `vendor/ESI/Leadshine/L3N_V1.00.xml`
- `CL3B` = `vendor/ESI/Leadshine/CL3B-EC_V1.08 XML.xml`
- `R1EC` = `vendor/ESI/Leadshine/R1EC/R1EC-v1.4.0.0.xml`

## 1. Định danh

Vendor ID Leadshine = **0x4321** (= 17185 thập phân; L5N:5, R1EC:5).

| Thiết bị | Loại | Product code | Revision | Nguồn |
|---|---|---|---|---|
| L5N (servo, vd L5N-400S) | Servo drive CiA402 | 0x000000F0 | 1 | L5N:21 |
| L3N | Servo drive CiA402 | 0x000000F8 | 1 | L3N:20 |
| CL3B-EC507 | Stepper closed-loop CiA402 | 0x00000800 | 0x0001 | CL3B:27 |
| CL3B-EC808AC | Stepper closed-loop CiA402 | 0x00000900 | 0x0001 | CL3B:5003 |
| CL3B-EC503 | Stepper closed-loop CiA402 | 0x00000A00 | 0x0001 | CL3B:9979 |
| R1EC | Coupler remote IO dạng module (MDP) | 0x61400015 | 0x22092010 | R1EC:19 |

Để tham khảo: máy trong `PGM/` dùng 6× L5N-400S, 2× CL3B-EC507 và 1× R1EC với 2× SC-3200-1 + 1× SC-0032-N-1 (`PGM/PartronCamTestAuto.Core/Core.Data/SystemData.cs:98-154`).
- **Máy đích của firmware này không phải máy PGM.** Model, số trục và rack IO thật sẽ chốt khi có phần cứng.
- Ở SOEM slave đánh số từ 1, còn PGM đánh từ 0.

## 2. Servo L5N / L3N

L5N và L3N có cùng cấu trúc PDO, SM và DC; chỉ khác product code.

**Sync manager và mailbox** (L5N:6656-6659)
- SM0 MBoxOut 0x1000, SM1 MBoxIn 0x1100, mặc định 124 byte (40–256).
- SM2 Outputs 0x1200, SM3 Inputs 0x1400, tối đa 100 byte mỗi SM.

**CoE** (L5N:6875): `SdoInfo=false`, `CompleteAccess=false`, `SegmentedSdo=false`, `PdoAssign=true`, `PdoConfig=true`.
- Không có SDO Info, nên không duyệt được object dictionary từ HMI; danh sách object phải lấy từ ESI.
- Không có Complete Access, nên SOEM đọc PDO mapping theo từng sub-index (chậm hơn nhưng chạy được).
- Không có segmented SDO, nên mỗi SDO tối đa 4 byte (expedited).

**InitCmd** (L5N:6876): ở PS ghi `0x6060 = 8` (**CSP**). ESI mặc định cho drive chạy CSP.

**DC** (L5N:6888-6898): hai chế độ, `DC-Synchron` (AssignActivate 0x0300, tức SYNC0) và `Free Run` (0x0000).
CSP có chạy được ở Free Run hay không thì **(chưa xác minh)**; trên thực tế CSP luôn đi cùng SYNC0.

**PDO mặc định** (gán sẵn vào SM)

| PDO | Nội dung | Kích thước |
|---|---|---|
| RxPDO 0x1600 → SM2 (L5N:6660) | 0x6040 controlword, 0x607A target position, 0x60B8 touch probe function | 64 bit = **8 byte** |
| TxPDO 0x1A00 → SM3 (L5N:6809) | 0x503F servo error code, 0x603F error code, 0x6041 statusword, 0x6061 mode display, 0x6064 position actual, 0x60B9 touch probe status, 0x60BA touch probe pos1, 0x60FD digital inputs | 168 bit = **21 byte** |

**PDO thay thế** (không gán sẵn, chọn bằng 0x1C12):
- 0x1601: CSV, gồm 6040, 60FF target velocity, 60B2, 6060.
- 0x1602: CST, gồm 6040, 6071, 6087, 6060.
- 0x1603: tham số homing, gồm 6040, 6098, 6099:1/2, 609A, 607C, 6060.
- Ba PDO này loại trừ lẫn nhau. Nguồn: L5N:6686-6808.

**Object map được vào PDO** (cờ `PdoMapping` trong ESI): 6040, 6060, 607A, 6081, 6083, 6084, 6098, 60B8, 60FF (RxPDO); 6041, 6061, 6064, 606C, 6077, 60F4, 60FD, 603F, 503F (TxPDO).
- Vì vậy có thể **tự map lại 0x1600** để chạy PP/HM/CSP mà không cần đổi mode bằng SDO.
- PGM không làm việc này mà dùng PP qua SDO, xem mục 6.

**Object hãng đáng chú ý**
- 0x503F servo error code (mã lỗi Leadshine, hiển thị trên ALARM).
- 0x5400 DC Period Setting.
- 0x2000–0x27xx là tham số Pxx.xx của drive (gain, notch, tỉ số truyền P00.08–P00.10, ...).
- Danh sách đầy đủ ở phần Dictionary của ESI.

## 3. Stepper CL3B-EC (507 / 808AC / 503)

- **Sync manager và mailbox** (CL3B:4730-4733): mailbox 64 byte (6–128); SM2 và SM3 tối đa 100 byte.
- **CoE** (CL3B:4971): giống L5N (không SdoInfo, không CA, không segmented). InitCmd PS `0x6060 = 8` (CSP).
- **DC**: giống L5N (SYNC0 0x0300 hoặc Free Run).
- **PDO mặc định:**
  - RxPDO 0x1600: 6040, 607A, 60B8 → **8 byte**.
  - TxPDO 0x1A00: 603F, 6041, 6061, 6064, 60B9, 60BA, 60FD → 152 bit = **19 byte**. Không có 0x503F.
- **PDO thay thế, khác L5N:**
  - 0x1601 là bộ **PP đầy đủ**: 6040, 607A, 6081 profile velocity, 6083, 6084, 6060 (152 bit).
  - 0x1602: PV.
  - 0x1603: homing.
  - Nguồn: CL3B:4763-4909.

## 4. Remote IO R1EC (coupler dạng module)

### 4.1 Coupler

- **Sync manager và mailbox** (R1EC:971-974): mailbox 0x1000 / 0x1400, 128 byte; Outputs 0x1800, Inputs 0x1C00, `DefaultSize=0`.
- **CoE** (R1EC:987): `SdoInfo=1`, `PdoAssign=true`, `PdoConfig=true`, `PdoUpload=false`, `CompleteAccess=0`.
- **DC** (R1EC:990-1007): SM-Synchron hoặc DC-Synchron (0x0300). IO không cần DC.
- **TxPDO cố định 0x1BFA "Device Status"** (R1EC:975-985): `0xF100:01` DINT 32 bit trên SM3. Đây là chẩn đoán của coupler (module mất hay lỗi), **không phải kênh DI**.
- **Bắt buộc master tải danh sách module:**
  - `DownloadModuleList="true"` (R1EC:19).
  - `<Slots ... IdentifyModuleBy="ModuleIdent" DownloadModuleIdentList="1">` (R1EC:1009).
  - Object liên quan: `0xF030` Configured Module Ident List (R1EC:737) và `0xF050` Detected Module Ident List (R1EC:852).
- **Đánh số theo slot:** `SlotIndexIncrement = SlotPdoIncrement = 0x10` (R1EC:1009), và mọi index của module đều có `DependOnSlot="true"` (vd R1EC:1059, 1063, 1191).
  - Module ở slot *n* dùng `0x1A00 + n·0x10`, `0x1600 + n·0x10`, `0x6000 + n·0x10`, `0x7000 + n·0x10` và `0x8000 + n·0x10` (cấu hình).
  - Slot đầu tiên là n = 0 hay n = 1 thì **(chưa xác minh)**: đọc `0x1C13` trên máy thật để chốt.

### 4.2 Module (trích những loại liên quan; ESI có 20 loại)

| Module | Ident | PDO mặc định (Fixed) | PDO gọn thay thế | InitCmd PS |
|---|---|---|---|---|
| **SC-3200-1** (32 DI, MIL) | 0x71101045 | 0x1A00: 32× BOOL `0x6000:1..32` | 0x1A01: 2× UINT `0x6001:1/2` | `0x8000:3..6 = 0x0A` (lọc DI theo nhóm 8 bit) |
| **SC-0032-N-1** (32 DO NPN, MIL) | 0x71101405 | 0x1600: 32× BOOL `0x7000:1..32` | 0x1601: 2× UINT `0x7001:1/2` | `0x8000:1/2 = 0xFFFF` ("out bit state when link lost") |
| SC-1600 (16 DI) | 0x71100025 | 16× BOOL | 1× UINT | lọc 0x0A |
| SC-3200 (32 DI) | 0x71100045 | 32× BOOL | 2× UINT | lọc 0x0A |
| SC-0016-N / -P (16 DO) | 0x71100205 / 0x71110205 | 16× BOOL | 1× UINT | link-lost 0xFFFF |
| SC-0032-N / -P (32 DO) | 0x71100405 / 0x71110045 | 32× BOOL | 2× UINT | link-lost 0xFFFF |
| SC-1616-N / -P (16 DI + 16 DO) | 0x71100225 / 0x71110225 | 16+16 BOOL | UINT | cả hai |
| SC-0808-N (8 DI + 8 DO) | 0x71100115 | 8+8 BOOL | USINT | cả hai |
| SC-0016-R (16 relay) | 0x71900205 | 16× BOOL | 1× UINT | link-lost 0xFFFF |
| SC-A0400-IV (4 AI) | 0x71000025 | 4× INT + 4× chẩn đoán | — | cấu hình kênh |
| SC-A0004-IV (4 AO) | 0x71000205 | 4× INT; TxPDO chẩn đoán | — | cấu hình + giá trị khi mất link |

Nguồn: R1EC:1053 trở đi, mục `<Modules>`. Bảng đầy đủ ở bản trích tự động; xem mục 7 để tạo lại.

**Ý nghĩa `0x8000:x = 0xFFFF` "state when link lost"** của module DO: ESI không giải thích, nên **(chưa xác minh)**.
- Có thể là mặt nạ "giữ nguyên trạng thái" hoặc "về 0" cho từng bit.
- Phải tra manual R1EC trước khi dùng, vì đây là hành vi an toàn của van khí khi mất bus.
- 0xFFFF cũng là giá trị mặc định trong ESI (R1EC:2037), nên firmware ghi nó không làm thay đổi gì so với để nguyên. **Đây chưa phải quyết định an toàn**: cần chọn giá trị sau khi tra manual và làm phép thử ngày đầu (SETUP.md mục 7), rồi sửa trong generator.
- Kích thước InitCmd: dữ liệu trong ESI có `AdaptAutomatically` và ngắn hơn object (vd lọc DI ghi `0a` 1 byte, object `0x8000:3` là UINT 16 bit, R1EC:1193 và 1279-1289). Generator lấy kích thước từ dictionary của module để CoE không từ chối.

### 4.3 Ví dụ process image: rack của PGM (2× SC-3200-1 + SC-0032-N-1)

Máy đích của firmware này không phải máy PGM; rack thật sẽ khác. Bảng này khớp với `PGM/HISTORY.md:24-34`, nơi được tính từ ESI.

```
SM3 Inputs  12 byte: [0..3] 0xF100:01 Device status | [4..7] SC-3200-1 #1 (32 DI) | [8..11] SC-3200-1 #2 (32 DI)
SM2 Outputs  4 byte: [0..3] SC-0032-N-1 (32 DO)
```

### 4.4 Hệ quả cho SOEM (quan trọng)

**Hiện tượng.** Nếu để SOEM tự cấu hình như với các slave khác, R1EC chỉ lộ ra Device Status:
- SOEM đọc mapping qua CoE, tức `0x1C00`, `0x1C12`, `0x1C13` (`SOEM/src/ec_coe.c:856-930`).
- Nếu không đọc được thì SOEM rơi về PDO trong SII, mà SII của R1EC chỉ khai báo `0x1BFA` (`SOEM/src/ec_config.c:555-600`).
- Kết quả là **Ibytes=4, Obytes=0**. PGM đã gặp đúng triệu chứng này khi chạy thật ngày 2026-08-20 (`PGM/.../Real/EtherCatBusManager.cs:135-141`).

**Những gì PGM đã tìm ra**

Nguồn: chuỗi log và tên hàm trong `PGM/.../bin/Debug/net48/EtherCatMaster.Native.dll`. Mã nguồn không có, nên đây là suy ra từ chuỗi; xem `docs/REFERENCE_PGM.md` mục 4.

- **R1EC không trả object PDO mapping qua SDO.** Log chẩn đoán của PGM ghi: SOEM "sizes the image from PDO mapping objects this coupler refuses to serve. Fix: supply the module bit counts from the ESI instead of reading them".
  - Vì vậy SOEM **không tự tính được** kích thước SM2/SM3, kể cả khi đã gán `0x1C12`/`0x1C13` đúng.
  - Bề rộng bit của từng module phải lấy từ ESI (bảng 4.2).
- **`0xF030` chỉ "ăn" một lần mỗi lần bật nguồn.**
  - PGM có kiểm tra "DEVICE VIRGIN THIS POWER CYCLE", "<<< DID NOT STICK", "POWER-CYCLE the coupler before the next test".
  - Có cả cảnh báo "coupler kept SM3/SM2 … module data is NOT in its process image (a coupler that has already been configured since power-up does this; power-cycle it and connect once)".
  - Nghĩa là kết nối lại mà không tắt nguồn R1EC có thể làm mất dữ liệu module.
- **Sau khi ghi `0xF030`, coupler tự đặt lại độ dài SM.** PGM đọc lại thanh ghi SM của ESC và **ghi lại độ dài SM2/SM3 theo rack** ("SM LENGTH CORRECTION … rewritten from rack").
  - Nếu SII không có SM nào thì PGM lấy địa chỉ và control byte từ ESI: SM2 0x1800 / 0x64, SM3 0x1C00 / 0x20.
- **PGM không chắc firmware R1EC dùng biến thể PDO nào:** bit (0x1A00, 32× BOOL) hay word (0x1A01, 2× UINT). Công cụ chẩn đoán có một bước để xác định điều này.
- **`0xF000`** (Modular Device Profile): đọc được khoảng cách index giữa các slot (kỳ vọng 0x10) và số module tối đa. Nên đọc ở runtime thay vì hard-code.
- **Thứ tự byte của SM3:** byte 0–3 là Device Status, DI của **slot 0** bắt đầu ở byte 4. Như vậy slot đếm từ 0, nhưng điều này **(chưa xác minh trên phần cứng)**.
- **Đường cuối cùng của PGM có chế độ "direct SM access"** ("OK (modular coupler, direct SM access)"): đọc và ghi trực tiếp vùng nhớ SM3/SM2 của R1EC bằng FPRD/FPWR, bỏ qua FMMU.
  - PGM không ghi lại kết quả cuối cùng trên phần cứng; `HISTORY.md` dừng ở P3a, chưa nghiệm thu P3b.

**Cách làm cho firmware này**

Đã code ngày 2026-10-03 theo phương án (a). **Chạy được trên phần cứng ngày 2026-10-11** (kết quả ở mục 4.5).
- Hook nằm trong `firmware/components/ecat/ecat_coupler.c`.
- Bản vá nằm trong `firmware/components/soem/port/ec_config.c`.

1. Đăng ký hook `PO2SOconfig` cho slave R1EC. SOEM gọi hook này trước khi đọc mapping (`ec_config.c:519-521`). PGM cũng có đúng hàm `PreOpToSafeOpConfig`.
2. Trong hook:
   - Đọc `0xF000` và `0xF050:0..n`.
   - So với cấu hình mong đợi (bảng ident ở mục 4.2). Nếu khác thì báo "IO module mismatch" và không vào OP.
   - Đọc `0xF030`. Nếu đã đúng danh sách thì **không ghi lại**. Nếu khác và không ghi được thì báo "power-cycle R1EC".
     - Theo ESI, `0xF030:0` là **chỉ đọc**, chỉ 16 entry là ghi được (R1EC:511-545). Firmware vẫn thử ghi sub0 (0 trước, n sau) nhưng bỏ qua nếu bị từ chối.
     - "Đúng danh sách" nghĩa là entry 1..n bằng rack và entry n+1 bằng 0 (hoặc không đọc được). Không dựa vào sub0. Log in giá trị sub0 để ngày đầu biết coupler có giữ số đếm hay không.
     - Rack tối đa 16 module (mảng 0xF030/0xF050 trong ESI).
   - Ghi InitCmd của từng module: lọc DI, trạng thái khi mất link.
   - Ghi `0x1C12` = {0x1600 + n·0x10 của các module DO} và `0x1C13` = {0x1BFA, 0x1A00 + n·0x10 của các module DI}. Ghi sub0 = 0, ghi từng entry, rồi ghi sub0 = số entry.
3. **Kích thước lấy từ ESI, không đọc từ slave.** SOEM luôn ghi đè `Obits`/`Ibits` sau hook (`ec_config.c:549-550`), nên cần một trong hai cách:
   - **(a) Đề xuất:** chép `ec_config.c` sang `port/` và thêm điểm chèn kích thước: hook điền `Ibits`/`Obits` + `SM[2]`/`SM[3]`, và SOEM giữ nguyên khi đọc CoE ra 0.
     - Đây là fork một file, phải ghi vào `SOEM_UPSTREAM.md`.
     - R1EC vẫn nằm trong frame LRW chung, nên không tốn thêm datagram.
     - PGM có vẻ cũng làm thế: DLL native có hai symbol `ecx_userExtraIbits`/`ecx_userExtraObits`. Hai symbol này **không có trong SOEM gốc**, nên PGM đã vá SOEM để cộng thêm bit cho slave (suy ra từ tên; không có mã nguồn).
   - **(b) Cách của PGM:** đưa R1EC ra khỏi mapping LRW và đọc/ghi SM trực tiếp (FPRD 0x1C00, FPWR 0x1800) mỗi chu kỳ. Không phải sửa SOEM, nhưng tốn thêm 2 datagram mỗi chu kỳ và phải tự lo watchdog SM2.
   - ENI (`eniconv.py`) không giúp được: ENI của SOEM v2 chỉ phát lại InitCmd CoE (`ec_main.c:1784`), không đặt kích thước.
4. Sau khi vào SAFE-OP, đọc lại thanh ghi SM2/SM3 của ESC (0x0810/0x0818). Nếu coupler đã tự đổi độ dài thì sửa theo rack, như PGM làm.

**Ngày đầu có phần cứng, cần chạy chẩn đoán** (tương tự công cụ của PGM):
- in `0xF000`, `0xF050`, `0xF030`, `0x1C00`, `0x1C12`, `0x1C13`;
- thử đọc mapping `0x1A00`/`0x1A10`;
- đọc thô SM3 bằng FPRD trong lúc bật/tắt một đầu vào thật, để xác định byte nào là slot nào và biến thể bit hay word;
- bật vài đầu ra rồi rút cáp EtherCAT ở cổng IN của R1EC, xem đầu ra giữ nguyên hay tắt, để chốt ý nghĩa `0x8000:x` "state when link lost" (mục 4.2);
- timeout mailbox của R1EC trong ESI là 10 s (R1EC:33-34), còn SOEM chờ SDO 0,7 s (`EC_TIMEOUTRXM`). Nếu ghi `0xF030` hay gán PDO báo timeout thì tăng timeout cho coupler.

### 4.5 Kết quả trên phần cứng (2026-10-11)

Bus thật: 7× L5N + 2× CL3B-EC507 + R1EC (rev 0x22092010) ở vị trí 10. Rack: slot 0 và 1 là SC-3200-1 (0x71101045), slot 2 là SC-0032-N-1 (0x71101405), giống rack của PGM.

**Lỗi lần scan đầu: "Rack unreadable, 0xF050 is not readable".** Đây là lỗi của firmware, không phải của R1EC:
- `ecx_statecheck(ctx, 0, PRE_OP)` chỉ ghi trạng thái vào `slavelist[0]`.
- `ecx_mbxsend()` của SOEM v2 không gửi mailbox cho slave nào mà trạng thái **đã ghi của chính nó** dưới PRE-OP (`SOEM/src/ec_main.c`, `ecx_mbxsend`). Sau `ecx_config_init()` mọi slave vẫn ghi là INIT, nên lệnh đọc SDO bị từ chối ngay, không gửi ra bus.
- Sửa trong `ecat_scan.c`: gọi `ecx_readstate()` sau bước kiểm tra PRE-OP. Mọi đoạn code đọc SDO ngay sau `ecx_config_init()` đều phải làm như vậy.

**Đọc bằng `sdo 10 rack` lúc vừa bật nguồn, ở PRE-OP:**

| Object | Giá trị |
|---|---|
| `0xF000` | sub1 = 0x10 (khoảng cách index slot), sub2 = 0x20 (tối đa 32 module, trong khi mảng trong ESI có 16 entry) |
| `0xF050` | 3 entry: 71101045, 71101045, 71101405 |
| `0xF030` | rỗng (sub0 = 0) |
| `0x1C00` | 1 2 3 4 |
| `0x1C12`, `0x1C13` | rỗng: sau khi bật nguồn không có PDO nào được gán sẵn, kể cả 0x1BFA |

**Hook cấu hình chạy được ngay lần đầu:**
- Log: `0xF030 written (sub 0 = 3), InitCmd errors 0, PDO assign ok, in 96 bits, out 32 bits`.
- Ghi entry xong thì **sub0 tự thành 3**, tức coupler có giữ số đếm.
- Kích thước SM lấy từ ESI được chấp nhận (vào 12 byte, ra 4 byte), bus vào OP với WKC 30/30.
- Device Status = 0. DI của slot 0 và slot 1 có bit ON (cảm biến trên máy), DO đều 0.
- Cả bus 10 slave: 211 byte ra + 179 byte vào; ở chu kỳ 1 ms round trip khoảng 125 µs, jitter 1 µs.

**Khởi động lại master mà không tắt nguồn R1EC (đã thử 2 lần liên tiếp, 2026-10-11):**
- Mỗi lần hook đều báo `0xF030 written (sub 0 = 3)`. Tức là sau khi bus về INIT thì 0xF030 lại trống, và coupler nhận lại danh sách mà **không cần tắt nguồn**.
- Bus vào OP, Device Status = 0, DI giống hệt trước khi khởi động lại.
- Vậy hiện tượng "0xF030 chỉ ăn một lần mỗi lần bật nguồn" mà PGM gặp (mục 4.4) không xảy ra với firmware R1EC này và trình tự cấu hình của firmware này.
- Lưu ý: các drive (L5N và CL3B) báo lỗi 0x821B (vendor 0x081B) mỗi khi master khởi động lại lúc bus đang OP. Nhấn Reset ở trang Alarms là hết.

**Còn phải xác minh:**
- Bật/tắt một đầu vào đã biết để chốt slot 0 bắt đầu ở byte 4 và thứ tự bit (IO monitor).
- Ý nghĩa `0x8000:x` "state when link lost" (mục 4.2).

## 5. Ví dụ dung lượng process image (cấu hình PGM, PDO mặc định)

| | Out (byte) | In (byte) |
|---|---|---|
| 6× L5N | 6 × 8 = 48 | 6 × 21 = 126 |
| 2× CL3B-EC507 | 2 × 8 = 16 | 2 × 19 = 38 |
| R1EC (2 DI + 1 DO module) | 4 | 12 |
| **Tổng** | **68** | **176** |

- 244 byte nằm gọn trong một frame LRW. Frame khoảng 300 byte mất khoảng 25 µs truyền ở 100 Mbit/s, chưa tính trễ qua slave.
- Ước lượng IOmap cần dưới 512 byte. Mailbox mặc định của các slave ≤ 128 byte.
- `CONFIG_SOEM_MAXSLAVE = 32` là dư.

## 6. Chế độ điều khiển trục: PGM làm gì, firmware này nên làm gì

**PGM** chạy trên Windows/Npcap nên không có thời gian thực. Nó dùng:
- **Profile Position (PP)**: drive tự sinh quỹ đạo.
  - PDO giữ mặc định, chỉ có 6040/607A.
  - 0x6060, 0x6081, 0x6083 và 0x6084 được ghi bằng **SDO** (hằng `LeadshineDrives.SDO_ONLY_OBJECTS_NOTE` trong `EtherCatMaster.dll`).
  - Mỗi move phát xung bit 4 "new set-point" (`EtherCatConfig.NewSetpointPulseMs`, `ClearNewSetpoint`).
  - Chi tiết ở `docs/REFERENCE_PGM.md` mục 3.
- **Jog** = PP tới điểm rất xa theo chiều jog (soft limit làm đích), dừng bằng Halt. PGM chọn cách này để khỏi phải remap PDO (`PGM/HISTORY.md:81-88`).
- **Homing (HM)**: chờ statusword bit 12 (attained) / bit 13 (error).

**Firmware ESP32-P4.** Có vòng 1 ms tất định nên có hai hướng:

| | PP + HM (như PGM) | CSP + DC (ESI mặc định) |
|---|---|---|
| Quỹ đạo | Drive tự tính | ESP32 phải tính mỗi chu kỳ (trapezoid hoặc S-curve) |
| DC/SYNC0 | Không bắt buộc (Free Run) | Bắt buộc, cần PI loop `ec_sync` |
| Nhiều trục phối hợp (nội suy XY, chạy theo đường) | Không đồng bộ chặt | Có |
| Mức rủi ro bring-up | Thấp; logic đã chạy trong PGM | Cao hơn |
| PDO cần | Remap 0x1600: 6040, 607A, 6081, 6083, 6084, 6060 (ESI cho phép, mục 2) | Mặc định 0x1600 đã đủ (6040, 607A) |

**Đề xuất:**
- **Giai đoạn đầu dùng PP + HM**, remap 0x1600 trong hook `PO2SOconfig`. Nhờ vậy đổi mode và tốc độ qua PDO, không phải chờ SDO.
  - Mục tiêu RxPDO: 6040, 607A, 60FF, 6081, 6083, 6084, 6060.
  - Mục tiêu TxPDO: 603F, 6041, 6061, 6064, 606C, 60FD, cộng 503F (chỉ L5N).
- Chỉ chuyển sang **CSP + DC** khi máy thật có trục cần nội suy.
- PGM chỉ có `MoveLinear` hai trục bằng PP, chia tốc độ theo tỉ lệ (Vx, Vy). Cách này không phải nội suy thật.

Đây là đề xuất; chờ người dùng chốt khi có máy thật.

**Đã code giai đoạn đầu (2026-10-10, `firmware/components/ecat/ecat_drive.c`, chưa chạy với drive thật):** PP + PV + HM, không DC. Bố cục PDO dùng chung cho mọi drive:

| PDO | Entry (theo thứ tự) | Kích thước |
|---|---|---|
| RxPDO 0x1600 | 6040, 607A, 60FF, 6081, 6083, 6084, 6060 | 184 bit = 23 byte |
| TxPDO 0x1A00 | 6041, 603F, 6061, 6064, 606C, 60FD, cộng 503F (chỉ servo Leadshine) | 136 / 152 bit |

- Số entry nằm trong giới hạn của ESI: tối đa 10 với L5N/L3N, 8 với CL3B (`DT1600`/`DT1A00`).
- Mọi object trên đều có cờ `PdoMapping` trong ESI.
- Drive trả mapping qua SDO, nên SOEM tự tính kích thước SM sau hook; không cần bản vá như R1EC.
- **Đã chạy với CL3B-EC507 thật (2026-10-10, alias 0, nhận theo vị trí trên dây là AX1):**
  - Drive nhận remap ở PRE-OP. Log: `PDO remapped (PP/PV/HM); encoder 4000/1, gear 1:1, feed 10000/1, homing method 19`.
  - SOEM map 23 byte ra + 17 byte vào (CL3B không có 503F), WKC 3/3, RTT khoảng 80 µs ở chu kỳ 1 ms.
  - Servo on và jog chạy được: vị trí đổi từ 0,07 lên 2,77 vòng trong lúc người dùng thao tác.
  - Khi master bị reset lúc đang OP (nạp firmware), drive báo lỗi **0x821B** (statusword 0x0638). **Alarm reset** xoá được lỗi này và lỗi không quay lại. Có thể đây là lỗi mất giao tiếp/watchdog của drive; ý nghĩa chính xác **(chưa xác minh)**, cần tra manual CL3B.
- **(chưa xác minh)**:
  - đơn vị của 0x606C;
  - số units/vòng: firmware tính ra 10000 units/vòng từ feed 10000/1, nhưng encoder báo 4000 inc/vòng. Cần đánh dấu trục motor rồi chạy `axis 1 rel 1r` xem có đúng một vòng không;
  - phần này với L5N/L3N.

**Nội suy 2 trục bằng CSP (code 2026-10-12, `ecat_axis.c`, chưa chạy với drive thật).** Người dùng muốn thử nội suy cho từng cặp trục (trang Manual › Interpolation, `docs/SETUP.md` 7e).
- Không đổi bố cục PDO: RxPDO ở trên đã có 607A và 6060, nên đổi mode sang CSP (6060 = 8) qua PDO là đủ.
- Master tính đích 607A của hai trục mỗi chu kỳ từ một biên dạng tốc độ hình thang dọc theo đường (thẳng hoặc cung tròn). Bước thời gian là chu kỳ danh định (`CONFIG_APP_ECAT_CYCLE_US`).
- Trước khi chạy, đích của mỗi trục được đặt bằng vị trí thật, rồi chờ 0x6061 = 8 (tối đa 200 ms). Hết đường thì giữ đích cuối 50 ms rồi trở về PP.
- Bus vẫn **Free Run, không DC** và không ghi 0x60C2. Drive có chấp nhận CSP khi không có SYNC0 không thì **chưa xác minh**; ESI mặc định CSP đi kèm `DC-Synchron` (mục 2).
  - Nếu drive không vào CSP hoặc chạy giật, bước tiếp theo là bật DC: `ecx_dcsync0()` cho các drive, và vòng PI kéo chu kỳ của master theo đồng hồ DC (như `ec_sync` trong ví dụ của SOEM).
- An toàn: lỗi drive, rớt servo hoặc WKC sai trên một trục thì trục còn lại giảm tốc dọc theo đường rồi dừng. Đường không dùng halt (bit 8).

## 7. Tạo lại bản trích

Bản trích ESI được sinh bằng script PowerShell đọc XML. Script không lưu trong repo; nếu cần thì viết lại.
Cách làm: duyệt `Descriptions/Devices/Device` và `Descriptions/Modules/Module`, in `Type@ProductCode`, `Sm`, `Mailbox/CoE`, `Dc/OpMode`, `RxPdo`/`TxPdo` (Index, Sm, Entry: Index:SubIndex, BitLen) và `InitCmd`.

## 8. Fastech (có trong `vendor/ESI/Fastech/`, máy PGM không dùng)

- Vendor ID **0x0FA00000**.
- `FASTECH_Ezi-IO_EtherCAT.xml`: 15 thiết bị IO.
  - Loại cũ (0x02002–0x02023): không có CoE, PDO cố định.
  - Loại "A" (0x02104–0x02125): có CoE và DC.
- `FASTECH_Ezi-SERVO2_EtherCAT.xml`: 25 biến thể Ezi-SERVO2 / S-SERVO.
  - Có CoE với `CompleteAccess=1` và InitCmd SO `0x6060 = 8`.
  - PDO chọn giữa "CSP" (0x1600/0x1A00) và "PP" (0x1601/0x1A01).
- Chỉ cần đọc kỹ nếu máy thật có thiết bị Fastech.
