# OV02C10 driver from the vendor package

The camera module on FPC2 uses an OmniVision OV02C10 (docs/BOARD.md 3.8). No esp_cam_sensor release
on the Espressif registry has a driver for it (checked 1.7.0, 2.1.0 and 2.6.0, 2026-10-13). Guition
added one to their copy of esp_cam_sensor 2.1.0:

- Source: `JC1060P470C_I_W_Y.zip` → `JC1060P470C_I_W_Y/1-Demo/Demo_IDF/components/esp_cam_sensor/sensors/ov02c10/`
  (the copy under `video_lcd_display/components/` is identical).
- Their esp_cam_sensor is upstream 2.1.0 (commit `70d2ce41`) plus that folder, the lines that build it
  (CMakeLists.txt, Kconfig, project_include.cmake) and one accessor, `esp_cam_sensor_detect_get_array()`,
  which nothing here uses.

So this component keeps upstream esp_cam_sensor 2.1.0 from the registry and adds the sensor next to it.
Copied without modification:

| File here | Vendor file |
|---|---|
| `ov02c10.c` | `sensors/ov02c10/ov02c10.c` (sha256 `42e75147…ab993`) |
| `Kconfig.ov02c10` | `sensors/ov02c10/Kconfig.ov02c10` |
| `include/`, `private_include/` | the same folders |
| `cfg/ov02c10_default_p4_eco4.json` | IPA tuning for P4 silicon below v3 (this board is v1.3) |

Written here: `CMakeLists.txt`, `Kconfig` (menu around `Kconfig.ov02c10`), `project_include.cmake`
(the vendor's IPA lines for this sensor), `idf_component.yml`.

Facts from the driver (`ov02c10.c`, `include/ov02c10.h`):
- SCCB address 0x36, chip ID 0x5602 (registers 0x300A/0x300B).
- Formats, 24 MHz input clock from the module, RAW10, 30 fps: 1288x728 (1 lane), 1920x1080 (1 lane),
  1920x1080 (2 lanes). The vendor demo `video_lcd_display` uses 1920x1080 on 1 lane
  (its `sdkconfig` lines 2518-2523), with the ISP pipeline controller on.

Known vendor slips, left as they are: `include/ov02c10.h` declares `sc2336_detect()` instead of
`ov02c10_detect()`, and the 1288x728 entry uses a register table named `..._2lane_..._10fps`.
Licence: Apache-2.0 (file header).
