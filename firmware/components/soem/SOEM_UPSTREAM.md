# SOEM upstream

`SOEM/` is a subset of https://github.com/OpenEtherCATsociety/SOEM, copied without modification.

- Commit: `88e8ed46efba7dfa7b94d08a512db25a33e3f8d5` (2026-04-10, "Fix mbxstatus IOmap offset calculation")
- That is v2.0.0 plus 13 bug-fix commits (`git describe`: `v2.0.0-13-g88e8ed4`).
- Included: `src/`, `include/soem/`, `osal/osal.h`, `scripts/eniconv.py`, `LICENSE.md`, `README.md`.
- `samples/` (simple_ng, slaveinfo, ec_sample) is copied for reference only and is not compiled.
- Not included: the linux/win32/rtk OS ports, `contrib/`, docs, CI files.

The ESP-IDF port is in `port/`:

| File | Role |
|---|---|
| `port/osal.c`, `port/osal_defs.h` | OSAL on FreeRTOS + esp_timer; GPTimer-assisted `osal_monotonic_sleep()` |
| `port/nicdrv.c`, `port/nicdrv.h` | Frame index/buffer handling, derived from `oshw/rtk`; single port, blocking RX wait |
| `port/oshw.c`, `port/oshw.h` | esp_eth glue: raw L2 input path, EtherType 0x88A4 filter, RX queue |
| `port/include/soem/ec_options.h` | Replaces the CMake-generated options header; sizes come from Kconfig |
| `port/include/soem_esp.h` | Public API: `soem_esp_attach()`, stats, `soem_esp_set_pdo_layout()` |
| `port/ec_config.c` | **Patched copy** of `SOEM/src/ec_config.c`, compiled instead of it (see below) |
| `port/pdo_layout.c`, `port/soem_port_hooks.h` | Application-supplied PDO layouts used by the patch |

## The one upstream patch

`port/ec_config.c` is `SOEM/src/ec_config.c` at the commit above plus two lines marked `ESP-PORT PATCH`:
an `#include "soem_port_hooks.h"` and, at the end of `ecx_map_coe_soe()`, a call to
`soem_port_apply_pdo_layout()` just before `Obits`/`Ibits` are stored.

Why: SOEM sizes SM2/SM3 from the PDO mapping objects (CoE) or the SII. The Leadshine R1EC
modular coupler does not serve its mapping objects over SDO and its SII only describes the
4-byte Device Status, so SOEM maps 4 input bytes and no outputs. The firmware therefore takes
the module sizes from the ESI and registers them with `soem_esp_set_pdo_layout()` in the
coupler's PO2SOconfig hook (`components/ecat/ecat_coupler.c`). The user's earlier PC project
patched SOEM for the same reason (`docs/REFERENCE_PGM.md` section 4.2).

## License

SOEM v2 is dual-licensed: GPLv3 or a commercial license from RT-Labs (see `SOEM/LICENSE.md`).
The port files are derived from SOEM and carry the same terms. A closed-source product needs the
commercial license; this decision is still open for the project.

## Updating

Replace `SOEM/` with a newer checkout (same subset), update the commit above, and diff
`oshw/rtk/nicdrv.c` upstream against the previous commit to carry fixes into `port/nicdrv.c`.
Then copy the new `SOEM/src/ec_config.c` over `port/ec_config.c` and re-apply the two
`ESP-PORT PATCH` lines and the header comment.
