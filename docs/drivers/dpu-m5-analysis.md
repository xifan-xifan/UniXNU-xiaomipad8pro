# DPU M5 panel sleep analysis

The dark-wake fix-image gate is **HOLD**. [Current gate](#fix-image-gate)

**The historical stop-order cause is closed; the exact dark-wake repair remains open.** Clean `v131y12u-m5obs3` reproduces dark pixels with panel `awake`, moving scanout and no serial-3 overwrite. DSI1's FIFO remains clean through completed wake-command cleanup, then faults before the first video snapshot. Both hosts report CMD engine/MDP busy, video busy and contention, with **command DMA idle** and **CMD_MDP/lane HS underflows**, not video FIFO overflow. This narrows the failure to the restart interval without identifying one causal store. `RESET +0x118` loses undefined `0x1c00` bits through the same literal writes Linux uses. [Clean serial-2 decode and remaining check](#v131y12u-m5obs3-serial-2-wake-decode)

## References and topology

- Ubuntu v1.4.0 at release commit `9f4b92899a71` selects Linux `83f0f89bed14` (`7.4.0-rc2-piano98`). Linux line numbers below are from that pin. The older slave-first change `32d04280ddeb` and deferred-cleanup change `8b53b972e9bd` (the pin's version of `c55958fa09dd`) describe earlier stages of the Linux implementation. The shared-video branch added by `1fead1793340` supersedes their stop order on piano. The older slave-first code and test assumptions describe the diagnostic baseline. [Ubuntu kernel pin](https://github.com/yzddmr6/xiaomipad-8pro-mainline/blob/v1.4.0/kernel/source.json), [shared-video handoff patch](https://github.com/yzddmr6/linux-sm8750-piano/commit/1fead1793340e023ab6f9112d542f46460d00351), [earlier M5 comparison](dpu.md#84-m5a-display-sleep-qualification)
- The NT36532 panel has two four-lane DSI video links and DSC 1.1 at 10 bpc and 8 bpp, with two 800 × 24 slices per half. The Linux device tree declares DSI0 the master, DSI1 the bonded partner, and uses the DSI0 PHY's PLL clock for DSI1. The SM8750 catalog maps DSI0 to **INTF1** and DSI1 to **INTF2**. y12n independently reports one video CTL (CTL0), `INTF_ACTIVE=0x6` (bits 1 and 2) and `INTF_MASTER=0x2` (bit 1, **INTF1**). The two INTFs show the same frame number. DSI1 is the Linux command-transfer entry point for synchronized writes (it triggers DSI0 then DSI1), a different role from the DPU timing master.
- The captured effective stock Android device tree confirms both P81 variants use two controllers/PHYs and LP on/off command sets. BOE `35-02-0b` matches the y12n panel ID. In the BOE node, `display-timings` specifies a 1600 × 2136 half and DSC. All three timing entries have short DCS `0x28` with a 20 ms post-wait and `0x10` with a 100 ms post-wait, with the request-ACK field zero. The on sequence initializes panel settings and ends with `0x11`, a 120 ms wait, then `0x29`. The BOE firmware panel XML corroborates the command IDs and waits. This gives the stock command contract, while stock SDE/DSI stop ordering and the exact installed vendor driver remain unverified. MiCode publishes [piano-w-oss display drivers](https://github.com/MiCode/vendor_opensource_display-drivers/tree/piano-w-oss) and [display device trees](https://github.com/MiCode/vendor_qcom_proprietary_display-devicetree/tree/piano-w-oss). The branch listing was readable, but the relevant driver bodies could not be fetched and the release's equivalence to the captured stock build is unknown.
- The macOS 27.3 shared-cache IOMobileFramebuffer symbols corroborate display discovery (`iomfb_populate_all_display_infos`). The power ABI for this trace comes from the local IOMobileFramebufferLegacy subclass and its documented AppleParavirtDisplay contract. The exact WindowServer call site is unknown. The family calls the subclass's gated power hook, which combines system power and WindowServer API power. The measured failure is below that callback. The sheng port's `ShengDpuObservation.c:24–34,64–72` uses the same active bit, enable word and counter offsets for observation and supplies no bonded stop sequence. [NativeGopDisplay provenance](../../package/NativeGopDisplay/UPSTREAM)

## Earlier runs

References to `console.out`, `CLASSIFY.md` and snapshot extracts below are line numbers in the y12m and y12n qualification captures. The harness reached the display blocks, SSH and same-boot checks passed, and y12n has both live and later paired snapshot samples with a positive counter-motion control. The M5 success grep was zero alongside an actual worker failure and ten later refused sleep cycles. The qualification counted 11 refusals in its capture window (`RESULT.md:414–423`). Additional refusals occur later in the full console. y12n's dim-still and brightness-reset failures followed an auto-brightness command that lacked root and left the dim lock-screen policy active (`CLASSIFY.md:44–49`).

| Run and evidence | Result |
| --- | --- |
| y12m `console.out:3650–3651,3701,3705–3706,71037+`, `CLASSIFY.md:46–53` | Both frame counters already measured 119.998 Hz. The old COUNT_EN gate interpreted zero readback as stopped counters and latched `gPanelBroken`. The enabled boot-argument banner still appeared. The old readiness flag still dispatched later requests to a worker that returned early with I/O error. No M5 DCS or timing-stop attempt was reached. |
| y12n `console.out:3633–3635,3678–3690`, `CLASSIFY.md:28–41` | Both counters advance at 119.878 Hz despite `COUNT_EN 0/0 → 0/0`. The worker verifies line motion as well and serves M5. The command path has software trigger 4, hardware block-in-frame clear and an idle test-pattern FIFO. The initial M3 mode selection refusal at line 3692 is a separate path, with a combined CTL/counter diagnostic. |
| y12n `console.out:1170,3697–3700,4676` | NativeGopDisplay already found PianoDpu at line 1170. The observed boot edge is system 1/API 0, before the first API-on. PianoBacklight reports keeping the boot screen lit. The copy worker's combined guard refuses M5 before dispatch. Its text folds several error returns together, so the exact blank-call return is unknown. API-on appears at line 4676. |
| y12n `console.out:74533–74545,74750–74751,75382–75388` | First real runtime off, before the ten M5 cycles. Both KTZ8866 BL_EN writes read back off and touch parks. The two DCS sends finish in 1264 and 1266 µs. Step 3 then returns the INTF2 40 ms timeout. The log truncates at `engines intf1 -1`. The code only produces that timeout after an accepted store and polls with the slave bit set through the deadline. It never issues INTF1's stop request. Recovery reports completion and the boot latch disables future M5 sleep. Wake later restores the backlight and Night Shift. |
| y12n `console.out:77047+`, `m5-1-asleep.txt:1–4`, `CLASSIFY.md:37–38` | The ten qualification off edges hit `RequestGate` and use backlight-only sleep/wake. The first pair of asleep snapshots shows `EN=1`, `STATUS=1`, `COUNT_EN=0` and frame advance from 206884 to 206924 on both links. These are samples after the fault and recovery. |

## Sequence comparison

Ubuntu filenames under `msm` refer to `drivers/gpu/drm/msm/` (`disp/dpu1/` for DPU and `dsi/` for the host/manager). The panel is under `drivers/gpu/drm/panel/`. Native line numbers are for the y12n tree at `061fe59c`. Status labels assess each operation in the diagnostic baseline.

The observed native path had `piano-dpu-panel=1` with LP DCS and engine stopping enabled. NativeGopDisplay enables the callout for the internal display only (`NativeGopDisplay.cpp:938–939`). The options also allow DCS-only sleep, leaving both timing engines running (`piano-dpu-panel-engines=0`), and high-speed command transfer (`piano-dpu-panel-hs=1`). The worker's normal wake and its undo path both use the same start/flush helper. [Option parsing](../../package/PianoDpu/DpuSlices.cpp#L148-L151)

Ubuntu blanks the panel backlight, drains and stops the DPU, unprepares the panel over the still-powered DSI hosts, and then disables the hosts and PHYs. On enable, the DSI bridge powers the hosts first, the panel prepares and sends its init commands, and the DPU configures and starts the video path. Backlight enable is in the bridge enable phase. Video timing starts at DPU kickoff, after the shared CTL flush.

| Operation | Ubuntu v1.4.0 file and lines | Native file and lines | Comparison and assessment |
| --- | --- | --- | --- |
| Bind bonded roles and gate access | `sm8750-xiaomi-piano.dts:1437–1487,1513–1534`; `dpu_kms.c:601–618`; `dpu_encoder.c:2769–2789`; `catalog/dpu_12_0_sm8750.h:411–427` | `DpuSlices.cpp:812–839`; `DpuSlices.h:107–164` | Same topology. **ours-OK** on y12n. Native waits for survey/flip readiness, premaps, checks the memory/clock gate, a video CTL driving both INTFs, matched geometry and live DSI hosts. It reads `INTF_MASTER` but assumes INTF1 is master, so explicit role validation is **ours-missing** for a bonded stop transaction. |
| Provide frame/line timing | `dpu_hw_intf.c:250–258,326–344` | `DpuSlices.h:260–269`; `DpuPanel.h:53–78`; `DpuSlices.cpp:856–867,887–899` | Same enable value, different readiness test. **ours-OK** after dpu-m5fix. Request both enables and admit panel sleep only after both lines move and each frame count advances at least twice, with no more than one frame delta between links. |
| Request display off and quiesce users | `msm_atomic.c:224–249`; `drm_atomic_helper.c:1229–1240`; `drm_panel.c:163–179,835–849` | `NativeGopDisplay.cpp:1524–1569,2268–2289`; `GopPanelPower.h:7–41` | Different entry point. **ours-OK**. macOS publishes off, queues a backlight request and posts the edge to the copy worker. The worker serializes the power call after its current copy/flip job and holds pending presentation. Linux serializes atomic commits and starts bridge disable before encoder disable. |
| Blank and park the touch bus | `drm_panel.c:835–849` calls followers and blanks the backlight | `NativeGopDisplay.cpp:1699–1719`; `PianoBacklight.cpp:699–719,1053–1108`; `Ktz8866.h:261–276`; `PianoTouch.cpp:1567–1585` | Different ordering of the pre-stop operations. **ours-OK**. Require the serving backlight worker to confirm both ICs dark (up to 800 ms, with retries); then, when enabled, require touch SPI to park (up to 600 ms). Boot off is deliberately suppressed to preserve the splash. Both runtime blank readbacks and the park appear before the real attempt. |
| Admit request and drain already-submitted work | `msm_atomic.c:224–243`; `dpu_encoder.c:1445–1453,2964–2988`; `dpu_encoder_phys_vid.c:335–366,505–552` | `DpuSlices.cpp:624–635,1054–1077`; `DpuFlip.cpp:656–667` | Different. **ours-OK** for serializing and revalidating the CTL/links. **ours-missing** for an explicit stop-specific vsync/flush handshake. Linux waits for the prior CTL commit and video TX completion before the disable writes. Master vblank retires pending kickoff only when the corresponding CTL flush bits have cleared and signals video frame done. The front-porch DCS wait provides neither a CTL drain proof nor a stop event. |
| Send display-off and sleep-in | `drm_atomic_helper.c:1375–1382`; `drm_bridge.c:894–978`; `drm_panel.c:182–199,605`; `panel-novatek-nt36532.c:78–82,169–198,780,821–836` | `DpuPanel.h:38,95–114,397–419` | Same commands and 20/100 ms waits. Different scheduling. **ours-OK** for the sequence sent. Linux sends during panel unprepare after the encoder stops but before host teardown. Native keeps video running for FIFO transmission and waits for the commands before touching either INTF. Panel receipt/execution is **unknown**. |
| Transmit on both hosts | `dsi_host.c:1220–1259,1464–1495,2188–2252`; `dsi_manager.c:523–583` | `DpuPanel.h:132–233`; `DpuSlices.h:49–54,228–252` | Same two-link trigger order, different transport. **ours-OK** for software trigger, LP FIFO packet and both command completions. Native checks controller enable/video, free FIFO, software trigger and DMA idle (20 ms), saves registers, enables command mode beside video, clears stale CMD_DMA_DONE, and arms both LP FIFO transfers. With bit 12 clear it waits in INTF1's front porch, triggers DSI0 then DSI1 with interrupts masked, polls both done bits (50 ms), and restores completed links only. Linux uses memory-backed DMA, waits for DSI VIDEO_DONE if video is busy, and waits for command DMA completion on the triggering host. |
| Request the timing stop | `dpu_encoder.c:1385–1421,1455–1469`; `dpu_encoder_phys_vid.c:586–666`; `dpu_hw_intf.c:275–282` | `DpuPanel.h:268–285,420–438` | Same zero write to TIMING_ENGINE_EN. Different bonded order, **ours-missing**. Ubuntu requests INTF1 off, waits for master vsync (retries a wait if active status persists), verifies both active bits clear, hands off the slave while the pixel path remains bound, then requests INTF2 off. Native requests INTF2 off and blocks on its active bit before it requests INTF1 off. |
| Verify stop and shared CTL handoff | `dpu_encoder.c:1393–1421,1471–1506,2433–2457`; `dpu_hw_ctl.c:144–181,414–472,693–706,853–859` | `DpuPanel.h:268–285,426–444`; `DpuSlices.h:57–83` | Same active bit, different prerequisite and completion. **ours-missing** for the shared-route submission. The pin's handoff resets the stopped CTL, removes the master route (`ACTIVE 0x6 → 0x4`, `MASTER 0x2 → 0x0`), submits INTF flush and CTL START with the slave's pixel resources intact, and polls CTL_FLUSH for zero. Linux then polls the slave's status at its own frame boundaries, and resets and flushes the now-empty shared path once. Native retains routing and its store guard excludes CTL reset, START and active/master writes. The full-mode cleanup would unbind resources needed by a retained-path wake. |
| Disable prefetch or tear down power | `dpu_hw_intf.c:284–305`; `dpu_encoder.c:2352–2427,2433–2457`; `dsi_manager.c:358–411`; `dsi_host.c:2493–2507,2639–2668`; `panel-novatek-nt36532.c:122–132,184–198,452–459` | `DpuPanel.h:251–266,422–446` | Different, intentional light-sleep scope. **ours-OK** in code and unverified on a successful live stop. Native saves both CONFIG/FETCH_START pairs, leaves prefetch active for the final frame, clears CONFIG bit 31 after both stops and checks the register readback. Linux unprepares the panel with the hosts still powered, asserts panel reset, drops AVEE/AVDD/VDDIO in reverse order with the corresponding 2/3/2 ms waits, then the DSI manager disables and resets the controller operation mode (DSI0 then DSI1), takes IRQs down, powers off DSI1 before DSI0, and releases the PHYs. Linux host reset and host power-off are distinct from stopping the DPU INTF. Retaining the hosts, PHYs and panel supplies avoids rebuilding that path on M5 wake. |
| Bring up the link and wake the panel | `dsi_manager.c:208–270,301–335`; `drm_panel.c:125–161,605`; `panel-novatek-nt36532.c:103–167,652–673,843–852` | `DpuSlices.cpp:652–674`; `DpuPanel.h:290–350,452–480` | Different. Linux brings up/resyncs both PHYs, resets/enables both hosts with IRQs available, then prepares the panel, programs supplies/reset/vendor mode, sends `0x11`, waits 120 ms, sends `0x29` and sends DSC PPS. Native keeps clocks and the configured panel/PHY state, restores prefetch then starts timing before wake DCS, waits 120 ms between commands and 10 ms after display-on. **unknown** for a live M5 wake from both engines stopped. |
| Flush and start video, release presentation | `dpu_encoder.c:1363–1377,1862–1913,2253–2272`; `dpu_encoder_phys_vid.c:446–502,668–683`; `drm_atomic_helper.c:1736–1745`; `drm_panel.c:763–790` | `DpuPanel.h:290–350`; `DpuSlices.cpp:666–675,754–778`; `NativeGopDisplay.cpp:1681–1696,1929–1948` | Same flush-before-enable rule. Linux configures the slave physical encoder before the master, submits the shared CTL once, then enables timing in post-kickoff physical order (master tile first). Native restores both fetch starts/configs, stages INTF_FLUSH `0x6` and CTL_FLUSH bit 31, then starts INTF1 and INTF2 back to back. It checks both active bits (50 ms), the flush latch (60 ms), and frame motion/lockstep (100 ms sample) before DCS wake and PCC replay. On successful return Native releases touch, publishes on, lights backlight and releases swaps. **ours-OK** for the observed recovery and backlight-only wake. Paired stop/restart remains **unknown** on hardware. |
| Undo failed off and decline later requests | Linux modeset owns the path and tears down its resources | `DpuPanel.h:301–350,368–386,426–437`; `DpuSlices.cpp:635–649,654,772–778,1062–1075`; `NativeGopDisplay.cpp:1681–1696,1719–1724` | Native keeps the display dark until the wake reply following a dispatched error. On the observed failure it checks/restores fetch and engine state, cancels pending EN=0 when needed, completes `0x11/0x29`, and latches M5 unavailable for the boot. **ours-OK** for y12n recovery. A restore failure holds the swaps/backlight and publishes a fault. |

## y12o qual2b row-by-row decode

The off property, the wake property and the filtered console all contain the same serial-1 off report and exactly the same 216 indexed rows, with zero overwritten. The header says `ok=0 step=3 restored=1`. The subsequent wake reused the completed rollback and published no new hardware sequence. The console independently identifies INTF1 as CTL0 master (lines 261 and 74280). Both backlights confirmed off and touch SPI parked before the attempt (lines 74625–74632). The console records the stop failure and boot latch at lines 74880–74882 and eight later refusals. The command reports have LP FIFO DMA done on both links, front-porch triggers, 1,245 and 1,270 µs to completion, zero ACK_ERR before and after, and unchanged FIFO status `0x1010` on both hosts. These are host completions.

Every adjacent sample is ordered in time and, accounting for 2,310 lines per frame, both raw counters progress across all 215 intervals. The frame words agree at each sample. The raw INTF2 line leads the INTF1 line by 0–2 across sequential MMIO reads. In all 216 samples, CTL TOP is `0xf0000000` (video), ACTIVE is `0x6`, INTF1 EN/STATUS is `1/1`, and INTF2 STATUS is 1. The following ranges partition every row. Times are microseconds from row 0.

| Rows | Time (µs) | Decoded transition |
| --- | ---: | --- |
| 0, 1 | 0, 8,229 | Before/after DCS `0x28`. Both EN/STATUS are 1/1. Both CTL flush words are zero. |
| 2, 3 | 28,858, 33,281 | Before/after DCS `0x10`. Same engine and CTL state. The post-`0x28` and post-`0x10` waits are 20,629 µs and 101,254 µs, respectively. |
| 4 | 134,535 | Last boundary before the INTF2 EN write. Both EN/STATUS 1/1. |
| 5–38 | 134,557–174,786 | Every one of the 34 stop polls sees INTF2 EN/STATUS 0/1, INTF1 1/1 and FLUSH/INTF_FLUSH 0/0. INTF2 goes from frame 294637, line 668 to frame 294642, line 270 in 40,229 µs, a net 11,152 lines at about 120 Hz. INTF1 advances 11,151 lines over the same polls. No `stop1` row or CTL submission intervenes. |
| 39–41 | 174,799–174,837 | Undo, fetch and enable boundaries. INTF2 remains at 0/1 and both counters still scan. |
| 42–48 | 174,861–180,905 | The undo start row reads INTF2 EN back at 1. Both engines continue scanning. FLUSH is `0x80000000`, INTF_FLUSH is `0x6` at start and the next six polls. |
| 49 | 182,158 | Both flush words first read zero, on the next master frame with INTF1/2 at lines 2/3. This is the rollback’s wake flush with EN2 already restored. |
| 50–172 | 182,242–190,505 | First frame-edge measurement, ending at frame 294644. Both engine and CTL states return to the pre-stop values while counters continue. |
| 173–212 | 287,687–290,507 | Second frame-edge measurement, ending at frame 294656, with continued joint motion. |
| 213–215 | 290,518–436,640 | Undo issues DCS `0x11` then `0x29` and returns restored. Both EN/STATUS pairs remain 1/1 with flush words clear. |

The stop request is visible in EN. STATUS reflects effective scanning, corroborated by the counters, rather than a stale bit. The first CTL flush in this sequence occurs after rollback restores EN2. The capture therefore demonstrates sustained bonded scanning with INTF1 active, and supplies no experiment in which a flush alone stops INTF2. The pinned Ubuntu handoff establishes the reference mechanism: the master gates both effective engines, while releasing a still-enabled slave’s local request requires a reset and route submission with its video resources still bound. Linux then issues the slave’s disable and waits for its final frame before full cleanup.

## Stop registers and remaining qualification

- **Three independent registers.** `TIMING_ENGINE_EN +0x000` bit 0 is the request. `STATUS +0x26c` bit 0 is the effective state on DPU 12, as in Linux on DPU core ≥5. `COUNT_EN +0x0a8` controls the raw count words at `+0x0ac/+0x0b0`, which advance even when COUNT_EN reads zero. Linux returns software zeros for counters when inactive. Native can corroborate a stop using stable raw registers with clocks still readable.
- **The reference uses separate deadlines.** At 120 Hz a frame is about 8.33 ms. The 40 ms slave-only wait spans nearly five frame periods. Ubuntu drains the master first using its frame completion, verifies both effective STATUS bits before reset, waits up to 40 ms for the handoff flush, and then allows the slave to stop at its own frame boundary. The early CTL reset is required to consume the handoff and precedes any unbinding of pixel resources.
- **DCS completion is local to the host.** `sendDcs` issues DCS short writes with no BTA/panel read and marks success from DMA_DONE on both hosts. It samples DSI_ACK_ERR before and after and stores the FIFO status, but does not use either as an acknowledgment or include them in the transition log. Boot ACK_ERR was zero on both hosts. DMA timing, a zero error latch, backlight darkness and a successful rollback all leave actual panel acceptance of `0x28/0x10` unknown. The stock DT packets also request no ACK. Existing snapshots expose host state only.
- **Host replay.** The previous independent-engine model could reproduce y12n's truncated error with either a dropped EN write or a permanently stuck slave. The updated timed model replays all 216 y12o samples at their captured intervals and checks both links and the CTL state. It compares counter position within 12 lines of the recorded values to allow nominal clock and MMIO sampling differences. The fixture rebases the timestamps to row 0 and preserves all sampled words. The same model and starting phase exercise the corrected off/wake path. The old driver fails that assertion, and the corrected path freezes both counters, restores the bonded route and returns both links to lockstep. [Timed replay](../../package/PianoDpu/tests/dpu_panel_host.cpp)

## Read-only checks on an already running boot (y12n baseline)

Use `sudo sysctl -n machdep.piano_dpu_snapshot` when the existing boot exports it. The snapshot takes loads only, caches each sample for 100 ms and timestamps/serializes new samples (`DpuSnapshot.cpp:33,105–129,137–160`). Retain the full output and require a new serial, `gate open` and valid register reads. Independent calls close together may return the same sample.

| Observation | What it can settle |
| --- | --- |
| Two fresh snapshots at least 100 ms apart while lit, during an already-occurring dark interval and after wake | Compare both INTF EN, STATUS, COUNT_EN, FRAME_COUNT/LINE_COUNT, and CONFIG/FETCH_START. Check CTL TOP/ACTIVE/MASTER and FLUSH/INTF_FLUSH, TOP0 vsync bits 27/29 and underrun bits 26/28, and gate/byte/pixel clocks. Increasing frames with `EN=STATUS=1` in the dark identify backlight-only operation. With clocks valid, both inactive and stable frame counts substantiate an effective stop. For the first failed attempt, an independently timed snapshot during the stop window could discriminate `EN2=1` from `EN2=0, STATUS2=1`. Confirm whether INTF2's frames still advance inside the window. Sticky IRQ bits alone have no event ordering. |
| Log and IORegistry reads alongside those snapshots | Read `ioreg -r -c NativeGopDisplay -l` for `piano-display-power`, `ioreg -r -c PianoDpu -l` for `piano-dpu-panel`, and `ioreg -r -c PianoBacklight -l` for `piano-backlight-control`, `piano-backlight-primary-lit` and `piano-backlight-secondary-lit`. Correlate with the pre-first-on message, each BL_EN readback, touch park and panel-gate messages. An `awake` DPU property also describes a recovered, boot-latched fault with an otherwise safe awake path, so use the refusal log to identify that case. |
| DSI CTRL/STATUS0/TRIG_CTRL/INTR_CTRL/TPG and lane words in the same snapshots and the saved console | Correlate controller enable, video busy, command DMA state and DCS timings. The y12n snapshot lacks ACK_ERR. The current opt-in reads the host ACK error latch on both links. There is no existing read-only sysctl that issues a DCS status read or exposes intra-transition EN/counter history, CTL reset or VBIF halt ACK. |

An already recovered y12n boot cannot replay its one-shot failure through reads. Even on an armed boot, the stock snapshot cadence is too slow to guarantee two fresh samples in a 40 ms window. A well-timed snapshot can establish the request readback at one point. Use the transition trace below for counter motion during the stop window. Panel acceptance would require panel-side telemetry or a specifically validated DCS power-mode read.

## Trace read list

With `piano-dpu-panel=1`, save the console through the first runtime off, any rollback, and the subsequent wake. The worker prints `PianoDpu: m5` lines after the transition and publishes the same text as the read-only `piano-dpu-panel-trace` IORegistry data property on `PianoDpu`. Read the property after the off and again after a dispatched wake. It holds the latest completed transition, including any rollback in that call. A gate refusal or a wake with no hardware sequence leaves the preceding trace available. Use the trace serial to match the property and the console.

```sh
ioreg -a -r -c PianoDpu | python3 -c 'import plistlib,sys; print(plistlib.load(sys.stdin.buffer)[0]["piano-dpu-panel-trace"].decode(),end="")'
```

The header gives the serial, direction, selected CTL, result, step, whether rollback restored the panel, retained row count and number overwritten. Require `dropped=0` when using the capture as evidence for the entire transition. Each row gives the serial, sample index, monotonic microseconds, step name, and `B` for a step boundary or `P` for a poll. The three slash-delimited groups contain raw hexadecimal words in this order.

| Group | Read list |
| --- | --- |
| INTF1 at `0x0ae36000`, then INTF2 at `0x0ae37000` | `TIMING_ENGINE_EN +0x000`, `STATUS +0x26c`, `FRAME_COUNT +0x0ac`, `LINE_COUNT +0x0b0` on each link |
| Selected CTL (CTL0 at `0x0ae16000` on y12n) | `TOP +0x014`, `INTF_ACTIVE +0x0f4`, `FLUSH +0x018`, `INTF_FLUSH +0x110` |
| Separate `dcs 28/10` and, after the wake fix, `dcs 11/29` lines (one per command), DSI0 then DSI1 | Whether the trigger was attempted, DMA_DONE mask, elapsed microseconds until completion or deadline, front-porch wait, last polled `INTR_CTRL`, and raw `ACK_ERR +0x068` and FIFO status before and after per-link restore. In each mask bit 0 is DSI0 and bit 1 is DSI1. For each ACK/FIFO pair, the mask on the corresponding ACK pair tells which links were read. Host DMA_DONE and ACK_ERR describe the host transfer. |
| Wake `pre-reset status` and `host` lines | STATUS0 on each host before reset, then CTRL, STATUS0, LANE_STATUS and CLK_STATUS sampled before each wake command. `dsi-reset B/P` bracket each attempted host reset with the ordinary INTF/CTL snapshots. |

- For the y12n branch, retain the `stop2 B` row before the store, every `stop2 P` row through the 40 ms result, and the `undo B` row before restart. A running INTF1 is the control. On INTF2, compare EN and STATUS bit 0 across the window and follow both raw counters. Successive frames with EN=0 and STATUS=1 show that the request reads back while timing stays live. EN=1 through the deadline shows that the zero store did not produce a visible EN change. A stable frame/line pair with STATUS=1 needs confirmation against the earlier motion control and valid clocks. The MMIO reads within a row are sequential, so interpret a single frame/line pair near a rollover in context.
- For the fix, follow `drain`, `stop1`, `ctl-reset`, `handoff` and `stop2`. Both STATUS bits must clear before reset. The handoff must show ACTIVE 0x4 with the INTF flush submitted and then cleared, before the `stop2` boundary. Check both EN/STATUS pairs and raw counter freeze at `fetch-off` and `end`. During wake, `route-on` must show ACTIVE 0x6 and the flush clear before the `enable` boundary. The y12p baseline checked `start` and `rate` before DCS. The wake fix below requires DCS before `route-on`, `start` and `rate`. Snapshots give MASTER directly. Keep the normal readiness, blank/backlight, touch, recovery and refusal lines alongside this trace.
- The preallocated ring holds 2,048 rows. It samples timing-stop, alignment, start, flush and frame-edge polls, and records boundaries around DCS and fetch. The timed host replay used 150 ns per MMIO access and measured up to 1,650 ns of sample reads within a continuing poll's wait budget. All 41 INTF2 status polls had identical relative times with tracing on and off, for 0 ns of added poll drift against a 3,608 ns model line. Boundary and terminal rows take their reads at those boundaries. The next RAM run supplies hardware timing evidence.

## y12o stop fix gate (historical)

Path: IOMobileFramebufferLegacy power hook → NativeGopDisplay copy worker → confirmed dual backlight blank and touch hold → PianoDpu worker/flip lock → bonded DCS off → CTL0/INTF stop → wake or rollback → DCS on/PCC/touch/backlight. Ubuntu's counterpart is DRM atomic disable/enable → shared DPU encoder handoff → NT36532 unprepare/prepare with both DSI hosts available → DSI/PHY power sequencing.

Checks: **ours-OK** for counter readiness, dual backlight blank and touch park, DCS host DMA, retained configuration through the observed rollback and boot-latched refusal. **ours-missing** in the baseline for selected CTL master validation, prior-flush drain, and the shared-CTL master stop and handoff. **unknown** on hardware for reversible route restoration, VBIF halt ACK, DSI idle and panel-side receipt. The trace ring stays enabled for this opt-in.

Root cause: **Closed.** The independently polled slave reports EN=0 but STATUS=1 and advances its frame and line counters across the full 40 ms alongside INTF1. With ACTIVE=0x6 and no CTL flush during the wait, the active master keeps the slave scanning. The slave-first timeout prevents the master disable that the shared path needs. The pinned Ubuntu reference stops INTF1 first, waits for the master’s final frame, checks both inactive, resets the stopped CTL, flushes and starts the slave-only route, then clears and drains the slave’s local request.

ONE change: Replace the independent slave-first timing polls under `piano-dpu-panel=1` with a bonded stop and restart transaction. Validate the live route and drain the prior flush, stop INTF1 and prove both effective engines quiescent before reset, submit the slave-only route (`ACTIVE=0x4`, `MASTER=0`), then clear INTF2 and confirm both requests and counters are stopped. Keep pixel resources bound. For wake or a recoverable rollback, restore fetch and the saved route (`ACTIVE=0x6`, `MASTER=0x2`), latch the shared flush before enabling the pair, then prove lockstep before wake DCS. Refuse further work if reset or route submission cannot be verified. Both link EN/STATUS pairs, counter freeze, restored CTL route, lockstep and wake commands are required on the next qualification.

## y12p qual1 first wake

The run reached the runtime display block. The console records dual backlight blank, touch parked and a successful off transaction at lines 75033–75337. The wake attempt and C920 grade both recorded a failure before the runner found ABL. The snapshot text for the two DSI reads is empty of register data. Refer to the console for the transition. No panic stack, watchdog-pet timeline or reset-cause record survived in this capture. The runner skipped the mailbox dump because it had no qualified pre-USB reset path (run-ram.log:1–5).

| Serial and rows | Time from its row 0 (µs) | Observed result |
| --- | ---: | --- |
| 1, 0–116 | 0–7,994 | Both EN/STATUS 1/1 at ACTIVE 0x6. Both counters reach the master frame edge, with no pending CTL flush. |
| 1, 117–120 | 8,007–42,394 | Both links complete LP DCS 28 and 10 (DMA_DONE mask 3 in 1,185 and 1,265 µs, zero sampled ACK_ERR). Host completion gives no panel readback. |
| 1, 121–129 | 144,415–150,897 | INTF1 EN clears first. The terminal poll sees both STATUS 0 and INTF2 EN still 1. CTL reset follows. |
| 1, 130–143 | 150,912–159,695 | ACTIVE changes 0x6 → 0x4, CTL flush clears, the slave runs on its own before its EN and then STATUS clear. Fetch-off and end both have EN/STATUS 0/0 and identical raw counters. |
| 2, 0–3 | 0–205 | The four stopped counters equal the first serial’s end. Both requests and effective states are off. CTL reset completes, then fetch restore and route submission begin. |
| 2, 4–6 | 228–263 | ACTIVE 0x6 with FLUSH/INTF_FLUSH 0x80000000/0x6, then 0/0. Both EN/STATUS remain 0/0 at the enable boundary. |
| 2, 7–11 | 286–587 | Both read 1/1. Both counters start at frame zero, then reach frame one together. |
| 2, 12–65 | 97,092–100,561 | Both advance from frame 12 to 13 in lockstep. The CTL stays at ACTIVE 0x6 with clear flush words. |
| 2, 66–67 | 100,575–159,376 | DCS 11 begins with both at frame 13, line 16/17. At failure they reach frame 20, line 146/147. The porch wait succeeded and the command was triggered, then DSI0 lacked DMA_DONE at 50,747 µs. There is no DCS 29 boundary. The y12p trace printer only emitted DMA detail for off, so DSI1 completion and wake ACK/FIFO state are unknown. |

### Resume comparison against the pinned Ubuntu source

Native line numbers below refer to the failed baseline (`43c2a96c`). The reference's panel bridge pre-enable prepares NT36532 and sends **both** 11 and 29, with a 120 ms interval. The later panel enable enables the backlight. The encoder's atomic enable configures video before the bridge enable finishes, but the INTF only begins scanning at the encoder's post-kickoff. Qualcomm's bridge enables host video mode before prepare and overlays command enable during each transfer. With the INTF still stopped and the freshly reset host video engine idle, its command transfer needs no live video porch. [Pinned Ubuntu DSI host](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c) · [Pinned panel](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/panel/panel-novatek-nt36532.c)

| Ubuntu v1.4.0 file and lines | Native file and lines | Comparison |
| --- | --- | --- |
| `dpu_encoder.c:1385–1506`, `dpu_encoder_phys_vid.c:586–666` drain master, reset CTL, flush slave route and retire its last frame | `DpuPanel.h:490–565` | Same shared CTL handoff. **ours-OK** in serial 1. |
| `dsi_manager.c:378–386`, `dsi_host.c:1181–1203,2493–2507` disable operation mode and software-reset both hosts. Linux explicitly identifies the INTF stop leaving DSI video unable to finish and blocking commands. | `DpuPanel.h:527–565` only stops DPU INTFs and clears fetch. | **ours-missing**. DPU timing and DSI/video FSM are separate. Restarting INTF proved the DPU counters recovered. It did not prove DSI video reached idle. |
| `dsi_manager.c:388–411,224–270`, `dsi_host.c:2524–2576,2639–2668` manage DSI0 PLL, DSI1 external clock, IRQ, PHY, host clocks and regulators, then reset the controllers after PHY sync. `dsi_host.c:2614–2633` preserves CTRL and CLK_CTRL around the SM8750 software reset. | `DpuPanel.h:527–565`, `DpuSlices.cpp:759–786` | Different, retained power. Native sleep switches neither the DSI input clocks nor the PHY or bias supplies. Both host enable bits and readable registers are required at wake. Boot CTRL is 0x1f7 on both links. DSI0 STATUS0 0x2008 and DSI1 0x8 were boot samples during video. The late host video-busy state and actual escape/byte clock and lane states during wake are **unknown**. Use the reference reset mask temporarily and retain PHY/PLL and boot clock settings. |
| `dsi.xml:96–115,189–196,268–300`, `dsi_host.c:898–911,1206–1259,2188–2252` prepare LP DMA with software trigger, overlay CMD and ENABLE, wait for VIDEO_DONE only if busy | `DpuPanel.h:111–233,586–603` LP bit set in CMD_DMA_CTRL (boot 0x14000000), CTRL OR CMD, SW trigger 4, both FIFO DMAs armed. Boot TRIG_CTRL 0x80000004 has block-in-frame clear, so old wake waits for INTF1 front porch. | Same LP packet and software trigger. **ours-missing** for an idle-host path independent of the porch. LP selects escape-mode data transmission and permits LP11 idle between commands. It does not prove all lanes, including the clock lane, reached LP11. Ubuntu requests a continuous clock for piano. Native preserves the boot clock-lane policy, including PHY selection (LANE_CTRL 0x01000000 on both at boot), and avoids ULPS/PHY writes. The boot video sample LANE_STATUS 0x1f00 shows all five lanes out of ULPS. The stopped lane state is **unknown**. |
| `dsi_manager.c:523–583`, `dsi_host.c:1464–1495,2400–2410` prepare both, DSI1 write triggers DSI0 then DSI1 using a DMA buffer, complete via IRQ on triggering host | `DpuPanel.h:151–223` dual input FIFOs, back-to-back DSI0 then DSI1 stores, polls both done bits | Same ordering, distinct DMA plumbing. Both suspended hosts must be reset and prepared. **ours-OK** for off; **ours-missing** for restoring an owned FIFO after wake timeout. |
| `dsi_manager.c:301–335`, `drm_panel.c:125–179,605`, `panel-novatek-nt36532.c:103–167,652–673,780,821–852` prepare supplies/reset/vendor state, send 11 → 120 ms → 29 → DSC PPS | `DpuPanel.h:586–603` starts video, sends 11 → 120 ms → 29 → 10 ms | **ours-missing** for command-before-timing. The retained-power path uses boot panel setup and DSC PPS. Panel-side receipt and preservation of that setup are **unknown**. |
| `drm_atomic_helper.c:1720–1745`, `dpu_encoder.c:1862–1913,2253–2272`, `dpu_encoder_phys_vid.c:668–683` configure, flush and start timing after panel prepare | `DpuPanel.h:336–463,586–603` restores fetch, bonded route and CTL flush, then starts and checks the pair before DCS | Same flush-before-timing rule. **ours-OK** in serial 2 for CTL and two INTFs, **ours-missing** for the ordering relative to DCS. |
| `drm_panel.c:163–179,763–790` enable panel backlight in bridge enable | `NativeGopDisplay.cpp:1524–1570,1678–1724,1932–1952,2268–2291` IOMFB gated hook posts intent and returns. The copy worker drains the current job, dispatches panel work, then signals display power and backlight through the work loop on success. | **ours-OK** for normal gate ownership. **ours-missing** for forwarding a completed failed wake as a backlight-only wake and releasing touch, presentation and the VBL tick. |

### Reset forensics

The decoded 27.3 IOMobileFramebuffer cache exports `IOMobileFramebufferRequestPowerChange` and the swap and power notification APIs. The first callers inside WindowServer are unknown. The kernel ABI in `NativeGopDisplay.cpp:140–142,1524–1569` combines the family’s system and API states, updates the hardware-powered word and issues a power notification. It releases the copy lock immediately after queuing the intent. The M5 copy call runs outside that lock and outside IOMFB’s work-loop gate. The sheng observation code (`ShengDpuObservation.c:24–34,64–72`) agrees on the INTF registers and does not drive resume.

The concrete stuck edge is `setPanelPower` returning false before touch release, followed by `PanelPower.finish` leaving readiness false. The copy worker then sleeps indefinitely at `NativeGopDisplay.cpp:2287–2289` unless another power edge queues work. Presentation and the off-gated VBL tick stay parked. In addition, the copy thread's synchronous `PianoDpuPanel` call had an unbounded reply wait at `DpuSlices.cpp:1190`. The DPU worker also held the shared flip lock for the transaction. Those are distinct dependencies. In this capture the DPU reply arrived, proven by the NativeGopDisplay failure at line 76214, so neither the request wait nor the flip lock explains a further 25-second kernel stall. Video, power and GPU jobs continued, with a final GPU timestamp about 24 seconds after the M5 end row.

The boot enabled a 32 s Gunyah bite and a 6,900 s scheduled reset. Patch `0063` shows that `piano_reset_tick` pets the watchdog every 10 s from a high-priority kernel thread call and does not take either display lock. A watchdog bite fits the interval if pets stopped, and an abandoned DMA or interconnect fault remains a candidate. Returning to ABL is common to several reset sources given this boot's restart reason. The reset occurred well before the scheduled reset, there is no panic line, and the actual last pet, reset source and blocking thread are **unknown**. The confirmed indefinitely blocked path is display presentation, independent of the reset attribution.

## Historical y12p wake-sequencing proposal

Path: WindowServer/IOMobileFramebuffer API and IOMFB family system/API gate → NativeGopDisplay publishes off, queues intent and drains the copy job → PianoBacklight confirms dual BL_EN and PianoTouch parks → PianoDpu worker drains and stops the shared CTL, resets the retained DSI hosts for LP commands, then restores bonded video → PCC replay, touch release, display power, backlight and VBL on the copy/work-loop boundary. Ubuntu and sheng counterparts are listed above.

Checks: **ours-OK** in the capture for the master-first sleep, two LP DMA completions on off, and bonded CTL/fetch/INTF restart. **ours-missing** for clearing both stale host video engines, both wake commands before video timing, cleaning up timed-out wake DMA and releasing the waiting display path. **unknown** for retained lane state in the command window, panel receipt, and reset provenance.

Root cause: Confirmed sequencing bug. The baseline parks the shared DPU path without resetting either DSI video engine, restarts both timing engines, then overlays CMD mode and relies on a front-porch DMA. DSI0 times out despite the restored CTL and moving INTF counters. Ubuntu specifically requires a host reset for the video-engine blockage created by INTF disable. A retained stale DSI video FSM is the best-supported hardware mechanism and needs confirmation from the next wake with host status sampled. The failure reply permanently parks the display copy/VBL path. The reset mechanism is still unproven.

The change after y12p moved wake DCS into a bonded command window before restarting timing, under `piano-dpu-panel=1`. With both INTFs quiescent, it disables video and software-resets both DSI hosts with the SM8750 force-clock mask, sends the paired LP 11 → 120 ms → 29 sequence in command-only mode, restores the saved host controls, then restores fetch, the shared CTL route and lockstep timing. It bounds a failed wake reply, aborts a timed-out command owned by the stopped-host path, attempts scanout restoration once when safe, and latches M5 unavailable for the boot. The fallback can recover software progress while the panel state remains unknown.

## M5R dark wake and pinned Linux comparison

The November 26–27 M5R evidence is a dark-panel failure after wake. On RAM `v131y12t-qual1`, the sampled CTL, DSC, pipe/fetch, INTF and DSI compression route matched the bright control byte for byte. Both INTFs counted and both KTZ8866 ICs reported `pwm_raw=0x07ff`. The optical YAVG stayed about 8.2–8.9 and all ten post-wake source captures returned `EBUSY`. Counter motion and register matches establish the sampled DPU state. The active pixels at the dark exposures, emitted DSI traffic, and panel decoder state remain unknown. [November 27 RAM evidence](../results-2026-11-27.md#ram-qualification)

The native column below describes `0f321b51e` on the bonded `enginesStopped` path. **`ours-OK`** means the named operation or readback exists in code, with hardware evidence called out where available. **`ours-missing`** identifies an operation absent relative to the reference and by itself establishes no requirement for retained-power wake. **`unknown`** marks hardware effects or state the sources do not establish. The probe column records coverage at the start of this audit and the fields added here. [Native off and wake](../../package/PianoDpu/DpuPanel.h#L559-L723)

### Board, panel and transition order

The pinned board DTS supplies both P81 variants from VDDIO 1.9 V, positive AVDD and negative AVEE (5.8 V magnitudes), with active-low reset on TLMM 98. TLMM 100 selects `42-02-0a` (CSOT, low) or `35-02-0b` (BOE, high). Both use the same bonded D-PHY and panel supplies. Each host has a 1.2 V supply and each PHY a 0.88 V supply. The pin translates stock `qcom,mdss-dsi-panel-phy-timings` into `qcom,phy-timings` on each PHY. The DTS provides the wiring and PHY bytes, and the panel driver supplies display modes and DCS command lists. The desktop DTS enables the two hosts/PHYs and panel supplies. [Board supplies and backlight](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts#L248-L280) · [Board DSI nodes](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts#L1437-L1545) · [Desktop enablement](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-desktop.dts#L306-L330)

The following Linux file and line references are from `yzddmr6/linux-sm8750-piano@83f0f89bed14a0cb7d582a26e72bced0387870f4`. The `panel-novatek-nt36532.c` P81 profiles share LP video mode, RGB101010, 3200 × 2136 overall (1600 × 2136 per link), 144/120/90/60 Hz, and DSC 1.1 at 10 bpc and 8 bpp with two 800 × 24 slices per link and block prediction. Both request continuous clock. The standalone `csot,ppc100hb1-1` profile in the same file has different timings and PPS order. [P81 modes and DSC](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/panel/panel-novatek-nt36532.c#L420-L470) · [P81 variant selection](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/panel/panel-novatek-nt36532.c#L805-L875)

| Step | Pinned Ubuntu | PianoDpu M5R comparison |
| --- | --- | --- |
| 1. Bind the pair | DSI0 is clock master and sources both links. DSI1 is the synchronized write caller. Each link has four lanes `0,1,2,3`. | **`ours-OK`** for DPU master validation (`INTF1` from DSI0), both hosts and DSI0→DSI1 triggers. **`unknown`** for physical clock mux and lane state during the dark wake. |
| 2. Blank and drain | DRM panel bridge disables the backlight before DPU encoder teardown. NT36532 has `prepare`/`unprepare` callbacks. `drm_panel` handles backlight in enable/disable. | **`ours-OK`** for external dual backlight blank, pre-stop drain (`CTL_FLUSH` and frame edge) and touch park. `DpuPanel.h:570–582` and the NativeGopDisplay handoff. |
| 3. Sleep commands and DPU stop | DPU stops master INTF1, hands off CTL and stops local INTF2. Panel unprepare sends `0x28`, waits 20 ms, sends `0x10`, waits 100 ms on powered hosts. `panel-novatek-nt36532.c:169–197,821–836`. | **`ours-OK`** for both sends and 20/100 ms, and for the master/CTL/slave stop. Off DCS runs while scanout still moves, with a porch wait when required, before master stop. **`unknown`** for panel receipt. `DpuPanel.h:183–225,583–648`. |
| 4. Off reset and power-down | Assert panel reset, drop AVEE/AVDD/VDDIO with 2/3/2 ms delays. Disable host mode and soft-reset DSI0 then DSI1, disable IRQs, save PLL state, power off DSI1 then DSI0 and put PHYs down. `panel-novatek-nt36532.c:122–132,184–198,452–459`, `dsi_manager.c:178–193,358–411`. | **`ours-missing`** for this cold shutdown sequence. M5 retains panel, hosts, PLL and PHY power, and clears programmable fetch after both INTFs stop. Host soft reset is on wake. `DpuPanel.h:639–649,665–677`. |
| 5. PHY and host power-up | Distinct `PHY_RESET` on both before either PLL operation. Enable both PHYs, then power/program DSI0 and DSI1. Both resync FIFOs are toggled before `CLK_EN_SEL` is released on each PHY. `dsi_manager.c:137–164,212–252`. | **`ours-missing`** for cold PHY reset, resync and clock-select release. M5 uses the boot configuration of this retained path. **`unknown`** whether its clocks and FIFO synchronization hold across M5. |
| 6. Reset both controllers and enter the command window | The SM8750 post-PHY-sync host reset preserves `CTRL`/`CLK_CTRL`, DSI0 then DSI1. Both hosts then enable video mode before panel prepare. Transfers overlay CMD mode and wait for `VIDEO_DONE` only when busy. `dsi_manager.c:254–270,301–335`, `dsi_host.c:1220–1259,2188–2252,2591–2635`. | **`ours-OK`** for both INTFs quiescent, mask/1 µs host reset, both hosts ready before either trigger, and both wake commands ahead of restarted timing. The operation bits are cleared on both hosts first, DSI0 reset precedes DSI1, saved clocks restore on both, then CMD-only is set on both. No code evidence of reversed master/slave ordering. **`unknown`** for physical link behavior in the command-only window. `DpuPanel.h:494–533,654–697`. |
| 7. Reset/power the panel and initialize | Enable VDDIO, AVDD, AVEE in order, wait 2–3 ms after each, and drive physical reset `0/10 ms → 1/3 ms → 0/3 ms → 1/15 ms`. Init covers ESD, CABC, vendor pages, compression (`0x90/91/92/9d`) and rate (`0xb2/b3`). CSOT and BOE differ in ESD D1, vendor block and rate-write order. `panel-novatek-nt36532.c:103–167,473–650,839–852`. | **`ours-missing`** for panel reset and init replay. M5 assumes panel configuration survives sleep-in. Its allowlist has only the four sleep/display commands. **`unknown`** for retained vendor, rate and decoder state. `DpuPanel.h:77–96,140–150`. |
| 8. Panel PWM and brightness | Page `0x10` writes `0x51 0x0f 0xff` and `0x53 0x24` in the CABC PWM init block before Sleep Out. `0x51 0x5c` on page `0x23` is a CABC table entry. The two KTZ8866 ICs power the LCD LEDs separately. `panel-novatek-nt36532.c:530–538,559–599`, `sm8750-xiaomi-piano.dts:1315–1324,1399–1417,1460–1463`. | **`ours-missing`** for DCS brightness/control-display replay. **`ours-OK`** for external PWM restore readback in the RAM run. **`unknown`** for panel-side PWM/CABC state and actual light output attributable to the LED strings. |
| 9. Sleep Out and Display On | Short DCS `0x11` → 120 ms → `0x29` in both P81 prepares. `panel-novatek-nt36532.c:652–654`. | **`ours-OK`** for the same 120 ms before `0x29` and an additional 10 ms before restoring video. The pin supplies no evidence for a longer Sleep Out wait. **`unknown`** for panel acceptance, even if both host DMAs finish. `DpuPanel.h:93–96,687–698`. |
| 10. Send DSC PPS | After `0x29` and before video timing starts, pack and send the 128-byte DSC PPS, with first-line BPG offset 13 and range-0 max QP 8. `0x0a` is the MIPI DSI **packet data type**. `panel-novatek-nt36532.c:656–673`, `drm_mipi_dsi.c:721–729,1553–1569`, `mipi_display.h:46–51`. | **`ours-missing`** for PPS replay. Native sends only short DCS. Matching host DSC routing describes the host packetizer and DPU configuration. Panel PPS retention across powered sleep, and whether restoring it alone would change the symptom, are **`unknown`**. |
| 11. Restore scanout and backlight | Atomic enable configures video, panel bridge enables the external backlight, and the shared CTL flush precedes DPU post-kickoff timing. | **`ours-OK`** for restoring both saved host `CTRL` words, fetch and bonded CTL route, then enabling INTF1 and INTF2 and checking lockstep before PCC, touch and presentation resume. The sampled route and counter motion agree with this path. Active source pixels during the dark exposure are **`unknown`**. `DpuPanel.h:370–469,699–723`. |

[Linux bonded manager](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_manager.c#L125-L411) · [Linux panel callbacks](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/panel/panel-novatek-nt36532.c#L103-L233) · [P81 init and wake](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/panel/panel-novatek-nt36532.c#L473-L685) · [PPS transfer](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/drm_mipi_dsi.c#L719-L731) · [DRM panel backlight ordering](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/drm_panel.c#L763-L849) · [DPU kickoff](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/disp/dpu1/dpu_encoder_phys_vid.c#L668-L683)

### DSI host register comparison

Offsets below are relative to **each** DSI controller resource, including the DSI 6G `+4` shift from `dsi.xml`. Linux's generic `dsi_sw_reset` pulses reset for 20 ms. On full power-up it programs video timing and video compression **before** that reset and applies `dsi_ctrl_enable` **after** it. The later SM8750 reset runs after both PHYs are synchronized and restores only `CLK_CTRL` and `CTRL`. The pin therefore relies on the programmed timing and host config surviving that specific reset in its own sequence. It documents no register reset map or proof of survival with the firmware configuration M5 retains. In particular, matching after-wake `VIDEO_COMPRESSION_MODE_CTRL` narrows persistent loss of that word, while a transient change around reset is still **`unknown`**. [Host setup and generic reset](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L2523-L2635) · [Host register XML](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml)

| DSI0 and DSI1 word | Linux use at the pin | Native wake and status | Snapshot |
| --- | --- | --- | --- |
| `CTRL +0x004` | Lane bits 4–7 and clock bit 8 in `dsi_ctrl_enable`, operation bits 0–2 in `dsi_op_mode_config`. `dsi_host.c:935–955,1181–1203,2614–2633`. | **`ours-OK`**. Save and check the full word, clear operation bits on both, set enable+CMD with video clear, then restore the full pre-reset word on both before INTF restart. Lane enables are restored through this word. | Existing `ctrl`. |
| `CLK_CTRL +0x11c` | Save, OR `0x200b3f`, restore in post-sync reset. `dsi_host.c:2614–2633`. | **`ours-OK`**. Same force mask, restore and verify saved word on both links. `DpuSlices.h:55–57`, `DpuPanel.h:502–519`. | Existing `clk-ctrl`. |
| `RESET +0x118` | Generic reset (20 ms) and SM8750 post-sync reset (1 µs), discrete from PHY reset. `dsi_host.c:1150–1179,2614–2633`. | **`ours-OK`** for the 1 µs pulse and deassert/busy readback. **`unknown`** for the set of other words actually affected. | Added `reset`. A later zero only reports deassertion. |
| `PHY_RESET +0x12c` | Reset before dual-PHY setup. The manager explicitly says this reset can clear PHY1 registers already set by PLL0 operations. `dsi_host.c:2421–2431`, `dsi_manager.c:145–164`. | **`ours-missing`** for a PHY reset. M5 writes the separate host reset only. | No extra PHY reset-latch read. Compare live PHY state below. |
| `VID_CFG0 +0x010`, `VID_CFG1 +0x020` | Programs traffic mode, BLLP stops, pixel format, VC, wide bus and RGB swap. `dsi_host.c:833–856`. | **`unknown`** after reset. Native has no save/restore for either word. Changing `CTRL` is its only command-to-video switch. | Existing `vid-cfg0`, `vid-cfg1`. |
| `ACTIVE_H +0x024`, `ACTIVE_V +0x028`, `TOTAL +0x02c`, `ACTIVE_HSYNC +0x030`, `ACTIVE_VSYNC_HPOS +0x034`, `ACTIVE_VSYNC_VPOS +0x038` | Programs per-host compression-adjusted active/total and sync. `dsi_host.c:1020–1124`. | **`unknown`** for reset retention of each word. None is replayed on M5. | Existing `active-h`, `active-v`, `total`, `hsync`, `vsync-hpos`, `vsync-vpos`. |
| Raw timing/FIFO words `+0x1e4/+0x1e8/+0x1ec` | Native timing-shadow interpretation comes from the VFP path. The pinned host reset does not replay this trio. | **`unknown`** for retention or side effects. M5 reads the raw trio in the probe and pulses `+0x1ec` in FIFO cleanup. This is an explicit write during DCS completion, independent of host reset. `DpuPanel.h:123–137`, `DpuHw.h:165–170`. | Existing raw `timing shadow` words. |
| `VIDEO_COMPRESSION_MODE_CTRL +0x2a0` | Compressed pixel data type, word count, packets per line, EOL bytes and enable. `dsi_host.c:961–1016`. | **`ours-OK`** for matching bright/dark readback in M5R. No native replay. **`unknown`** for transient reset effects, packet emission, or panel PPS. | Existing `video-compression`. |
| `COMMAND_COMPRESSION_MODE_CTRL +0x2a8`, `CTRL2 +0x2ac`, `CMD_CFG0 +0x040`, `CMD_CFG1 +0x044` | Compression stream and pixel write formatting are set in Linux's command-mode display branch. P81 uses the video branch and overlays command enable for LP transfers. `dsi_host.c:872–895,994–1016,1125–1147,2222–2227`. | **`unknown`** for reset retention. Native does not replay them. Short FIFO DCS is distinct from command-mode pixel streaming. | Existing `cmd-compression`. Added `cmd-cfg0/1`. |
| `CMD_DMA_CTRL +0x03c`, `DMA_LEN +0x04c`, raw `+0x390`, `TEST_PATTERN_GEN_CTRL +0x15c` | Default LP DMA, memory-backed transfer using address/length and software trigger. `dsi_host.c:898–911,2400–2410`. | **`ours-OK`** for per-transfer FIFO setup on both and restore of the values saved before each command, or cleanup after a reset on the owned timeout path. These saves happen **after** the first wake reset. **`unknown`** for any changes to their pre-reset values. `DpuPanel.h:123–137,152–192,701–710`. | Existing `cmd-dma`, `+390`, `tpg`. Added `dma-length`. FIFO payload and trigger ports are outside the read list. |
| `TRIG_CTRL +0x084` and `TRIG_DMA +0x090` | Select TE/SW trigger and hardware block-in-frame, trigger DSI0 then DSI1. `dsi_host.c:902–911,2400–2410`, `dsi_manager.c:569–583`. | **`ours-OK`** for requiring the SW selector and the trigger order. **`unknown`** for the other selector bits across reset. Native does not replay `TRIG_CTRL`. Wake reads it after reset and refuses if either SW selector is lost. | Existing `trig`. The trigger port is issued only by the existing transfer path. |
| `INTR_CTRL +0x110`, `STATUS0 +0x008` | IRQ mask/DMA and video completion; host FSM and error state. `dsi_host.c:1220–1259,1667–1702,2188–2252`. | **`ours-OK`** for idle DMA/video checks after reset and polling both completion bits. Native preserves enables sampled per transfer and clears DMA_DONE in existing cleanup. **`unknown`** for pre-reset IRQ enables and the meaning of a single busy sample during video. | Existing `intr`, `status0`. |
| `LANE_SWAP_CTRL +0x0b0`, `LANE_CTRL +0x0ac` | Programs lane map and requests continuous clock, using the PHY clock-request path when supported. `dsi_host.c:940–950`, `dsi_phy_7nm.c:1307–1319`. | **`unknown`** after reset. M5 replays neither word nor forces LP11/HS. LP command DMA selects LP transmission. This alone establishes no particular clock lane state. | Existing `lane-swap` and second `lanes` word. |
| `CLKOUT_TIMING_CTRL +0x0c4`, `T_CLK_PRE_EXTEND +0x180`, `EOT_PACKET_CTRL +0x0cc` | Host clock pre/post from PHY shared timing, conditional extended pre bit, TX EoT append. `dsi_host.c:913–926`. | **`unknown`** for retention. None is replayed. P81 PHY override sets pre `0x20`, post `0x0f`, extension false. | Existing `clkout`, `t-clk-pre-extend`, `eot`. |
| `HS_TIMER_CTRL +0x0bc`, `LP_TIMER_CTRL +0x0b8`, `TIMEOUT_STATUS +0x0c0`, `MISR_VIDEO_CTRL +0x0a4` | 6G video init sets an HS TX timeout for status reporting and per-frame video MISR control. LP RX/BTA timeout definitions are in the XML. `dsi_host.c:858–870,1622–1632`, `dsi.xml:246–266`. | **`unknown`** for retained timer/MISR settings. No native replay and no fresh MISR signature is established by this capture. | Existing `hs-timer`, `lp-timer`, `timeout`. Added `misr-ctrl` config only. |
| `LANE_STATUS +0x0a8`, `CLK_STATUS +0x120` | Host lane stop-state bits 0–4 and `ULPS_ACTIVE_NOT` bits 8–12. Clock-active bits include byte, escape and video pixel clock. Host PLL-unlocked error is bit 16. `dsi.xml:268–318`, `dsi_host.c:1680–1689`. | **`ours-OK`** for read coverage on both. **`unknown`** for LP11/HS and clock-ready state at the stopped command boundary. `0x1f00` means all five lanes out of ULPS, with no stop-state bits set. | Existing first `lanes` word and `clk-status`. |
| `FIFO_STATUS +0x00c`, `ERR_INT_MASK0 +0x10c`, `ACK_ERR_STATUS +0x068`, `DLN0_PHY_ERR +0x0b4` | IRQ enable policy and underflow/overflow, ACK error, escape/PHY fault latches. Linux clears by explicit writes. `dsi_host.c:928–933,1608–1665`. | **`ours-OK`** for both-link passive coverage. Native also samples ACK/FIFO around completed wake transfers. Zero ACK status gives only the error latch value, and short writes request no BTA. **`unknown`** for panel command receipt. | Existing `fifo`, `err-mask`, `ack-err`, `phy-err`. |

### PHY and PLL register comparison

Offsets are unshifted in the PHY common resource unless marked PLL. The DT uses PHY common `0x200`, lane `0x280` and PLL `0x400` resource lengths on each link. The SM8750 3 nm config uses the 7 nm register family and v7.2 quirks. Linux's master starts and locks PLL0. It configures PHY1's slave dividers, mux and external bit clock, and enables both PHY global clocks. A healthy slave PHY can have its own local lock bit clear. Use a bright control per link and the PHY1 mux state when interpreting it. [SM8750 PHY resources](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/arch/arm64/boot/dts/qcom/sm8750.dtsi#L4236-L4254) · [PHY1 resources](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/arch/arm64/boot/dts/qcom/sm8750.dtsi#L4321-L4339) · [PHY register XML](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi_phy_7nm.xml) · [PLL and PHY implementation](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/phy/dsi_phy_7nm.c#L380-L608)

| Both links' word | Pinned Ubuntu use | Native wake and status | Snapshot |
| --- | --- | --- | --- |
| `CMN_CLK_CFG0 +0x010`, `CMN_CLK_CFG1 +0x014` | Byte/pixel dividers, local/external bit-clock source, DSI mux and global clock enable/select. Programs PHY1 dividers/mux from master, releases D-PHY clock select after resync. `dsi_phy_7nm.c:444–503,525–548,772–799`. | **`unknown`** for retained state. No M5 clock mux/PHY writes. | Existing `clk-cfg0/1`. |
| `CMN_RBUF_CTRL +0x01c` | Toggle each resync FIFO once both HS clock paths are ready. `dsi_manager.c:244–252`, `dsi_phy_7nm.c:468–475`. | **`ours-missing`** for resync toggle. **`unknown`** for retained lockstep of both D-PHY outputs. A matching enable word only shows the programmed value. | Existing `rbuf-ctrl`. |
| `CMN_CTRL_0 +0x024`, `CMN_CTRL_1 +0x028` | CTRL_0 controls lane power, digital top and PLL bias. CTRL_1 is defined in XML without a programmed role in this pinned 7 nm path. PHY digital reset instead pulses `GLBL_DIGTOP_SPARE4` at PLL bring-up after power collapse. `dsi_phy_7nm.c:417–440,506–518,1183–1188,1229–1232,1346–1357`. | **`unknown`** for both retained words. No native restore. | Added `ctrl0/1`. CTRL_0 gates the PLL read. |
| `CMN_CTRL_2 +0x02c`, `CMN_CTRL_3 +0x030`, `CMN_CTRL_5 +0x1b0` | Selects full-rate D-PHY, sets CTRL_3 during global clock enable, and enables the v7.2 master's alternate clock output via CTRL_5. `dsi_phy_7nm.c:498–504,780–798,1214–1237`. | **`unknown`** for retention. No native replay of these words. | Added `ctrl2/3/5`. |
| `CMN_LANE_CFG0/1 +0x034/+0x038`, `CMN_LANE_CTRL0/1 +0x0a0/+0x0a4` | Lane mapping, lane enable, and continuous HS clock request (CTRL1 bits 5/6). `dsi_phy_7nm.c:1203–1205,1229–1232,1307–1319`. | **`unknown`** for retention. Native has no lane re-enable, LP11 transition or HS force write beyond restoring the host `CTRL` lane bits. The pin does not show a distinct LP11/HS re-arm after the post-PHY-sync host reset. | Added `lane-cfg0/1`, `lane-ctrl0/1`. |
| `CMN_TIMING_CTRL_0…13 +0x0b4…+0x0e8` in 4-byte steps | Both PHYs receive `00 27 0a 0a 1b 19 0a 0b 0a 02 04 00 20 0f`. The last two bytes also feed the host's CLKOUT timing. `sm8750-xiaomi-piano.dts:1498–1510,1539–1544`, `dsi_phy_7nm.c:1258–1279`. | **`unknown`** for retention of all 14 words. No M5 programming. Compare native bright and dark to each other first, since boot settings can differ from Linux's. | Added `phy timing 0..13`. |
| `CMN_PHY_STATUS +0x140`, `CMN_LANE_STATUS0/1 +0x148/+0x14c` | PHY_STATUS bit 0 is reference-generator ready. The lane words are raw in this XML. `dsi_phy_7nm.c:1073–1089`, `dsi_phy_7nm.xml:74–76`. | **`ours-OK`** for passive read coverage. **`unknown`** for the actual lanes at reset or under video. | Existing `phy-status`, `lane-status0/1`. |
| `CMN_PLL_CNTRL +0x03c`, `PLL_COMMON_STATUS_ONE` at PLL `+0x1b0` | PLL0 start via common control. Lock is bit 0 of the separate PLL status polled by `dsi_pll_7nm_lock_status`. `dsi_phy_7nm.c:380–393,551–608`. | **`unknown`** for lock on M5R. Host `CLK_STATUS` bit 16 records an error condition and PHY_STATUS is reference ready. Neither supplies the direct local lock bit. | Added `pll-ctrl` and `pll: status … local-lock …` for each link. |

This is a retention audit of the DPU route, host command/video state and bonded PHY clock/lane path. The probe does not sample the cold-start analog calibration and individual lane trims, PPS bytes in the panel, or the on-wire DSI signal. The pinned post-sync reset restores `CTRL` and `CLK_CTRL` without re-running PHY setup or rearming LP11/HS after reset. M5 likewise has no such re-arm, but enters CMD-only and relies on retained PHY state from firmware. The sources establish neither a missing re-arm requirement nor a host-reset register wipe on the dark run. [SM8750 reset helper](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L2591-L2635)

## Read-only M5 snapshot and capture diagnostics

`piano-dpu-snapshot=1` enables the extended root-only `machdep.piano_dpu_snapshot` output. The CTL/INTF and DSI compression lines still expose the M5R route. Each extended link read rechecks the existing gate for GCC display AHB, a powered and non-collapsing MDSS domain, enabled AHB and MDP clocks, and the link's enabled, running byte and pixel clocks. The PLL read also requires a readable `CMN_CTRL_0` with both digital-top and PLL-bias bits set. A skipped link or an unreadable PLL is reported separately from a valid zero. Other unmapped words return `ffffffff` and the snapshot reports page-map failures. The opt-in makes zero MMIO stores. Combining it with `piano-dpu-clkread=1` permits passive clock reads and suppresses the RPMh ACTIVE transport. [Snapshot read list and format](../../package/PianoDpu/DpuSnapshot.h) · [Sysctl gating and caching](../../package/PianoDpu/DpuSnapshot.cpp#L207-L279) · [Clock readback](dpu-clkread.md)

With both `native-gop-display-shot=1` and `piano-dpu-snapshot=1`, a refused
`gop-shot` attempt logs why it returned `EBUSY`. The reasons cover a copy
already in flight, an unavailable initial or final DPU lock, a changed
scanout target or generation, and a copy epoch that changed during capture.
The log includes the timing, completed pixel bytes and the DPU state samples
available from successful try-locks. Host tests verify exact and distinct per-link
register loads, skipped reads with closed gates or absent PLL power, local lock
decoding, bounded output, zero stores and detection of a lost compression word
with both INTFs still running. [Snapshot host tests](../../package/PianoDpu/tests/dpu_snapshot_host.cpp) ·
[Capture diagnostics](../../package/NativeGopDisplay/GopShotDiagnostics.h)

### Golden display-sleep baseline on v131y12u-ram2

The November 27 RAM boot used `piano-dpu-panel=0` and `piano-dpu-snapshot=1` at 60 Hz. Six snapshots
(`pre1/2`, `off1/2`, `post1/2`, serials 1–6) each had an open gate, 254 loads and **0 stores**. The C920
measured YAVG 120.91 on the bright desktop, 9.75 after `pmset displaysleepnow` logged display off, and 116.47
after `piano_input_sim=150` restored the display with both KTZ8866 ICs at `1498/1498`.
[RAM capture results](../results-2026-11-27.md#m5-read-only-dsi-pllphy-baseline)

PLL0 stayed locked (`dsi0 pll: status 00000051 local-lock 1`) with DSI1's local lock clear
(`dsi1 pll: status 00000022 local-lock 0`). The bonded links had identical PHY timing words 0–13,
`video-compression 06400b11` and `cmd-compression 39003900/00000000`. Passive readback showed `mdp` 575 MHz
and `pll0` 1725 MHz. Both INTF counters advanced (`65789 -> 70649`) across the capture sequence.

This first hardware validation of `grade_m5_dsi.py` established that live SM8750 `lp-timer=ffffffff` is valid.
Commit `353b397e6` added the register-specific exception. The active-video and vertical front porch (VFP)
samples (`line 2231/2310`) showed normal scan-phase changes in the live host words.
[Snapshot grader](../../tooling/native/m5-dsi-diff/grade_m5_dsi.py)

| Live word | Active video | VFP |
| --- | --- | --- |
| `lane-status` | `0x1f00` | `0x1f1f` |
| `clk-status` | `0x008067c3` | `0x00006343` |
| `fifo` | `0x00001010` | `0x11111010` |

The grader returned byte-exact **`PASS`** for the register words in the active-video pairs `pre1` → `pre2`,
`pre1` → `off1` and the active-video post-wake comparison. The current grader reports each INTF's `line`,
`vtotal` and phase under each snapshot's `timing` field. It uses line 2136 as the active-to-blanking boundary
for this panel's saved counters. A snapshot with disagreeing INTF phases reports `mixed`. Out-of-range
positions report `unknown`.

`changes` retains every register difference and any difference still produces **`DRIFT`** (exit 1).
`transient_changes` identifies the table's exact DSI value pairs when that link's INTF crossed phases.
It also identifies `ctl.flush`, `ctl.top-intr-en` and `ctl.top-intr-status` on the same CTL when both links
crossed phases together. `static_changes` contains the remaining differences, including unexplained
live status values. The `phy-drift` flag uses unexplained PHY changes.
Compare same-phase repeats to check unexplained live register differences. The snapshots are sequential
MMIO reads. Native panel sleep was disabled for this display-sleep reference. The dark-wake observation
with `piano-dpu-panel=1` is analyzed [below](#v131y12u-m5obs2-register-divergence).

The `v131y12u-m5obs1` pre-observation confirmed the bright control (`gop-shot` `mean=224.0`,
`grade_m5_dsi.py PASS`). With standalone `piano-dpu-panel=1`, `DpuSlices::readSliceArgs()` chose the
`piano-dpu-ahb-vote` default before parsing the panel flag, so startup exited at the GCC display AHB clock gate.
Commit `2516bd2f` confines the fix to the argument order and AHB vote default in slice startup.
The wake capture needs a `caffeinate -u` user-activity assertion because macOS re-blanked after 10 s.
The subsequent `v131y12u-m5obs2` capture reached M5 and reproduced the dark wake; its remaining evidence gap is identified below.
[M5 RAM pre-observation](../results-2026-11-27.md#m5-read-only-pre-observation)

### Read-only RAM check plan

1. In an attended RAM qualification, use the existing opt-in M5 path with `piano-dpu-panel=1`, `piano-dpu-snapshot=1` and the existing `native-gop-display-shot=1` capture diagnostics. Keep the panel variant, mode, known bright static pattern, brightness policy and optical exposure fixed across the comparison. Record the mode and panel ID from the snapshot. First take a bright optical sample with a successful active-scanout capture as a positive control.
2. Collect `sudo sysctl -n machdep.piano_dpu_snapshot` before sleep, during stable M5 sleep, at the first available post-wake sample and again while the panel remains optically dark. Take two or more fresh reads in each stable phase, at least 100 ms apart, and confirm the serial and timestamp advance. Save the existing M5 transition log with both links' DMA_DONE, ACK/FIFO and timing boundaries alongside the sysctl samples. The sysctl is a series of MMIO loads and can return a cached sample within 100 ms. Use the transition log for the intra-wake order. The sysctl alone cannot bracket a 1 µs reset or reconstruct transient changes between samples.
3. Diff each link against its own bright sample. Compare stable `CTRL`, clock, trigger, timing, lane-map, compression, PHY and PLL settings word for word. Compare live counters, busy/stop-state bits, IRQ/error latches and FIFO occupancy in the context of scan phase and repeated samples. Require the common and link gates to be open when grading a stable bright/dark pair. Preserve closed gates, unexpected all-ones reads and PLL `unread` as unavailable evidence. The live SM8750 `lp-timer=ffffffff` is valid. On a PHY1 external-clock configuration, grade PLL0 lock and PHY1 mux/global clock plus lane and FIFO evidence together. A change isolated to PHY1's local lock bit is insufficient to call the bonded clock lost.
4. Pair the dark optical exposure with a successful, stable active-scanout capture of the same pattern. If capture returns `EBUSY`, retain its reason and retry at the same static workload while the same state persists. Record buffer generation and timing from the existing diagnostics. With a confirmed bright pixel source, a reproducible host or PHY change identifies a register to correlate with the transition. A matched bright source plus matching sampled host/PHY state still requires panel or link evidence, such as externally observed HS traffic or an independently qualified panel status measurement. Missing source pixels leave the outcome unclassified.

## v131y12u-m5obs2 register divergence

This is the historical, serial-3-contaminated capture. The [clean m5obs3 analysis](#v131y12u-m5obs3-serial-2-wake-decode)
below supplies its requested serial-2 report and supersedes its next-observation requirement.

This analysis uses the supplied November 27 RAM transcript for commit `411825dd`, BOE `35-02-0b`,
`piano-dpu-panel=1 piano-dpu-ahb-vote=1 piano-dpu-snapshot=1`, 60 Hz macOS presentation and 120 Hz live scanout.
The run returned `rc=0`, logged in at +49 s, retained its WindowServer PID, and recorded zero crashes,
GPU faults or panics. C920 health was `ok` across 21,179 frames at 29.999 fps. Startup reached
`DpuSlices serving with panel on`, the active panel flag and verified moving INTF counters after `2516bd2f`.
The extended snapshots report 254 loads, zero stores and an open gate; observation did not clear the error latches.
This closes the m5obs1 startup blocker, not the dark wake. [Run result](../results-2026-11-27.md#v131y12u-m5obs2-2026-11-27)

Native line references in this section describe the files at `411825dd`; the audited files match this checkout.
Linux citations remain pinned to `83f0f89bed14`. The local Linux checkout's HEAD is newer; a path-limited
comparison against that pin found no differences in the host/manager/config, panel, 7 nm PHY or two XML files
used here. No inference below relies on the newer HEAD or on a register name from a different DSI generation.

### Path and capture chronology

Path: IOMFB's combined system/API display-power gate → NativeGopDisplay's serialized copy worker → confirmed
dual backlight blank and touch park → `PianoDpuPanel` → `panelOff` (LP `28`, 20 ms, LP `10`, 100 ms,
master stop, CTL reset/slave route submission, slave stop, fetch off) → retained-power sleep → `panelOn`
(quiescent hosts, host reset, CMD-only `11`, 120 ms, `29`, 10 ms, saved host mode, fetch/route/INTF restart)
→ PCC replay, touch release and display/backlight publication. The later serial-3 off fails before either
timing engine is stopped. [IOMFB gate](../../package/NativeGopDisplay/NativeGopDisplay.cpp#L1585-L1623) ·
[Worker and callout](../../package/NativeGopDisplay/NativeGopDisplay.cpp#L1739-L1791) ·
[Worker serialization](../../package/NativeGopDisplay/NativeGopDisplay.cpp#L2356-L2379) ·
[DPU sleep/wake ownership](../../package/PianoDpu/DpuSlices.cpp#L807-L864) ·
[Panel transaction](../../package/PianoDpu/DpuPanel.h#L562-L725)

| Ordered evidence | What it establishes |
| --- | --- |
| Bright `m5-bright1`: YAVG 102.13, panel `awake` | Positive optical control. CTL0 ACTIVE/MASTER `6/2`, both INTFs `EN/STATUS=1/1`. Bright active FIFO `1010/1010`. |
| Serial 1 off: `ok=1 step=5 restored=0 rows=135 dropped=0` | Both `28` and `10` trigger and finish on both links in 1,268 µs each, `porch=1`, INTR `00010001/00010001`, ACK masks `3→3` with zero errors. The first command changes FIFO `11111010→1010`; the second retains `1010`. Host completion does not prove panel receipt. |
| Serial 1 rows 122, 133, 134 | `stop2` precedes `fetch-off` and `end`. Both final EN/STATUS pairs are zero; CTL TOP/ACTIVE `f0000000/4`, MASTER 0. The earlier stop-order fault is absent. |
| `m5-asleep1/2`, before any wake reset | YAVG 8.80, panel `asleep`, both EN/STATUS zero and counters frozen across 3.9 s. **DSI0 already has FIFO `99991090`, STATUS0 `2005`, CLK_STATUS `8077c3`; DSI1 has `11111010`, `2000`, `6343`. Both RESET words are still `1c00`.** |
| Serial 2 wake | Software success and restored scanout are reported. Its full per-command register report is not in the supplied extracts. The next serial's saved FIFO values are already `99991090/99991090` before its DSI FIFO setup. This brackets the DSI1 change between sleep and serial-3 entry; it does not locate it within serial 2. |
| Serial 3 off, before `m5-wake1` | `ok=0 step=1 restored=0 rows=82 dropped=0`. DCS `28` triggers, `done=0`, 50,683 µs, INTR `00010000/00010000`. Rows 0→80 still have both INTFs running, ACTIVE `6`, clear flushes and advancing counters. This is a command timeout, not another failed timing stop. |
| `m5-wake1/2/3` after serial 3 | YAVG 20.24/20.26 despite an awake assertion, both KTZ8866 ICs lit (`1496/1496`, enables `127/127`) and display power Yes. Panel property `fault` follows serial 3. The DPU route/timing, static host setup and all sampled PHY/PLL words equal bright; both INTFs keep counting at 120 Hz (frame samples 8806→9297). All six source captures return `EBUSY: scanout kept changing`. |

The last optical/register samples are **post-timeout**, not an uncontaminated observation immediately after
serial 2. They prove a dark display and a faulted host command path at that time. The supplied optical
ordering does not independently establish the panel's pixels before serial 3's `28`. Source-buffer contents
remain unverified because every dark `gop-shot` refused capture.

### Offset audit, including the 6G shift

Linux sets `DSI_6G_REG_SHIFT=4` (`dsi_cfg.h:39`), adds `cfg->io_offset` to `ctrl_base`
(`dsi_host.c:2067–2068`), and reads/writes `ctrl_base + reg` (`193–201`). Native `dsiReg` adds its
already-shifted offset directly to physical DSI0/1 bases `0x0ae94000/0x0ae96000`.
**Do not add another four bytes.** `6G_HW_VERSION` is the special unshifted word at physical base+0.
The host's 6G v2.x version handling and the SM8750 v7.2 force-clock implementation do not supply an
alternative RESET field layout. [Shift constant](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_cfg.h#L39) ·
[Linux base adjustment](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L2067-L2068) ·
[Native addresses](../../package/PianoDpu/DpuHw.h#L20-L25) · [Native address helper](../../package/PianoDpu/DpuSlices.h#L49-L59)

The following inventory covers every DSI host offset used or declared by `DpuHw.h:157–170`, the panel's
imported `DpuSlices.h:51–59` constants, and `DpuSnapshot.h:29–44,173–196`. Corresponding entries on each
row are in the same order. All XML-defined offsets match; the raw extensions remain explicitly unverified.
The DPU/CTL/INTF, DISPCC and SMMU registers in those files belong to other register domains, not `dsi.xml`.

| Physical host offset | Pinned XML offset and name | Exact XML lines |
| --- | --- | --- |
| `000`; `004`, `008`, `00c` | `000 6G_HW_VERSION` (unshifted); `000 CTRL`, `004 STATUS0`, `008 FIFO_STATUS` | 90–144 |
| `010`, `020` | `00c VID_CFG0`, `01c VID_CFG1` | 145–163 |
| `024`, `028`, `02c`, `030`, `034`, `038` | `020 ACTIVE_H`, `024 ACTIVE_V`, `028 TOTAL`, `02c ACTIVE_HSYNC`, `030 ACTIVE_VSYNC_HPOS`, `034 ACTIVE_VSYNC_VPOS` | 164–187 |
| `03c`, `040`, `044`, `04c` | `038 CMD_DMA_CTRL`, `03c CMD_CFG0`, `040 CMD_CFG1`, `048 DMA_LEN` | 189–208 |
| `068`, `084`, `090` | `064 ACK_ERR_STATUS`, `080 TRIG_CTRL`, `08c TRIG_DMA` | 227–238 |
| `0a4`, `0a8`, `0ac`, `0b0` | `0a0 MISR_VIDEO_CTRL`, `0a4 LANE_STATUS`, `0a8 LANE_CTRL`, `0ac LANE_SWAP_CTRL` | 264–287 |
| `0b4`, `0b8`, `0bc`, `0c0`, `0c4`, `0cc` | `0b0 DLN0_PHY_ERR`, `0b4 LP_TIMER_CTRL`, `0b8 HS_TIMER_CTRL`, `0bc TIMEOUT_STATUS`, `0c0 CLKOUT_TIMING_CTRL`, `0c8 EOT_PACKET_CTRL` | 239–263 |
| `10c`, `110`, **`118`**, `11c`, `120` | `108 ERR_INT_MASK0`, `10c INTR_CTRL`, **`114 RESET`**, `118 CLK_CTRL`, `11c CLK_STATUS` | 288–318 |
| `15c`, `180`, `1f4` | `158 TEST_PATTERN_GEN_CTRL`, `17c T_CLK_PRE_EXTEND`, `1f0 VERSION` | 330–349,370–372 |
| `2a0`, `2a8`, `2ac` | `29c VIDEO_COMPRESSION_MODE_CTRL`, `2a4 COMMAND_COMPRESSION_MODE_CTRL`, `2a8 COMMAND_COMPRESSION_MODE_CTRL2` | 374–394 |
| `17c`, `390` | Would require XML `178`, `38c`: neither exists. Native FIFO payload port and second DMA control word come from the separate PeripheralIO path, not this XML. | `DpuSlices.h:53–59`; `DpuPanel.h:187–191` |
| `1e4`, `1e8`, `1ec` | Would require XML `1e0`, `1e4`, `1e8`: none exists. Native raw timing-shadow words; the completed-DCS cleanup pulses `1ec`. Its designation as a FIFO reset is **not established by this pin**. | `DpuHw.h:167–170`; `DpuPanel.h:123–137` |

Source: [pinned host XML](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L90-L394) ·
[Native host constants](../../package/PianoDpu/DpuHw.h#L157-L170) ·
[Snapshot offsets and loads](../../package/PianoDpu/DpuSnapshot.h#L29-L67).
For disambiguation, `RDBK_DATA[0..3]` are XML `068/06c/070/074`, physical `06c/070/074/078`
(`dsi.xml:228–230`); `RDBK_DATA3` is **not** physical `118`. PHY reset is XML `128`, physical `12c`
(`319–321`), separate from controller RESET. There is no separately named `DSI_SOFT_RESET` register in this
XML: `dsi_sw_reset()` drives `REG_DSI_RESET` (`dsi_host.c:1151–1178`).

Every PHY/PLL offset in `DpuSnapshot.h:45–54` also matches its **unshifted, separate resource**:
common `010/014/01c` = `CLK_CFG0/1/RBUF_CTRL`; `024/028/02c/030/1b0` = `CTRL_0/1/2/3/5`;
`034/038/03c` = `LANE_CFG0/1/PLL_CNTRL`; `0a0/0a4` = `LANE_CTRL0/1`;
`0b4…0e8` in four-byte steps = `TIMING_CTRL_0…13`; `140/148/14c` = `PHY_STATUS/LANE_STATUS0/1`.
PLL resource `1b0` is `COMMON_STATUS_ONE`, not common `CTRL_5`.
These are `dsi_phy_7nm.xml:12–61,74–76,201`, not host offsets plus four.
[PHY/PLL XML](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi_phy_7nm.xml#L12-L76)

### Bit decode of all five divergent registers

The tables account for every set bit, every XML-defined clear field and the remaining undefined bits.
“Undefined” means the pinned sources give no semantics; it does not mean that hardware ignores the bit.

**RESET, physical `+0x118`: `00001c00 → 00000000`.** XML line 291 declares a raw 32-bit `RESET`
with **no fields**. In the bright and pre-wake asleep samples, bits **10, 11 and 12** are each 1;
all other bits, including bit 0, are 0. Afterwards all 32 bits read 0. Linux's literal `1→0` sequence
establishes bit 0's software reset use; it gives no name, retention contract or writable-mask information
for bits 10–12. Both the generic reset (`dsi_host.c:1170–1172`, 20 ms) and the SM8750 post-sync reset
(`2625–2628`, 1 µs) write the whole word without preserving `1c00`.
[RESET declaration](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L291) ·
[SM8750 reset](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L2591-L2635)

Native likewise writes literal 1, waits 1 µs, writes 0, and requires a zero readback
(`DpuPanel.h:511–515`); its guard permits only 0/1 (`DpuSlices.h:90`). Thus the observed loss on both
links is consistent with a write both executed during wake. Neither serial-1 sleep nor snapshots write
RESET, explaining why the inherited bright value remains during sleep. The agent that originally set
`1c00` and the silicon meaning of its three bits are **unknown**. Their loss is not proof of erased video
configuration or a requirement to restore them. No pinned source supports calling them FIFO reset enables.

**FIFO_STATUS, physical `+0x00c`.** The full field mapping is `dsi.xml:118–144`.
[FIFO fields](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L118-L144)

| Bit(s), XML meaning | `00001010` bright active | `11111010` bright VFP / DSI1 asleep | `99991090` DSI0 asleep / both pre-serial-3 | `88881090` post-timeout |
| --- | --- | --- | --- | --- |
| 0 VIDEO_MDP overflow; 3 VIDEO_MDP underflow | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| **4 undefined** | 1 | 1 | 1 | 1 |
| **7 CMD_MDP underflow** | 0 | 0 | **1** | **1** |
| 8/9 CMD_DMA read/write watermark; 10 CMD_DMA underflow | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| 12 lane-0 LP empty; 13 LP full; 14 LP overflow | 1 / 0 / 0 | 1 / 0 / 0 | 1 / 0 / 0 | 1 / 0 / 0 |
| 16/20/24/28 lanes 0/1/2/3 HS empty | all 0 | all 1 | all 1 | all 0 |
| 17/21/25/29 lanes 0/1/2/3 HS full | all 0 | all 0 | all 0 | all 0 |
| 18/22/26/30 lanes 0/1/2/3 HS overflow | all 0 | all 0 | all 0 | all 0 |
| **19/23/27/31 lanes 0/1/2/3 HS underflow** | all 0 | all 0 | **all 1** | **all 1** |
| 1/2/5/6/11/15 undefined | all 0 | all 0 | all 0 | all 0 |

Arithmetic checks: `11111010 = 1010 | 11110000`; `99991090 = 11111010 | 88880080`;
`88881090 = 99991090 & ~11110000`. The last transition removes the four **empty** indications,
not the four HS-underflow flags or CMD_MDP underflow. Neither fault word asserts the XML's VIDEO_MDP
underflow bit 3 or CMD_DMA underflow bit 10. Calling it simply a video-input FIFO underflow loses that distinction.
Linux writes FIFO status back on its error path (`dsi_host.c:1650–1664`) and specifically queues
`dsi_sw_reset` for CMD_MDP underflow (`1591–1599`). These are error indications, not just VFP occupancy.
They can describe a prior event; a passive snapshot does not timestamp the event or prove continuous starvation.
[Linux FIFO error handling](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L1591-L1664)

**STATUS0, physical `+0x008`.** XML lines 109–116 define only busy bits 0–4 and contention bit 31.
[STATUS0 fields](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L109-L116)

| Bit, meaning | Bright DSI0 `2008` | Bright DSI1 `8` | Asleep DSI0 `2005` | Asleep DSI1 `2000` | Both post-timeout `8000a00f` |
| --- | --- | --- | --- | --- | --- |
| 0 CMD engine busy | 0 | 0 | **1** | 0 | **1** |
| 1 CMD DMA busy | 0 | 0 | 0 | 0 | **1** |
| 2 CMD MDP busy | 0 | 0 | **1** | 0 | **1** |
| 3 VIDEO engine busy | 1 | 1 | 0 | 0 | **1** |
| 4 DSI_BUSY | 0 | 0 | 0 | 0 | 0 |
| 13 undefined | 1 | 0 | 1 | 1 | 1 |
| 15 undefined | 0 | 0 | 0 | 0 | **1** |
| 31 INTERLEAVE_OP_CONTENTION | 0 | 0 | 0 | 0 | **1** |
| 5–12,14,16–30 undefined | all 0 | all 0 | all 0 | all 0 | all 0 |

`8000a00f = BIT(31) | BIT(15) | BIT(13) | 0xf`: the documented low bits are all four engine-busy
flags, not DSI_BUSY. Linux treats bit 31 as an error and writes it back (`dsi_host.c:1667–1677`).
Neither XML nor the audited host's 6G/v7.2 paths defines STATUS0 bits 13 or 15. The asleep sample proves
DSI0's command engine/MDP were busy while video-busy and command-DMA-busy were clear; it does **not** show
an outstanding command DMA then. The post-timeout sample shows a blocked command DMA alongside video
busy and recorded contention. The exact time contention first asserted is unmeasured.

**CLK_STATUS, physical `+0x120`.** XML lines 301–318 give the following complete decode.
[Clock fields](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L301-L318)

| Bits and fields | `008067c3` bright active | `008077c3` DSI0 asleep / both post-timeout | `00006343` DSI1 asleep |
| --- | --- | --- | --- |
| 0 AON AHBM, 1 DYN AHBM | 1 / 1 | 1 / 1 | 1 / 1 |
| 2 AON AHBS, 3 DYN AHBS, 4 AON DSI, 5 DYN DSI | all 0 | all 0 | all 0 |
| 6 AON BYTE, 7 DYN BYTE | 1 / 1 | 1 / 1 | 1 / 0 |
| 8 AON ESC, 9 AON pixel, 10 DYN pixel | 1 / 1 / 1 | 1 / 1 / 1 | 1 / 1 / 0 |
| **12 DSI_DYN_CMD_PCLK_ACTIVE** | **0** | **1** | 0 |
| 13 DSI_CMD_PCLK_ACTIVE, 14 DSI_VID_PCLK_ACTIVE | 1 / 1 | 1 / 1 | 1 / 1 |
| 15 CAM_BIST pixel, 16 PLL_UNLOCKED | 0 / 0 | 0 / 0 | 0 / 0 |
| 23 undefined | 1 | 1 | 0 |
| 11,17–22,24–31 undefined | all 0 | all 0 | all 0 |

The active bright→dark XOR is exactly `00001000`, **dynamic command pixel clock active**, not PLL
unlock, missing byte clock, ULPS or clock failure. Its presence on asleep DSI0 is consistent with its
busy command/MDP engines, but does not explain why they became busy. PLL_UNLOCKED is bit 16 and clear;
the independent PLL0 lock remains 1. PLL1's local lock 0 is already present in the bright control and
fits the external-clock bonded role; it is not a new dark-wake failure.

**TEST_PATTERN_GEN_CTRL, physical `+0x15c`: `0 → 00030006`.** XML lines 71–76,330–337 decode
bit 1 `CMD_DMA_TPG_EN=1`, bit 2 `TPG_DMA_FIFO_MODE=1`, and bits 17:16
`CMD_DMA_PATTERN_SEL=3` (custom pattern / DMA FIFO). Bit 0 `EN=0`, VIDEO_PATTERN_SEL bits 5:4=0,
CMD_MDP_STREAM0_PATTERN_SEL bits 9:8=0; every other bit (3,6–7,10–15,18–31) is zero.
The two zero pattern selectors mean PRBS (`dsi.xml:59–69`), with their generation disabled.
This is the command FIFO route, not enabled video test-pattern generation.
[TPG fields](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L330-L337)

`sendDcs` writes this exact value to both links before the trigger (`DpuPanel.h:183–191`), then skips
restoration for a link without DMA_DONE (`227–232`). Serial 3 has no completed links, so the observed
TPG residue follows deterministically. It cannot explain FIFO `99991090/99991090` sampled **before**
that setup. The separate sixth difference, DSI1 DMA_LEN `84→4` (132→4 bytes), is also compatible with
serial 3 writing four bytes and skipping restoration; its pre-serial-3 value is not supplied. It is not a
changed pixel timing or DSC slice size (`dsi.xml:208`; `DpuPanel.h:89,191,228`).

**The serial-3 printed trailing zeros are not hardware evidence.** ACK mask `3:0/0 → 0:0/0` means
both before reads exist and neither after read exists. `Sent` initializes `fifoAfter` and `ackAfter` to
zero (`DpuPanel.h:112`); the timeout skips the only read sites (`228–232`). Thus its printed
`fifo=99991090/99991090>00000000/00000000` does not show FIFO clearing. The formatter emits the fields
regardless (`DpuSlices.cpp:780–786`). [Transfer and cleanup](../../package/PianoDpu/DpuPanel.h#L103-L241) ·
[Trace formatting](../../package/PianoDpu/DpuSlices.cpp#L775-L792)

### Wake comparison and check matrix

The Linux sequence is a full power cycle, while M5 retains power. Missing full-cycle operations are
candidate differences, not automatically requirements for light sleep. The SM8750 post-sync controller
reset is distinct from the generic 20 ms reset and the PHY resync FIFO toggle.

| Native operation and exact lines | Pinned Linux counterpart and exact lines | Check and implication |
| --- | --- | --- |
| `panelOn:668–677`: require saved bonded path, CTRL match, readable clocks, TPG free and CMD_DMA not busy | `dsi_manager.c:224–270`: power/program hosts and both PHYs before IRQ/commands | **ours-OK** for admission and retained configuration reads. **ours-missing** for checking CMD engine/MDP busy, contention and FIFO-underflow flags. Asleep DSI0's `2005/99991090` is admitted because DMA bit 1 is clear. This is a proven diagnostic/readiness gap, not proof that rejecting it would repair the panel. |
| `resetDsiHosts:499–508`: prove both INTFs disabled and quiescent; clear CTRL bits 0–2 on both; force saved clocks | `dsi_host.c:2614–2624`: clear identical CTRL bits, force saved CLK_CTRL with `23f | BIT(8) | BIT(9) | BIT(11) | BIT(21)` | **ours-OK** for address, force mask and disable-before-clock ordering. Native mask `200b3f` includes bit 9 via `23f` (`DpuSlices.h:57`). With saved `23f`, CTRL goes `1f7→1f0`, clocks `23f→200b3f`. Each native store has `dsb sy` (`DpuSlices.cpp:199,295–308`), so Linux's explicit barriers are not an omitted native ordering step. |
| `resetDsiHosts:509–523`: DSI0 then DSI1 RESET `1→1 µs→0`, require RESET zero and DMA/video idle, restore clocks | `dsi_host.c:2625–2633`: identical pulse and clock restore, then restore CTRL | **ours-OK** for SM8750 post-sync reset sequence. Native defers CTRL restore to CMD-only at `527–535`. A missing separately addressed SOFT_RESET is ruled out. Generic `dsi_sw_reset:1151–1178` is a different 20 ms helper; longer reset is not established as needed here. **unknown** whether the pulse clears earlier error latches or internal FIFO state; native tests only STATUS bits 1/3, not 0/2/31 or FIFO. |
| `setDsiMode:527–535`, called at `680`: restore enable/lanes/clock with VIDEO clear and CMD set (`1f5`) | `dsi_manager.c:323–335`, `dsi_host.c:1181–1203,2473–2489`: enable host video before prepare; transfer ORs ENABLE/CMD at `2222–2227` | **ours-OK** for both hosts configured before either command. **unknown** for behavioral equivalence of native CMD-only versus Linux's enabled, idle video host with CMD overlaid. No measured mismatch of final CTRL (`1f7`) closes a transient mode error. |
| `panelOn:689–699`, `sendDcs:152–232`: paired LP FIFO `11`, 120 ms, `29`, 10 ms, no porch wait in CMD-only | `dsi_manager.c:523–583`, `dsi_host.c:1220–1259,1464–1495,2400–2410`: prepare both, memory-backed DMA, DSI0→DSI1 triggers, VIDEO_DONE wait when busy; P81 panel `652–654` sends `11→120 ms→29` | **ours-OK** for command order, wait, paired trigger order and software success as reported. Full serial-2 DMA/ACK/FIFO fields are **unknown** in these extracts. CMD_DMA_DONE is host completion, not a panel ACK. Native's raw `17c/390/1ec` transport extensions are outside the XML and require independent validation. |
| `panelOn:703–721`: on owned wake-command failure reset stopped hosts, restore transfer words; restore saved CTRL before start | `dsi_manager.c:560–564`, `dsi_host.c:2236–2252`: restore both prepared hosts after transfer returns | **ours-OK** for explicit abort/cleanup on the stopped-host wake path. This code does not run for serial-3 `panelOff` failure at step 1 (`590–594`). Its un-restored TPG/length are a **closed secondary effect**, not the earliest failure. Arbitrary restoration while its DMA owns the FIFO is not a justified fix. |
| `startEngines:371–472`, called at `715`: reset stopped CTL, restore fetch, submit saved route, latch flush, start pair and measure lockstep | `dpu_encoder.c:1862–1913,2253–2272`, `dpu_encoder_phys_vid.c:668–683`: configure/flush before timing | **ours-OK** on hardware for ACTIVE/MASTER `6/2`, static CTL/DSC/pipe/fetch/INTF matches and moving lockstep counters. These checks do not prove DSI packet emission or panel decoding. |
| No PHY writes in the M5 path | `dsi_manager.c:244–259`, `dsi_phy_7nm.c:468–488`: both RBUF_CTRL `0→1`, release CLK_EN_SEL, then host reset | **ours-missing** for resync/release during wake. **unknown** whether retained PHYs need them: common RBUF reads 1 and all sampled PHY words equal bright. Unchanged control/lock words cannot prove FIFO phase alignment, and host HS-underflow flags do not identify the common RBUF FIFO. |
| No panel reset/init/PPS replay; only four short DCS allowed (`77–96,148–150`) | P81 `panel-novatek-nt36532.c:473–673,821–852`: vendor init, PWM/CABC, mode, `11/29`, then PPS with BPG 13 and range-0 max QP 8 | **ours-missing** for PPS/vendor/brightness replay. **unknown** whether BOE loses these over retained-power sleep. Matching `VIDEO_COMPRESSION=06400b11` and DPU DSC registers cannot read panel PPS. PPS loss could explain dark pixels but no pinned source ties it deterministically to CMD_MDP/HS FIFO underflow or contention. |
| No Linux-style DSI error IRQ worker/status clearing in this M5 transaction | `dsi_host.c:1591–1599,1650–1689`: FIFO writeback, CMD_MDP-underflow reset, contention writeback, PLL-unlock check | **ours-missing** for equivalent fault handling. **unknown** whether error clearing/reset would restore useful traffic or merely discard evidence. DSI0 underflow before reset with `RESET=1c00` disproves treating loss of that word as the origin of every observed error. |

Sources: [native reset/mode](../../package/PianoDpu/DpuPanel.h#L496-L535) ·
[native wake](../../package/PianoDpu/DpuPanel.h#L655-L725) ·
[Linux host reset](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L2591-L2635) ·
[Linux manager prepare](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_manager.c#L212-L335) ·
[PHY resync](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/phy/dsi_phy_7nm.c#L468-L490) ·
[P81 wake and PPS](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/panel/panel-novatek-nt36532.c#L652-L673).

### Root cause closure and the minimal remaining observation

**Closed:** the register-address ambiguity; the historical DPU stop-order failure; literal reset writes as
the software operation consistent with losing `1c00`; serial-3 timeout as the cause of the un-restored
command FIFO route and four-byte length; and the false interpretation of its unread trailing zeros.
**Not closed:** the physical cause of the dark pixels or the first DSI underflow event. The observed host
failure class is CMD_MDP plus all four lane HS underflow flags, followed by an unfinished command DMA and
interleave contention while the DPU continues scanning. That localizes a real host-side error indication;
it does not establish one deterministic repair or exclude a concurrent source/panel problem.

The asymmetry has a measured time ordering, not a proven silicon explanation. Serial 1 drains and stops
the master, resets/submits the slave route and retires the slave's local frame. DSI0 has command/MDP busy,
dynamic command clock and underflow flags after that sequence; DSI1 is idle with empty HS FIFOs.
Linux explicitly warns that INTF disable can leave a DSI video engine blocking commands
(`dsi_host.c:2501–2505`), supporting stale host state as a candidate. Here asleep **video-busy is clear**,
so that comment does not fully explain DSI0's command/MDP busy state. No host samples bracket the individual
stop/handoff stores. The master/slave drain histories differ, but attribution of DSI0's exact onset to a
specific store is **unknown**. [Linux disable rationale](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L2493-L2507)

Both hosts then undergo the same wake reset, command window and video restart; by serial-3 entry both
show underflow. Whether DSI0's indication survived reset or reasserted, and whether DSI1 first faulted
during reset, `11/29`, FIFO cleanup or video restart, cannot be read from the supplied samples. The reset
bits' meanings, PHY phase, emitted traffic and panel PPS/brightness state remain unknown. A shared final
fault word cannot distinguish these alternatives. In particular, neither changing RESET to preserve
`1c00`, adding RBUF toggles, extending reset/Sleep Out, adding a speculative video-FIFO reset nor replaying
PPS is justified as **the** fix by this capture.

The minimal next read-only check is to recover **serial 2's existing report from the saved console**:
its `pre-reset status`, both `dcs 11`/`dcs 29` records (trig/done/porch/INTR, ACK masks, before/after FIFO),
and `host 11`/`host 29` CTRL/STATUS/lane/clock records. The existing printer emits these before each indexed
trace (`DpuSlices.cpp:775–792`). `before` is collected before FIFO setup; `after` is read only after a
completed link's restore, including the `1ec` pulse. Compare **each link** before reset → first command
entry after reset → first command cleanup → second command entry/cleanup → first video snapshot.
This can locate the DSI1 fault relative to the command window; it still cannot separate reset from the
mode write, or DMA completion from cleanup, without an intervening sample.

If serial 2 was overwritten in the property and absent from the console, repeat an **observation-only**
qualification with the same driver image and hold user activity **before waking** to avoid the serial-3
auto-off. Save the wake property immediately and take fresh, open-gate repeated snapshots plus optical
samples before any further transition. Match active scan phases, retain snapshot serial/timestamps, and
obtain a successful stable bright-pattern source capture with the existing diagnostics. This is not a
proposal to clear status, poke RESET/RBUF, send new DCS or edit the driver. If the available records still
cannot bracket the onset, report that limitation and keep HOLD; a second snapshot alone cannot invent
missing intra-reset evidence. A definition for RESET bits 10–12 would require a separately qualified
v7.2 register specification, not a guessed decode from the raw XML declaration.

## v131y12u-m5obs3 serial-2 wake decode

**Clean dark wake is confirmed independently of serial 3. The exact host-side repair is not closed.**
The supplied November 27 RAM evidence reuses frozen image SHA-16 `216a01734436b7c4` and reports
**120 PASS / 0 FAIL / `rc=0`**, login **+54 s**, WindowServer PID **102** unchanged, zero crashes,
GPU faults or panics, and C920 **7,756 frames at 29.996 fps**, `health=ok`, zero transient frames.
The harness result records successful observation; it does not grade the optical wake as repaired.
`caffeinate -u -d -i -t 300` holds the wake; **no serial 3 occurs**. The serial-2 report has 63 rows,
zero dropped, `ok=1 step=3 restored=0`. This closes the missing-report requirement from `a73b7adc3`.
[Run summary](../results-2026-11-27.md#v131y12u-m5obs3-2026-11-27)

Linux references here are to **`83f0f89bed14`**. A path-limited comparison of all six requested Linux
files against that commit is empty despite the local checkout's newer HEAD. Native references are to
this checkout's `DpuPanel.h`, `DpuHw.h` and `DpuSlices.cpp`; no driver files were changed for this analysis.
Physical DSI host offsets already include the Linux **+4** shift, as established in the
[offset audit](#offset-audit-including-the-6g-shift).

### Optical controls and transition chronology

| Phase / evidence | Result and boundary established |
| --- | --- |
| M5A, bright `224,224,224`, `CS_READY_S=14` | YAVG **112.984**; `gop-shot source=flip mean=224.0 nonblack=100.0%`, 98,562 B. Panel `awake`, display power Yes, MDP 575 MHz, PLL0 read `ok`. Positive optical and source controls. |
| Serial 1 off, 122 rows, zero dropped | `ok=1 step=5 restored=0`; DCS `28/10` complete on both hosts in **1,115/1,134 µs**, both `porch=1`, INTR `00010001/00010001`, ACK masks `3→3`, all sampled ACK errors zero. FIFO `11111010/11111010→00001010/00001010` on `28`, unchanged on `10`. Both engines stop in 6,319/8,569 µs, STATUS `0/0`; console step times `6/37/143/149 ms`. |
| M5B, real retained-power sleep | YAVG **9.524**, panel `asleep`, display power No. Both INTF EN/STATUS `0/0`; counters frozen for **3,902,305 µs**, INTF1 `110922:1`, INTF2 `1:2310`. The stop-order repair works. |
| Serial 2, before reset | STATUS0 **`00002005/00002000`**. DSI0's FIFO error history predates wake; DSI1 has the clean empty-FIFO state. |
| DCS `11` | `trig=1 done=3 us=5 porch=0`, INTR `00010001/00010001`, zero ACK errors with both before/after masks 3. FIFO **`99991090/11111010→99991090/11111010`**. |
| DCS `29` | Same evidence, **4 µs**, FIFO again **`99991090/11111010→99991090/11111010`**. DSI1 is still FIFO-error-free **after** completed-command cleanup. |
| Host records for `11` and `29` | Both: CTRL **`1f5`**, STATUS **`2000`**, LANE_STATUS **`1f1f`**, CLK_STATUS **`2343`**. These are command-entry reads, not post-command host reads; see the sampling distinction below. |
| M5C, wake1 / wake2 | YAVG **20.555 / 20.924**, dark pixels with the backlight lit; panel **`awake`**, display power **Yes** sustained. `grade_m5_dsi.py PASS`: **+54 frames in lockstep**, zero compared-register changes. Both hosts have STATUS0 **`8000200d`**, FIFO **`88881090`**, CLK_STATUS **`008077c3`**, TPG **0**. |
| Wake source capture | Attempt 1 reached `source=flip format=&BGA layout=8-bit fetch bytes=8369741`, the supplied active lock-screen wallpaper evidence (8.37 MB). USB transfer hit the 45 s cap; this is not a fully received image with a new measured pixel mean. Four `EBUSY reason=odd-epoch` diagnostics occurred during transition. Together with live flips, this rules out treating wake as simply a stopped source; it does not measure panel input packets. |

The supplied indexed rows place reset and restart in this order (µs relative to row 0):

| Row(s) / time | Transition |
| --- | --- |
| 0 / 0; 1 / 195 | `dsi-reset B/P`, both INTFs stopped. These rows contain DPU/CTL fields, not DSI FIFO samples. |
| 2 / 214 | `dcs11 B`. |
| 3 / 122,325 | `dcs29 B`, **122,111 µs** after `dcs11 B`; the code waits 120 ms after `11`. |
| 4 / 133,480; 6 / 133,645 | `ctl-reset B`, then `fetch-on B`, after `29` and its 10 ms wait. Saved CTRL restoration precedes `ctl-reset`. |
| 7–9 / 133,667–133,707 | `route-on B/P/P`, ACTIVE `4→6`; CTL_FLUSH/INTF_FLUSH `80000000/6→0/0`. |
| 10 / 133,726; 11 / 133,749 | `enable B`, then `start P`, both EN/STATUS **1/1**, route active 6 and flushes clear. Console start polls report 6 µs each and lockstep, step times `0/0/122/133 ms`. |

**Sampling limit:** `DpuSlices.cpp:703–715` prints `host 11/29` from `Sent::before`, populated by
`DpuPanel.h:152–175` **before** FIFO setup and trigger. `fifoAfter` is read at `230–232` **after**
`restoreDcsLink`, including its `+1ec` pulse. Thus DSI1's FIFO is proven clean through `29` cleanup,
but STATUS/CLK after `29` cleanup are not sampled. The indexed ring reads only INTF and CTL
(`DpuPanelTrace.h:73–92`), so `dropped=0` does not supply missing host boundaries.
The first host fault sample follows saved-mode restoration **and** video restart. It cannot establish
that either single store immediately caused it.
[Report fields](../../package/PianoDpu/DpuSlices.cpp#L703-L715) ·
[Command samples](../../package/PianoDpu/DpuPanel.h#L152-L175) ·
[FIFO after cleanup](../../package/PianoDpu/DpuPanel.h#L227-L232)

### All nine changed snapshot words and their exact decode

All other compared words in the 254-load snapshots match bright byte for byte: CTL/DSC/pipe/fetch/INTF
timing, host static configuration/compression and all sampled PHY/PLL words. In particular CTRL=`1f7`,
VIDEO_COMPRESSION=`06400b11`, CLK_CTRL=`23f`, lane controls=`1f00/01000000`, TPG=`0/0`,
DMA_LEN=`4/84`, PLL0=`51` with local lock 1. Wake1→wake2 repeats the same fault words over +54 live frames.

| Changed word | Bright → clean wake | Pinned definition / implication |
| --- | --- | --- |
| `ctl.top-intr-status` | `28000300→20000300` | Bit **27** clears: INTF1 vsync, not an underrun (`DpuHw.h:55–59`). A sticky IRQ difference does not identify missing pixels. |
| `dsi0.reset`, `dsi1.reset`, physical `118` | Each `1c00→0` | Bits 10–12 lost; **undefined** by `dsi.xml:291`. Both native and Linux write literal RESET `1→0`. |
| `dsi0.fifo`, `dsi1.fifo`, physical `00c` | Each `1010→88881090` | CMD_MDP underflow bit 7 and four HS lane underflows bits 19/23/27/31 (`dsi.xml:118–144`). |
| `dsi0.status0`, physical `008` | `2008→8000200d` | CMD engine + CMD_MDP + VIDEO engine busy and contention; DMA busy bit **1 is clear** (`dsi.xml:109–116`). |
| `dsi1.status0`, physical `008` | `8→8000200d` | Same final defined fields; bit 13 is undefined in the XML. |
| `dsi0.clk-status`, `dsi1.clk-status`, physical `120` | Each `8067c3→8077c3` | XOR=`1000`: bit **12 DSI_DYN_CMD_PCLK_ACTIVE** (`dsi.xml:313`). |

**STATUS0 correction (question 1 and 1g):** `8000200d = BIT(31) | BIT(13) | BIT(3) | BIT(2) | BIT(0)`.
Bit **0** is `CMD_MODE_ENGINE_BUSY`, **1** is `CMD_MODE_DMA_BUSY`, **2** is `CMD_MODE_MDP_BUSY`,
**3** is `VIDEO_MODE_ENGINE_BUSY`, **4** is `DSI_BUSY`, **31** is `INTERLEAVE_OP_CONTENTION`
(`dsi.xml:110–115`). Bits 1 and 4 are **zero** here. There is **no stuck command DMA in this clean
wake sample**. This differs from m5obs2's post-serial-3 `8000a00f`, which did set bit 1.
The command **pixel/MDP** engine can be busy without a software DCS DMA: these are distinct engines.
TPG=0 disables the command test source, not `CTRL.CMD_MODE_EN` or the MDP engine. Busy status plus
bit 12 is consistent with unwanted command-pixel activity during video; contention is a recorded
error (`dsi_host.c:1667–1677`), not proof of an outstanding `29` packet or a timestamp for the conflict.
[STATUS0 definition](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L109-L116) ·
[Linux contention handling](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L1667-L1677)

**FIFO correction (questions 1f and 1g):** the four high nibbles each describe one HS lane.
Nibble `1` means **empty** (bits 16/20/24/28), `2` full, `4` overflow, `8` **underflow**
(bits 19/23/27/31). Therefore `9` means empty **plus** recorded underflow, not a deeper FIFO level.

| FIFO value | HS lane empty / underflow | Other set bits |
| --- | --- | --- |
| `00001010` | All 0 / all 0 | Bit 12 LP lane-0 empty; bit 4 undefined. |
| `11111010` | All 1 / all 0 | Same low bits. Clean idle/empty HS FIFOs. |
| `99991090` | All 1 / all 1 | Same low bits plus **bit 7 CMD_MDP_FIFO_UNDERFLOW**. |
| `88881090` | All 0 / all 1 | Same low bits plus **bit 7 CMD_MDP_FIFO_UNDERFLOW**. |

For all four values, **bit 0 VIDEO_MDP_FIFO_OVERFLOW=0**, **bit 3 VIDEO_MDP_FIFO_UNDERFLOW=0**,
**bit 10 CMD_DMA_FIFO_UNDERFLOW=0**, and lane HS overflow bits 18/22/26/30=0. `0x80` is **CMD_MDP
underflow**, not VIDEO_MDP overflow. `99991090→88881090` removes only the four empty flags.
Linux acknowledges FIFO flags by writeback and specifically schedules `dsi_sw_reset()` for CMD_MDP
underflow (`dsi_host.c:1591–1599,1650–1664`). These snapshots do not clear them or demonstrate that
every following frame newly underflows. DSI1's new flags establish an event after `29` cleanup;
DSI0's unchanged flags cannot distinguish retained history from recurrence.
[FIFO definition](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L118-L144) ·
[Linux FIFO acknowledgement](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L1650-L1664)

**Clock correction (questions 1b and 1e):** `CLK_STATUS` is a status register, distinct from restored
`CLK_CTRL=23f`. The exact fields are `dsi.xml:301–318`:

| Clock status field | CMD window `2343` | Idle/VFP `6343` | Bright active `8067c3` | Dark active `8077c3` |
| --- | --- | --- | --- | --- |
| Bits 0/1 AON/DYN AHBM | 1/1 | 1/1 | 1/1 | 1/1 |
| Bits 6/7 AON/DYN BYTE | 1/0 | 1/0 | 1/1 | 1/1 |
| Bits 8/9/10 AON ESC / AON pixel / DYN pixel | 1/1/0 | 1/1/0 | 1/1/1 | 1/1/1 |
| **12 DSI_DYN_CMD_PCLK_ACTIVE** | **0** | **0** | **0** | **1** |
| 13 DSI_CMD_PCLK_ACTIVE | 1 | 1 | 1 | 1 |
| **14 DSI_VID_PCLK_ACTIVE** | **0** | **1** | **1** | **1** |
| 16 PLL_UNLOCKED | 0 | 0 | 0 | 0 |
| **23 undefined in this pin** | 0 | 0 | 1 | 1 |

Other set bits are absent; bits 2–5, 15 and all other undefined positions are zero in these words.
`6343 ^ 2343 = 4000`: the video pixel clock indication disappears in the deliberately **VIDEO-disabled**
command window. That is not evidence that LP command transmission lacks its clocks. `8067c3 ^ 8077c3 = 1000`
tracks dynamic command-pixel activity, not PLL unlock, an HS lane fault bit or ULPS. Bit 23 has no name
in the pinned XML; its presence in both active samples does not explain the divergence.
`LANE_STATUS=1f1f` means all five stop-state bits and all five `ULPS_ACTIVE_NOT` bits set
(`dsi.xml:268–280`): lanes stopped, not a sampled ULPS assertion. No waveform was captured.
[Clock definition](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L301-L318)

### Exact command writes and why 5 / 4 µs is not a timeout

For each link `sendDcs(..., commandOnly=true)` performs the following. The supplied before-state is
CTRL=`1f5`; command-only admission checks DMA busy **bit 1** and VIDEO busy **bit 3**, not CMD_MDP bit 2
(`DpuPanel.h:166–180`). Neither check requires `CLK_STATUS` bit 14. All physical offsets below include +4.

| Native lines | Writes / behavior for `11` and `29` |
| --- | --- |
| `DpuPanel.h:183–191` | INTR_CTRL (`110`) = saved enable bits masked with `a222aa02`, OR `1` to clear old CMD_DMA_DONE. CTRL (`004`) = saved CTRL OR bit 2, still **`1f5`**. TPG (`15c`) = **`30006`**. FIFO (`17c`) gets **`80050011` / `80050029`** and one zero pad word; DMA_LEN (`04c`) = **4**. |
| `189–190` | CMD_DMA_CTRL (`03c`) = `(saved & ~(BIT(31) | BIT(30) | BIT(26))) | BIT(28) | BIT(26)` for LP. XML `189–193` names broadcast 31, FROM_FRAME_BUFFER 28 and LOW_POWER 26; native's master bit 30 is not defined there. DMA_CTRL1 (`390`) = saved value with bit 1 cleared. The raw FIFO/second-control offsets are not defined by the pinned XML. |
| `197–214` | Command-only skips front-porch waiting (`porch=0`), writes **TRIG_DMA=1** at physical `090`, DSI0 then DSI1. It does not mask CPU interrupts in this branch. No TIMING_SHADOW write occurs before trigger. |
| `215–225` | Poll both INTR_CTRL bit 0; if both set, finish immediately. The 1 ms sleep occurs **only if** a poll lacks completion. Thus 5/4 µs is compatible with both four-byte transfers completing before the first sleep. The ~1.1 ms off times include poll scheduling and do not set a minimum LP packet time. |
| `123–136,227–232` | For each completed link, acknowledge CMD_DMA_DONE, TPG=0, physical **`1ec=1`, wait 1 µs, `1ec=0`**, restore saved CMD_DMA_CTRL / DMA_CTRL1 / DMA_LEN / CTRL. Here CTRL returns to **`1f5`**, not `1f7`. Then read ACK_ERR and FIFO. There is no TRIG_DMA=0 cleanup store. |

TPG=`30006` selects CMD_DMA TPG enable bit 1, FIFO mode bit 2 and pattern 3 in bits 17:16;
video TPG enable bit 0 is clear (`dsi.xml:330–337`). The `1ec` pulse is labelled FIFO cleanup by
the imported transport, but `DpuHw.h:167–170` identifies that address as a timing double-buffer mode
word from a different reference. **This XML defines neither it nor a FIFO-reset contract.** DSI1's
clean `fifoAfter` includes the pulse, so it did not immediately latch DSI1 underflow; a later effect
on restart remains unmeasured. No TPG or DMA-length residue survives this successful wake.

INTR `10001` contains CMD_DMA_DONE bit 0; the code explicitly clears stale completion before trigger.
Both ACK masks are 3 with zero errors. These are credible **host transfer completions**, not panel ACKs
or optical proof of `11/29` execution. Linux likewise treats DMA completion as transfer completion,
using memory-backed DMA (`dsi_host.c:1464–1495,2400–2410`); neither pin nor trace supplies an on-wire
latency measurement that would make 4 µs impossible or prove a false interrupt.
[Native transfer and restore](../../package/PianoDpu/DpuPanel.h#L123-L241) ·
[TPG definition](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/registers/display/dsi.xml#L330-L337)

### Reset, mode and video restart versus Linux

| Question / native operation | Pinned Linux comparison and conclusion |
| --- | --- |
| **1c / 1d: reset while EN or lanes enabled?** `DpuPanel.h:500–515` proves both INTFs quiescent, clears **all CTRL bits 0–2** on both hosts (`1f7→1f0`), forces clocks, then pulses RESET. Lane and clock-lane bits 4–8 stay set. | `dsi_host.c:2614–2628` clears exactly the same three bits, retains lane bits and forces the same SM8750 clocks. Native does **not** leave CTRL.EN set. `DpuSlices.h:55–57` and `DpuHw.h:158–166` supply the correct reset/CTRL offsets. Native MMIO writes use `dsb sy` (`DpuSlices.cpp:122,218–231`). EN-before-reset or lane-bit retention is not the demonstrated mismatch. |
| **1c: why DSI0 FIFO survives reset?** No native write acknowledges FIFO_STATUS; reset checks only DMA/video busy bits 1/3 (`515`), not FIFO or CMD/MDP busy. | Linux uses FIFO writeback separately (`1654–1663`). Here STATUS `2005→2000` by `host 11` proves the reset/mode interval clears DSI0's sampled CMD engine/MDP busy flags, while FIFO `99991090` remains. The pin does not promise RESET clears all sticky FIFO history; survival is not proof that the reset failed internally. The `dsi-reset P` row does not read FIFO. |
| **1d: 20 ms versus 1 µs / another SOFT_RESET?** Native RESET literal `1→1 µs→0`, clocks restored at `DpuPanel.h:511–520`. | Generic `dsi_sw_reset` (`dsi_host.c:1151–1178`, delay constant line 36) disables EN, forces clocks, holds RESET **20 ms**, then restores CTRL. The SM8750 helper actually named **`msm_dsi_host_reset_after_phy_sync`**, not `dsi_sw_reset_restore`, uses **1 µs** (`2591–2635`). The native pulse matches that helper. Linux power-on also executes the generic reset first (`2570–2572`), and its error worker uses it for CMD_MDP underflow (`1597–1599`); whether that additional/longer recovery is required for retained M5 is untested. XML `291` exposes only RESET, not a separate DSI_SOFT_RESET register. |
| **1d: RESET `1c00→0`?** Both hosts undergo the literal native writes. | XML `291` has no bitfields for bits 10–12; both Linux helpers overwrite the full word with `1` and `0`. DSI0 underflow predates their loss, and DSI1 remains FIFO-clean after their loss throughout both commands. The loss alone is not sufficient evidence for a preserve-`1c00` fix or a deferred failure mechanism. |
| **1a / 1d: CMD-only→video mode switch?** `setDsiMode(true)` at `680` computes `(saved & ~BIT(1)) | BIT(2)` = **`1f5`**. After successful `29`, `714` calls `setDsiMode(false)` (`527–534`), restoring saved **`1f7`** with both modes enabled. | XML `96–104` permits separate VIDEO bit 1 and CMD bit 2; it does not specify exclusivity or a reset-between-modes rule. Linux constructs CTRL **`1f1`** for four lanes (`dsi_host.c:935–955`), enables video to **`1f3`** (`1181–1203,2473–2478`), temporarily overlays CMD to **`1f7`** for DCS (`2222–2227`), then restores **`1f3`** (`2236–2243`). Its enable helper ORs VIDEO without explicitly clearing CMD: the prior baseline determines the result. Native retains the inherited CMD enable after wake; this is a concrete difference and a plausible source of unwanted MDP activity, **not a proven illegal CTRL value**, because bright already uses `1f7` successfully. |
| **1d: missing reset after successful `29`?** `DpuPanel.h:703–714` resets again only on an owned command failure; this run takes no such branch. | Linux restores the pre-transfer CTRL after successful commands (`2236–2252`, `dsi_manager.c:560–564`), with **no mandatory post-`29` host reset**. Its SM8750 reset occurs **before** IRQs, panel commands and video (`2608`, manager `244–270`). Adding a reset after `29` would be a new intervention, not reproduction of that reference sequence. |
| **1d: missing PHY resync?** M5 never writes the PHY; sampled controls and PLL lock equal bright. | Linux manager first toggles both resync FIFOs, releases D-PHY CLK_EN_SEL, then resets hosts (`dsi_manager.c:244–261`). PHY code writes CMN_RBUF_CTRL **0→1** (`dsi_phy_7nm.c:468–475`) and clears CLK_EN_SEL (`478–488`). A static RBUF=1 cannot verify phase alignment; underflow bits describe host lane FIFOs, not this common PHY FIFO. Cold-power sequencing does not prove retained M5 needs the toggle, nor exclude it. |
| **1g: DPU start order?** `startEngines` resets the stopped CTL, restores fetch, submits and flushes the saved route, then enables INTF1→INTF2 (`DpuPanel.h:371–430`). | Linux configures timing/flush in `dpu_encoder_phys_vid.c:446–502` and enables timing only after CTL flush (`668–683`). Native rows 8→9 clear both flushes before row 10 enables timing, and +54 paired frames prove live timing. Neither INTF activity nor lockstep verifies correct DSI packetization; it does not explain command-MDP activation by itself. |
| **Panel state / command delivery** Native sends only `11`, 120 ms, `29`, 10 ms (`DpuPanel.h:689–699`), retaining panel supplies/reset. | P81 video+LP flags are `panel-novatek-nt36532.c:821–836`; its init sends `11→120 ms→29` (`652–654`) then DSC PPS (`656–673`) after vendor setup. M5 has no PPS/init replay or panel-state readback. This remains a possible additional optical issue, but does not itself explain the new host CMD_MDP/HS-underflow flags. |

`DSI_VID_MODE_CTRL` is not a register name in this pin: use **CTRL.VID_MODE_EN** (`dsi.xml:98`)
and **VID_CFG0/1** (`145–163`). Reusing a name/offset from another DSI generation cannot establish a missing write.
[Native reset and mode](../../package/PianoDpu/DpuPanel.h#L497-L535) ·
[Native wake](../../package/PianoDpu/DpuPanel.h#L655-L725) ·
[Linux host reset and mode](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L1151-L1204) ·
[Linux SM8750 reset](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_host.c#L2591-L2635) ·
[Linux resync order](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/dsi_manager.c#L244-L270) ·
[PHY resync implementation](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/dsi/phy/dsi_phy_7nm.c#L468-L490) ·
[DPU timing start](https://github.com/yzddmr6/linux-sm8750-piano/blob/83f0f89bed14/drivers/gpu/drm/msm/disp/dpu1/dpu_encoder_phys_vid.c#L668-L683)

### Root cause boundary and exact disambiguating check

**Closed by m5obs3:** a clean serial-2 wake alone produces the dark display and both-host contention /
CMD_MDP plus lane-HS underflow state. Serial-3 timeout, retained TPG, a stuck DCS DMA, stopped DPU timing,
changed sampled DSC/PHY/PLL configuration and a software `fault` property are not prerequisites.
DSI0's pre-wake busy state clears before `11`; DSI1's new FIFO flags arise **after successful `29`
cleanup and before wake1**, across the saved-mode / CTL / fetch / timing restart interval.

**Mechanism supported, causal store unproven:** both hosts report the command-pixel engine busy alongside
video, with recorded command-MDP/HS underflows and interleave contention, while source/timing continue.
Restoring inherited `CMD_MODE_EN` together with VIDEO after a CMD-only window is a strong candidate
because Linux returns to video-only; however, the working bright control also has `1f7`. The pin
contains no rule proving that the `1f5→1f7` transition necessarily faults, or that clearing bit 2
alone repairs this retained host/PHY state. Neither error flags nor clock activity reveal which store
started the internal request. Choosing a longer reset, RBUF toggle, post-command reset, omission of
`1ec`, PPS replay or CMD clear as **the** deterministic fix would exceed the evidence.

The remaining check is **an intra-wake host boundary trace**, not another unchanged wake1/wake2 pair.
In a separately authorized observation job, retain the same image behavior, optical setup and user-activity
hold and record **both hosts' CTRL, STATUS0, FIFO_STATUS, CLK_STATUS, TPG, CMD_DMA_CTRL, DMA_LEN and
raw `1e4/1e8/1ec`**, with timestamps and INTF EN/STATUS, at these boundaries:

1. Immediately before/after RESET deassertion and after entering `1f5`: distinguish FIFO history from
   reset/mode reactivation, retaining DSI0 versus clean DSI1 as separate controls.
2. After `29` DMA_DONE **before** cleanup, after TPG=0, and before/after the `1ec` pulse and remaining
   restores: determine whether CMD engine/MDP busy or contention begins even while FIFO still reads clean.
3. After the 10 ms wait **before** `setDsiMode(false)`, immediately after its `1f7` write while INTFs
   remain stopped, after CTL reset, after fetch restore and after the route/CTL START flush but **before**
   INTF enable, then immediately after the paired INTF enables and at the first active frame: locate the first
   new DSI1 FIFO error and the first bits 0/2/31 and dynamic command clock bit 12.

The existing ring cannot provide these host samples; collecting them requires separately reviewed
observation instrumentation, **no added hardware-control writes or status clearing**, and is not implemented
by this documentation job. Preserve the back-to-back INTF1/INTF2 enable stores: do not insert host reads
between them. Record observation latency and require the instrumented control to reproduce m5obs3's fault
signature. Once onset is bracketed, a controlled single-variable comparison can test the
specific candidate. For the CMD-enable hypothesis, the candidate intervention is **only** restoring
`saved CTRL & ~BIT(2)` (observed `1f3`) at the final successful command→video handoff before INTF start;
keep the two DCS transactions, reset, raw FIFO cleanup, clocks/PHY and CTL sequence identical. That is a
disambiguating experiment, **not an approved `m5fix1` change**. It must prevent new DSI1 underflows and
both-host command/MDP busy, contention and dynamic command-clock activity during stable video, and restore
bright optical pixels with a completed matching source capture before that candidate can close the cause.
Account separately for DSI0's pre-existing FIFO flags; do not demand that passive observation erase history.
If the fault predates the handoff or survives that single-variable comparison, follow the
measured earlier boundary rather than combining speculative resets/resyncs. No driver or hardware changes
were made for this analysis.

## Fix-image gate

Path: IOMFB power intent → NativeGopDisplay worker/backlight/touch handoff → successful bonded `panelOff`
and retained DSI/PHY sleep → disabled-host reset → command-only `11`, 120 ms, `29`, 10 ms → saved
CTRL `1f7` → CTL/fetch/route/INTF restart → PCC/display power/backlight. **No serial 3.**
[Clean chronology and sampling limits](#optical-controls-and-transition-chronology)

Checks: **ours-OK** for dual-engine stop/freeze, reset with EN/VIDEO/CMD disabled, both wake DMA_DONEs and
cleanup, live paired restart, matched static DPU/host/PHY configuration, locked PLL0, active flip-source
wallpaper evidence and sustained awake/power state. **ours-missing** for post-command/restart host-error
validation and Linux-style CMD_MDP/FIFO/contention handling. Native restores CMD+VIDEO while Linux normally
restores video-only; cold-path PHY resync and panel PPS/init replay are also absent, with their necessity
for retained sleep **unknown**. The full wake-source image transfer, panel packet receipt/state and exact
first faulting store remain **unknown**. [Exact comparison](#reset-mode-and-video-restart-versus-linux)

Root cause: **Clean serial-2 host restart fault localized; deterministic repair OPEN.** DSI1 stays FIFO-clean
through `29` cleanup, then joins DSI0 in **CMD_MDP + four HS lane underflow** and recorded interleave
contention with command/MDP and video busy. **CMD_DMA busy is clear; VIDEO FIFO overflow is clear**;
CLK_STATUS's only bright→wake change is **dynamic command pixel clock active**. The best-supported candidate
is unwanted command-MDP activity across the saved-mode/video restart, but existing host samples do not
separate cleanup, mode restore, CTL/fetch submission and INTF enable, or prove CMD clear alone repairs it.
RESET `1c00` loss and a missing extra reset/resync are not established causes.

ONE change: **HOLD — do not build `v131y12u-m5fix1` from an asserted root cause yet.** No minimal repair
is proved. Next obtain the precisely bracketed, read-only host samples above; if the final-mode handoff
is implicated, test only final CTRL CMD-bit clear (`1f7→1f3`) with every other operation fixed. Do not bundle
reset length, `1ec`, RBUF, PPS or RESET-bit preservation into that comparison. This job changes documentation
only. [Required disambiguation and candidate limits](#root-cause-boundary-and-exact-disambiguating-check)
