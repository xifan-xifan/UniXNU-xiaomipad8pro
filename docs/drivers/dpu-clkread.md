# SM8750 display clock and vote readback

The Ubuntu `xiaomipad-8pro-mainline/kernel/source.json` pins `linux-sm8750-piano`
at `b5100d5f2e0b`. Ubuntu line numbers below refer to that checkout. Piano
line numbers refer to the `lead/m120c` base at `52df12b1`.

| Evidence | Ubuntu file:line | Piano base and reader design |
| --- | --- | --- |
| DISPCC access | `arch/arm64/boot/dts/qcom/sm8750.dtsi:4429–4455`, `drivers/clk/qcom/dispcc-sm8750.c:1921–1946` | DISPCC at `0x0af00000`, GCC display AHB interface and MMCX domain. Reuse the powered MDSS/AHB/MDP gate at `package/PianoDpu/DpuSurvey.h:94–121` for the added reads. |
| MDP RCG | `drivers/clk/qcom/dispcc-sm8750.c:296–308,670–702,1573–1588`, `drivers/clk/qcom/clk-rcg2.c:26–52,165–226,517–559,1508–1516` | `DpuHw.h:40–45` and `DpuSurvey.h:109–113` read CMD/CFG at `+0x8150/+0x8154`. Source is CFG `[10:8]`, with XO 0 and PLL0 main 1. HID `[4:0]` gives a divisor of 1 for zero and `(HID+1)/2` otherwise. M/N/D are at `+0x8158/+0x815c/+0x8160`. The MDP RCG has `mnd_width=0`, so expose those words as raw and calculate with HID only. Linux uses `M/(M + ~N)` after width masking on RCGs with fractional hardware. The complemented D encodes duty cycle. |
| PLL0 | `drivers/clk/qcom/dispcc-sm8750.c:77–106`, `drivers/clk/qcom/clk-alpha-pll.h:200`, `drivers/clk/qcom/clk-alpha-pll.c:16–34,220–233,366–386,672–676,1078–1089,2419–2448,2536–2550,2575–2583`, `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts:123–141` | No Piano readback at the base. Taycan ELU uses Lucid EVO rate ops, with MODE/OPMODE/STATUS/L/ALPHA at `+0x00/+0x04/+0x0c/+0x10/+0x14`. `19.2 MHz × (L[15:0] + ALPHA[15:0]/65536)` is the configured PLL rate. MODE lock (bit 31), output (bit 0), and OPMODE run (bit 0) establish a running PLL. Return the current MDP rate only for supported, stable clock samples. |
| Display BCMs | `arch/arm64/boot/dts/qcom/sm8750.dtsi:4034–4039`, `drivers/interconnect/qcom/sm8750.c:500–505,926–931,966–977,1024–1028,1087–1097,1135–1139`, `drivers/interconnect/qcom/icc-rpmh.c:201–247,330–337`, `drivers/interconnect/qcom/bcm-voter.c:50–105,253–285`, `include/soc/qcom/tcs.h:68–79` | `DpuBandwidth.h:15–42` and `DpuSlices.cpp:237–282` send MM0. Look up MM0 (HF), SH0 (LLCC), ACV and MC0 (EBI) using the live cmd-db. Read each BCM's current DRV-2 vote, with X `[27:14]` and Y `[13:0]`. ACV Y is an enable mask. Keep the raw word and cmd-db scaling metadata. |
| RPMh read transport | `arch/arm64/boot/dts/qcom/sm8750.dtsi:399–403,6942–6965`, `drivers/soc/qcom/rpmh.c:180–258`, `drivers/soc/qcom/rpmh-rsc.c:89–93,157–204,448–469,498–530,619–693,982–1043,1088–1099` | `package/PianoA830/adreno830/platform/sm8750/Sm8750Rpmh.c:377–553` owns the apps DRV-2 transaction. Reuse its lock, ACTIVE slot selection, bounded completion and cleanup. A one-command read clears MSGID's write bit, leaves CMD_DATA untouched and collects CMD_RESP_DATA. Validate cmd-db resources before triggering. The response comes from the accelerator for DRV-2. The [RPMh driver's author confirms the voter scope](https://lkml.iu.edu/2510.3/03634.html). |
| MMCX and MXC ARC votes | `drivers/pmdomain/qcom/rpmhpd.c:141–166,190–199,658–674,962–977,1000–1043,1117–1156`, `arch/arm64/boot/dts/qcom/sm8750.dtsi:4112–4142,7034–7041`, `drivers/regulator/qcom-rpmh-regulator.c:257–270` | Read `mmcx.lvl` and `mxc.lvl`. Translate the returned ARC hardware level using the resource's little-endian cmd-db level table. Linux's 575 MHz MDP OPP requests MMCX NOM_L1 (320). A VRM voltage selector represents millivolts, while these two resources are ARC corner indices. DRV-2 corner votes and the actual aggregate rail corner are distinct. |
| CESTA | `drivers/clk/qcom/dispcc-sm8750.c:694–700` describes downstream CESTA control. The local pin has no CRM driver. The stock device tree maps `disp_crm`. | Read the display CRM configuration window at `0x0af29f00` under the display gate. Qualcomm's proposed `drivers/soc/qcom/crm.c` `crm_v2_cfg` and probe specify VERSION, CFG1/2 and ENABLE at `+0/+4/+8/+0xc`. This exposes firmware enable and topology. Claiming a winning CESTA client or all-voter BCM/ARC state requires further status sources. [Qualcomm CRM register proposal](https://lists.openwall.net/linux-kernel/2026/04/20/1743) · [Display CRM DT proposal](https://lists.openwall.net/linux-kernel/2026/04/20/1796) |

The fixed-120 qualification console records GCC AHB `88000003`, MDSS GDSC `fa22f800`, MDSS AHB and MDP CBCR
`08000001`, and MDP RCG CMD/CFG `00000000/00100105` (PLL0 ÷3). The earlier
snapshot captured the divider and left the PLL words and accelerator responses
unobserved.

## Snapshot and scope

Set `piano-dpu-clkread=1` to register the root-only
`machdep.piano_dpu_snapshot` with additional `clkread_` rows. The existing
`piano-dpu-snapshot=1` remains sufficient for the ordinary load-only snapshot.
A fresh read is available after 100 ms.

```sh
sudo /usr/sbin/sysctl -n machdep.piano_dpu_snapshot
```

- `clkread_mdp` and `clkread_pll0` give the raw words, sample status and the
  decoded Hz and MHz. An off, updating, changing or unsupported source yields
  an unavailable-rate status. The reader supports XO and PLL0 as MDP parents.
- `clkread_cesta` gives display CRM version, topology and firmware enable.
  The sample uses the MDSS gate and the non-GDSC AHB branch status. It leaves
  the winning CESTA voter unavailable.
- `clkread_bcm` gives the live apps DRV-2 MM0/HF, SH0/LLCC, ACV and MC0/EBI
  read responses. Compare MM0 X using matching cmd-db metadata. ACV Y is a mask.
- `clkread_arc` gives the apps DRV-2 MMCX/MXC votes as raw hardware indices and
  cmd-db virtual corner levels. Level 0 represents no vote from this voter.

When `piano-dpu-snapshot=1` is also set, the sysctl reports clock and CRM
words and suppresses RPMh queries so the M5 snapshot makes zero MMIO stores.
Use `piano-dpu-clkread=1` alone for the full vote readback.
[M5 snapshot](dpu-m5-analysis.md#read-only-m5-snapshot-and-capture-diagnostics)

The sysctl accepts reads only. RPMh queries program the existing ACTIVE TCS
transport with the command write bit clear. The query writes no vote payload.
The shared DRV-2 lock serializes each query with existing voters. After a
timeout or transport I/O error, the reader stops issuing queries for that boot
to avoid consuming additional slots. A timeout or unsupported read remains
visible in the raw rows. The existing snapshot's load/store count covers its
original display-path pass.

The pinned sources expose a per-voter response. Keep all-voter BCM allocation,
aggregate MMCX/MXC corners and the CESTA winner on HOLD until captured through
a source that reports those states. [Qualcomm's RPMh readback scope](https://lkml.iu.edu/2510.3/03634.html)

## Host grader

The CLK readback passed on the `v131y12t-qual1` RAM qualification run on
2026-11-27. The host grader takes saved snapshot text. For example, a pair
captured in an existing 120 Hz profile can be graded with:

```sh
python3 tooling/native/m120c-grade-clkread.py arm 120 before.txt after.txt
```

The `compare` subcommand accepts two pairs, in 120 then 144 Hz order. It
checks the per-voter MM0 increase and the decoded MDP and rail votes.
[Host grader](../../tooling/native/m120c-grade-clkread.py)
