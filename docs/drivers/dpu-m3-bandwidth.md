# Piano DPU M3 bandwidth at 144 Hz

**Owner decision (2026-11-27, `OWNER-0748`):** 120 Hz is approved as the daily default refresh rate; keep 144 Hz as opt-in.

y12o reached the INTF underrun check during 60 → 144 Hz after both links had passed timing, DMA and cadence checks. The sticky status only locates the error within the switch interval. Fixed 120 Hz is the working control. See [the qualification and rollback analysis](dpu-m3.md#y12o-qual1-underrun-and-login-failure) and [the earlier high-refresh analysis](dpu-m3-144.md).

## Ubuntu calculation

The local Ubuntu integration checkout pins `b5100d5f2e0b` in `kernel/source.json`. The display perf calculation and SM8750 interconnect routing below also match the v1.4.0 kernel `83f0f89bed14`. The calculation assumes one internal CRTC and a unity-scale, linear 32-bpp surface split across the two active pipes. Each pipe contributes 1600 pixels per line. These are driver requests from source. The applied clocks and bandwidth on any live boot still need a capture.

| Quantity | 120 Hz | 144 Hz |
| --- | ---: | ---: |
| Full uncompressed timing, total pixels × lines | 3498 × 2310 | 3306 × 2310 |
| DRM mode clock, integer kHz | 969,645 | 1,099,707 |
| HFP per link / compressed clocks per line | 117 / 576 | 21 / 480 |
| Nominal per-link pixel / byte clock | 159.6672 / 149.688 MHz | 159.6672 / 149.688 MHz |
| Line period | 3.608 µs | 3.006 µs |
| Plane base fetch sum, `3200 × 2310 × Hz × 4` | 3,548,160,000 B/s | 4,257,792,000 B/s |
| `mdp0-mem` AB, `floor(fetch × 120 / 100 / 1000)` | 4,257,792 kB/s | 5,109,350 kB/s |
| `mdp0-mem` IB, catalog DRAM minimum | 800,000 kB/s | 800,000 kB/s |
| Mode admission clock with 3D merge (integer kHz) | 465,696 kHz | 558,835 kHz |
| Calculated MDP core, `3200 × 2310 × Hz × 105 / 100` | 931,392,000 Hz | 1,117,670,400 Hz |
| MDP OPP request after ceiling, with NOM_L1 MMCX | 575 MHz | 575 MHz |

The 35-line prefill budget fits in the 106 lines of VBP + VSYNC, so the bandwidth calculation selects the base fetch at both rates. The 28.5 GB/s `max_bw_high` bounds the real-time client aggregate. `max_bw_low` (18.9 GB/s), `min_core_ib` (2.5 GB/s) and `min_llcc_ib` (0) are catalog entries exposed through debugfs. This path uses `min_dram_ib` for peak bandwidth. The two DSI links share one acquired `mdp0-mem` path, so AB is divided by one. MDSS adds a separate 400 MB/s enable IB on that route, and ICC takes the maximum of the two peaks.

Both unrounded DSI calculations use 427 compressed active clocks per link, DSC 10-bpc/8-bpp, and four lanes at about 1197.5 Mbit/s each. The DRM kHz truncation trims the computed DSI pclk by about 100 Hz before clock rounding. The 20% increase is in line arrivals and the calculated MDP core demand. `dpu_crtc_mode_valid()` divides its clock estimate by two for 3D merge, so both modes pass the 575 MHz admission limit. The runtime CRTC vote uses the full adjusted width and both calculations reach the OPP ceiling. The core tracepoint is before the clamp and can report an increase while both applied targets are 575 MHz.

Plane update populates `plane_fetch_bw` during commit and plane-state duplication copies it. The perf check before that update reads the stored plane values. The table gives the request with plane state populated at the given refresh. The first modeset commit can still carry the previous fetch estimate. Piano computes from the target timing and explicitly waits for the vote before staging it.

## Line-by-line comparison

Ubuntu lines refer to `b5100d5f2e0b`. DPU files are under `drivers/gpu/drm/msm/disp/dpu1/`. Piano lines refer to the task base `7e2ca038`. The repo's earlier analysis also captures the stock P81 panel and CESTA evidence. Its stock clock capture was taken while the display was off.

| Ubuntu file:line | Piano file:line | Comparison |
| --- | --- | --- |
| `drivers/gpu/drm/panel/panel-novatek-nt36532.c:420–470,628–650,821–836` and `drivers/gpu/drm/msm/dsi/dsi_host.c:608–687,1042–1070` | `package/PianoDpu/DpuHw.h:163–176`, `DpuModes.h:125–147`, `DpuModeSwitch.h:347–357` | **Same** porch geometry, fixed transport rate and panel rate-group values. **Different** programming sequence. Ubuntu sizes and programs DSC at modeset. M3 uses inherited DSC and live timing shadows. |
| `dpu_plane.c:116–155,165–184,1536–1584,1619–1634,1659–1671`, `dpu_core_perf.c:40–107,129–136,190–193`, `dpu_crtc.c:1646`, `drivers/gpu/drm/msm/msm_atomic.c:244–249` | `package/PianoDpu/DpuModes.h:125–147`, `DpuHw.h:178–179` | **Missing** native per-mode plane fetch and MDP estimates. Ubuntu fills the estimates during plane update and reads the saved values at CRTC check. The Piano constants describe the simplefb OPP (AB 3,280,896 / IB 4,257,792) at `sm8750-xiaomi-piano.dts:82–117`. |
| `catalog/dpu_12_0_sm8750.h:439–465` and `dpu_core_perf.c:139–209,471–485` | `package/PianoDpu/DpuModeSwitch.h:277–319,337–357` | **Missing** native SM8750 perf sizing and budget check. Current native checks cover the fixed timing, two live links, timing words, fetch lead, counters and underrun status. |
| `dpu_kms.c:410–429,1463–1492`, `drivers/gpu/drm/msm/msm_io_utils.c:145–161`, `arch/arm64/boot/dts/qcom/sm8750.dtsi:4034–4039` | `package/PianoDpu/DpuSlices.cpp:614–625` and `DpuSlices.h:62–85` | **Missing** a display interconnect consumer. Ubuntu acquires `mdp0-mem` from MDSS. The board has no second memory path. The native timing writes target display registers. |
| `dpu_crtc.c:758,1028,1296,1646`; `dpu_core_perf.c:215–242,337–397`; `drivers/gpu/drm/msm/msm_mdss.c:246–260` | `package/PianoDpu/DpuModeSwitch.h:332–357,360–401` | **Missing** an acknowledged BW increase before the rate latch. Linux submits increases at kickoff, before raising the clock, and lowers after the commit or on stop. Native verifies timing and a bounded clean window, then restores before starting a new clean observation window if needed. |
| `drivers/interconnect/qcom/sm8750.c:378–383,460–465,500–505,926–931,966–977,1024–1028,1087–1097,1135–1139`, `icc-rpmh.c:84–105,212–236`, `bcm-voter.c:80–142,338–378` | `package/PianoA830/board/PianoA830.cpp:698–734`, `package/PianoDpu/PianoDpu.cpp:630–640`, `DpuAhbVote.h:12–22` | **Missing** the vote for the HF path BCM `MM0` that carries MDP to GEM NoC. Linux normalizes AB by width × channels (32 × 2) and IB by width (32), with command-db aux width/unit, then encodes X/Y. Linux also votes SH0 (LLCC) and ACV/MC0 (EBI). Native A830 owns those shared DRV-2 resources, with a default scanout-sized 6,671,875 kB/s idle floor. The native DPU AHB vote opens register access. |
| `dpu_crtc.c:1656–1687`, `catalog/dpu_12_0_sm8750.h:11–20`, `dpu_core_perf.c:280–301,381–403,507–523`, `dpu_kms.c:1278–1304,1518–1521,1533–1582`, `sm8750.dtsi:4065–4081,4112–4142`, `drivers/clk/qcom/dispcc-sm8750.c:296–307,670–700`, `drivers/opp/core.c:1297–1387,1413–1466` | `package/PianoDpu/DpuHw.h:40–45`, `DpuSurvey.h:96–121`, `PianoDpu.cpp:230–241` | **Different** clock ownership. Ubuntu checks the two-mixer budget, chooses the max active CRTC request, clamps to the clock/OPP ceiling and sets the OPP with the supply corner and parent-rate transition. Piano inherits DISPCC/MMCX and reads the gates and MDP RCG. Stock also uses CESTA. Keep the inherited rate for the bandwidth experiment since both Ubuntu targets are 575 MHz. |
| `dpu_encoder_phys_vid.c:176–254` and `dpu_hw_intf.c:284–306` | `package/PianoDpu/DpuModes.h:111–118,145`, `DpuModeSwitch.h:296–313` | **Same** fetch start formula. **Different** enable policy. Ubuntu disables programmable fetch for this geometry. M3 moves the inherited 24-line lead with the mode and checks the result. |
| DPU atomic mode setup and verification above | `package/NativeGopDisplay/NativeGopDisplay.cpp:842–844,3223–3276,3297–3299`, `package/PianoDpu/DpuSlices.cpp:1083–1104` | **Different** publication. M3 starts via a synchronous worker and publishes one verified fixed timing. Preserve that contract for the experimental boot option. |

## Qualification profile

`piano-dpu-modes=144` uses Linux's AB calculation and the `MM0` command-db width and unit. The worker sends one ACTIVE TCS request under the existing DRV-2 lock. It completes before timing staging, and the vote stays held on panel sleep and mode rollback. This tests the HF bandwidth path using the existing shared DDR floor. Linux's ICC path aggregates other consumers and can also program wake and sleep votes. Effective ownership across system suspend remains a tablet check.

The timed host model exercises both 60 and 120 Hz as the starting rate, confirms no timing or DCS stores after a failed vote, and models both clean 144 and continued starvation after success. It checks the restored rate after starvation and the held-vote path through panel sleep and wake. The tablet capture needs the `mode: MM0` line (command-db aux, command word, TCS result), the active MDP RCG and PLL0 registers from the earlier analysis, and the INTF underrun bits around timing/DSI handover and during steady scanout. An ACTIVE TCS completion establishes command acceptance. Effective fabric bandwidth and the absence of underruns require direct observation.

`piano-dpu-clkread=1` adds the live apps DRV-2 MM0, SH0, ACV and MC0
responses, MMCX/MXC corner votes, MDP clock rate and CESTA enable to the root
snapshot. Aggregate allocation and aggregate corner qualification remain open.
[Clock and vote readback](dpu-clkread.md)

## Fix-image gate

Path: NativeGopDisplay → `PianoDpuMode` → slices worker under the flip lock → confirmed bandwidth vote → bonded CTL/INTF/DSI timing and panel group → cadence/underrun verification → single IOMFB timing, refresh attributes and VBL period. Ubuntu's active path is DRM CRTC/planes → DPU core perf → ICC `mdp0-mem` via MM0, SH0 and ACV/MC0 → OPP/DISPCC/MMCX → encoder/DSI/P81.

Checks: **ours-OK** for fixed 120, matching timing geometry, fetch readback, panel-group DMA completion and cadence before the previous 144 error. **ours-missing** at the task base for a native per-mode MM0 request. **Unknown** are the inherited MM0 vote, the live MDP PLL rate and MMCX corner, 144 steady-state margins, and whether the first underrun occurred during the handover or in steady scanout.

Root cause: The 144 timing cuts line service time by one sixth while the native display requests no additional feed bandwidth. The missing MM0 request is the first isolated capacity gap to test. Prior logs do not establish the applied bandwidth or exclude an MDP clock or timing-handover cause. The pinned Linux OPP path provides no 120-to-144 core clock step for this surface.

ONE change: Add `piano-dpu-modes=144` as a fixed qualification profile. Complete the active MM0 vote for 5,109,350 kB/s AB before touching 144 timing. Use the larger of KMS IB and the reference 120 Hz splash peak to avoid deliberately lowering the reference boot peak when replacing the same DRV-2 resource. Hold the vote for the boot, including a verified rollback. Keep existing 60/120 behavior and publish 144 only after both links verify. The next tablet read must check the MM0 command and effective fabric allocation, MDP PLL/core and MMCX, and per-INTF underruns through the switch and steady 144 under load against fixed 120.
