# SM8750 DisplayPort PHY tuning tables (data only)

`qmp-sm8750-dp.tbl` holds the DisplayPort tuning data selected by `sm8750_usb3dpphy_cfg` for
`qcom,sm8750-qmp-usb3-dp-phy`. The extractor generates a standalone C/C++ header for the
PianoDp link bring-up work. That header is exercised by host tests. DisplayPort remains design-only
and awaits tablet qualification. [DisplayPort design](../../docs/drivers/displayport.md)

## Provenance

The source is the Ubuntu piano kernel `yzddmr6/linux-sm8750-piano`, branch `piano/dev` at commit
`7f8d6bedde479a01f4a05d6d0581b43c7fd20a01` (the v1.2.0 merge and the same pin as the
[camera tables](../camera/README.md)). All hashes below are the SHA-256 of the raw Git blobs at that commit.

| Extraction input (Linux tree path) | Data used | SHA-256 at the pin |
|---|---|---|
| `drivers/phy/qualcomm/phy-qcom-qmp-combo.c` | Selected tables, block and register layouts, literal AUX init values, link and VCO clocks | `a8e8384679f6445e25d049267046b9766ad8aee2dd6b80bffb1411aaaaa92457` |
| `drivers/phy/qualcomm/phy-qcom-qmp-dp-phy.h` | AUX_CFG0–9 register offsets | `727ff7bc45b642ea5db00f54c6590a0957454ec3c3a55ce428b578fea0731b7c` |
| `drivers/phy/qualcomm/phy-qcom-qmp-dp-phy-v6.h` | VCO_DIV register offset | `6db2c0f7e6c8f3790b104fd17b81eb4dd696f59beb229553c4f38a9e7a05a6c9` |
| `drivers/phy/qualcomm/phy-qcom-qmp-qserdes-com-v6.h` | Common and per-rate serdes register offsets | `b7b3e2cdfb767f365b0f3376be48ca2327218aade6b7067f7c4ef7f78541943a` |
| `drivers/phy/qualcomm/phy-qcom-qmp-qserdes-txrx-v6.h` | Shared TX init register offsets | `b3058d41c64f7fadefa921df9efc967bb11c0d33a290bf865236687993235d12` |
| `drivers/phy/qualcomm/phy-qcom-qmp-qserdes-txrx-v8.h` | Swing and pre-emphasis register offsets selected by the SM8750 register layout | `b970d041e7e20c42e431c1b64d5b5e875e4723e8cb1c256ad9a534185d4726a9` |
| `arch/arm64/boot/dts/qcom/sm8750.dtsi` | Combo PHY MMIO window size | `b94834a8364d8007d5c12af1b5ad2e0a74a80094fcecb6a583a6d0d6b2ada923` |
| `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts` | `mdss_dp0_out` rates, capped at HBR2 (5.4 Gbit/s per lane) | `79fe7c8009621e4fe00423a10218eacee6b92294ee978a5044026d0e05d127dd` |

The driver selects `qmp_v6_dp_serdes_tbl` (20 writes) and its `_rbr`, `_hbr`, `_hbr2` and
`_hbr3` overlays (9 writes each). Both TX blocks use `qmp_v6_dp_tx_tbl` (11 writes).
`qmp_v4_dp_aux_init` sets ten literal AUX_CFG values. The extractor records those ten writes
in order. The driver uses `qmp_dp_v6_voltage_swing_hbr_rbr` and `qmp_dp_v6_pre_emphasis_hbr_rbr`
at RBR/HBR, and `qmp_dp_v5_voltage_swing_hbr3_hbr2` and `qmp_dp_v5_pre_emphasis_hbr3_hbr2`
at HBR2/HBR3. `qmp_combo_offsets_v8` locates the five blocks, and
`qmp_v3_configure_dp_clocks` supplies the VCO divider selectors and clock rates. HBR3 is retained
for completeness of the SoC data. Piano's DT allows RBR, HBR and HBR2.

### Comparison with the local kernel HEAD

The checked-out HEAD was `b5100d5f2e0bb80e1191d521a48596674608eb3d`. The combo driver differs
between the two commits. HEAD adds USB4 and other SoC support and drops the generic
`qmp_v4_calibrate_dp_phy` retry callback from SM8750 and other SoCs (K:`8fd2b30d187f`). That
callback cycled AUX_CFG1 after init. The ten initial AUX_CFG values, all selected serdes and TX
writes, the four matrices, block and tuning register offsets, rate parameters and piano's DT
cap were extracted at both commits and agree entry for entry. The two DT files changed elsewhere.
The comparison found no piano-specific fix to these PHY tuning settings, so this data uses the
v1.2.0 pin.

The five register headers used by the extractor, `phy-qcom-qmp-dp-com-v3.h` and
`phy-qcom-qmp-pcs-v8.h` are byte-identical between the commits. In the
`phy-qcom-qmp-qserdes-*.h` family, HEAD changes `com-v8.h` (copyright line and eleven added
definitions) and adds `com-pcie-v6_40.h`, `com-v10.h`, `com-v10_60.h`, `txrx-ufs-v8.h`,
`txrx-v10.h`, `txrx-v10_60.h` and `txrxz-pcie-v6_40.h`. In the DP headers, HEAD adds
`phy-qcom-qmp-dp-phy-v10.h` and removes `phy-qcom-qmp-dp-qserdes-com-v8.h`. The latter is
distinct from the shared `phy-qcom-qmp-qserdes-com-v8.h`.

| Other QMP header reviewed (under `drivers/phy/qualcomm/`) | SHA-256 at the pin |
|---|---|
| `phy-qcom-qmp-common.h` (init macro and shared TX semantics) | `6812948a3dcc49351e2493676b79c3ee9fb4eeaa516a041b13c3fa93c31c5b2e` |
| `phy-qcom-qmp.h` (register header includes) | `622ce22277eaf14d5914a37ba04050784b447d3895b4b7f5b468d1e9330c2f97` |
| `phy-qcom-qmp-dp-com-v3.h` | `e6c545a6cfb121fea9ff120c7e56d704cc5f9ae536e6d34788fea2071f767864` |
| `phy-qcom-qmp-pcs-v8.h` | `2a4bb246f2be7320bf6e411d84818bb93f3555feb473d09c85a9338a96f6dae3` |
| `phy-qcom-qmp-qserdes-com-v8.h` | `89110804b8524cd37438fbfc67982700ab1473715f33a9de4b67dcaa8777076d` |
| `phy-qcom-qmp-dp-qserdes-com-v8.h` | `bc1af997e3de99d52c877734bdf8b402363acc1d87674f796cfa4bcb2d9e2d30` |

**Licence.** The owner's decision of **2026-11-12** authorizes keeping the PHY tuning values as
hardware data in the project's own format with provenance. The Linux driver and headers are
GPL-2.0 sources. The extracted file contains only register offsets, values and clock parameters.
The project format and generated declarations contain no Linux functions, comments or C structures.
AUX power sequencing, interrupt masks and calibration behavior belong to driver logic.
[Hardware backlog §4](../../docs/BACKLOG-hardware.md#4-owner-decisions)

## Format

Text with one record per line. A `#` begins a comment. Blank lines separate the sections.

```text
phy NAME                               table identifier
commit GIT_SHA                         exact kernel commit
source PATH SHA256                     kernel-relative file and raw Git blob SHA-256
[layout qmp]                           combo PHY window, block locations and board rate cap
[layout tuning]                        tuning register locations (VCO_DIV, TX swing and pre-emphasis)
o KEY OFFSET                           hexadecimal offset (four digits); window gives size in bytes
p piano_max_mbps RATE                  decimal per-lane limit from piano's DT
[rates link]                           PHY rates in ascending order
l NAME MBPS LINK_HZ SELECTOR DIVISOR VCO_HZ
[serdes common]                        common serdes writes
[serdes RATE]                          rate-specific writes after common (rbr, hbr, hbr2, hbr3)
[tx shared]                            same sequence for TX A and TX B
[aux dp-phy]                           AUX_CFG0–9 init writes
r OFFSET VALUE                         aligned block-relative register offset and byte value (hex)
[matrix NAME]                          one of the four swing or pre-emphasis matrices
m VALUE VALUE VALUE VALUE              four byte codes (hex), one row per voltage swing level
```

Block locations are relative to the combo PHY base. Tuning registers and writes are relative to
their own block. Writes retain source order. In an `l` row, all numbers are decimal.
`SELECTOR` is the VCO_DIV register value, `DIVISOR` is the ratio used to compute `VCO_HZ`, and
`LINK_HZ` is the QMP link clock (one tenth of the serial bit rate). Matrix rows and columns
are indexed 0–3 by swing and pre-emphasis respectively. Valid codes are 0x00–0x3f.
`0xff` marks a combination where swing + pre-emphasis exceeds 3.

## Tools

```sh
python3 tooling/dp/dp-phy-tables.py check data/dp
python3 tooling/dp/dp-phy-tables.py check data/dp --src "$KERNEL_SRC"
python3 tooling/dp/dp-phy-tables.py header data/dp "$OUT_DIR/DpPhyTables.h"
python3 tooling/dp/dp-phy-tables.py --self-test

# After a deliberate extraction, review the table diff before regenerating MANIFEST.
python3 tooling/dp/dp-phy-tables.py extract "$KERNEL_SRC" data/dp
python3 tooling/dp/dp-phy-tables.py manifest data/dp > data/dp/MANIFEST
```

`KERNEL_SRC` is a kernel Git checkout containing the pinned commit. The extractor reads the
commit's blobs through `git show`. `check` verifies the table format, SHA-256 and the number of
records in each section, including each matrix's four rows. With `--src` it also checks byte-for-byte
reproducibility. Header generation verifies the same manifest. The
[CI host test](../../tooling/dp/test-dp-phy-tables.py) needs Python and clang++ without a kernel checkout.
