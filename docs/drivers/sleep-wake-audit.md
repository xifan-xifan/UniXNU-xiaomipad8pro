# Native s2idle sleep/wake audit and host-side qualification plan

**Status: HOLD. Analysis only, 2026-11-27.** This audit reads the native sources at main baseline
`c332164f6845db5b423404b4496469449742502b`. It changes no driver, kernel, build recipe or daily policy.
No tablet operation or new hardware qualification was performed. `ours-OK` below means the specified mechanism exists in
source; it does not certify a complete sleep cycle or a hardware power saving.

The task's B6 scope includes board-temperature freshness during charging handover. In the
[hardware backlog](../BACKLOG-hardware.md), **B6 is PMIC ADC thermistors**, B13 is per-unit sensor calibration, and B3
includes orientation. These are separate dependencies of sleep/wake, rather than alternative names for s2idle.
See [power](../power.md), [power IRQs](../power-irq.md), [display](display.md),
[M5 analysis](dpu-m5-analysis.md), [auto-rotate](auto-rotate.md) and [Wi-Fi rejoin](wifi-toggle-rejoin.md).

## Evidence and reference boundaries

The Linux comparison in this document is **only**
[`yzddmr6/linux-sm8750-piano@7f8d6bedde479a01f4a05d6d0581b43c7fd20a01`](https://github.com/yzddmr6/linux-sm8750-piano/commit/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01).
Files were read from that commit's Git objects, independent of the reference checkout's current HEAD and working tree.
Linux is read as a behavioral specification; no implementation is copied.

| Ref | Public file at that pin; relevant symbols | What it establishes |
| --- | --- | --- |
| L1 | [kernel/power/suspend.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/kernel/power/suspend.c): `suspend_devices_and_enter`, `suspend_enter`, `s2idle_loop`, `s2idle_enter` | Process freezing, device suspend, late/noirq stages, s2idle and reverse resume stages. Native uses XNU PM and its own CPU freeze rather than this Linux callback implementation. |
| L2 | [drivers/power/supply/xiaomi_mca.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/power/supply/xiaomi_mca.c): `xiaomi_mca_charge_limit_poll`, `xiaomi_mca_quiesce`, `xiaomi_mca_prepare`, `xiaomi_mca_complete` | Higher of both pack SOCs controls hysteresis. Prepare drains work before suppliers suspend, stops CP/auth, restores source voltage/currents, and can reject unsafe handover. Complete restarts polling. This is stronger than merely freezing a charger worker. |
| L3 | [drivers/input/misc/pm8941-pwrkey.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/input/misc/pm8941-pwrkey.c): `pm8941_pwrkey_suspend/resume`; [drivers/irqchip/qcom-pdc.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/irqchip/qcom-pdc.c): `qcom_pdc_gic_set_type`, `qcom_pdc_gic_enable` | Linux enables wake IRQs and programs hierarchical PDC interrupt routing. Native polling of PON status is a different wake mechanism. |
| L4 | [drivers/hid/hid-nanosic-wn8030.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/hid/hid-nanosic-wn8030.c): `nanosic_wn8030_bridge_sleep/wake`, `nanosic_wn8030_suspend/resume`; [touch core](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/input/touchscreen/nt36532e-thp/core.c): `nvt_quiesce`, `nvt_suspend/resume` | Release accessory inputs, separate panel and system gates, cancel work before disabling touch IRQ, and allow the later of panel-enable and SPI-resume to queue startup. |
| L5 | [panel-novatek-nt36532.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/gpu/drm/panel/panel-novatek-nt36532.c): `nt36532_off/unprepare`, `piano_init_sequence`, `piano_reset`; [ktz8866.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/video/backlight/ktz8866.c): `ktz8866_set_brightness`, `ktz8866_backlight_ops` | Piano off sends display-off, waits 20 ms, enters sleep, waits 100 ms, then resets/powers down. Cold prepare includes vendor/PWM initialization, sleep-out, 120 ms, display-on and DSC PPS. Backlight uses dual-chip brightness and `BL_CORE_SUSPENDRESUME`. |
| L6 | [dpu_encoder.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/gpu/drm/msm/disp/dpu1/dpu_encoder.c): `dpu_encoder_virt_atomic_disable`; [dsi_manager.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/gpu/drm/msm/dsi/dsi_manager.c): `enable_phy`, `dsi_mgr_bridge_power_on`, `dsi_mgr_bridge_post_disable`; [dsi_host.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/gpu/drm/msm/dsi/dsi_host.c): `msm_dsi_host_reset_after_phy_sync` | At this pin, DPU disable calls slave disable before master disable, then defers shared video-CTL cleanup until the master has waited. DSI cold startup resets both PHYs before PLL setup, resyncs both FIFOs, releases clock select, and resets both hosts after PHY sync. |
| L7 | [ath12k/core.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/net/wireless/ath/ath12k/core.c): `ath12k_core_suspend`, `ath12k_core_suspend_late`, `ath12k_core_resume_early`, `ath12k_core_resume`; [ath12k/pci.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/net/wireless/ath/ath12k/pci.c): `ath12k_pci_pm_ops` | Drain TX, disable IRQ/CE, power HIF down, power up early, then wait for restart completion. The restart-completion pre-completion handles an aborted late suspend. This supports clean restart as a reference, not transparent link retention. |
| L8 | [pwrseq-qcom-wcn.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/power/sequencing/pwrseq-qcom-wcn.c): `pwrseq_wcn7860_of_data`, `pwrseq_qcom_wcn_bt_unit_data`, `pwrseq_qcom_wcn_wlan_unit_data` | Six common regulators precede independent radio enables; WCN7860 has a 50 ms startup delay. Shared dependency lifetime must survive either radio's suspend order. |
| L9 | [ufs/core/ufshcd.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/ufs/core/ufshcd.c): `ufshcd_uic_hibern8_enter/exit`, `ufshcd_system_suspend/resume`, `ufshcd_suspend/resume`; [ufs/host/ufs-qcom.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/ufs/host/ufs-qcom.c): `ufs_qcom_suspend/resume` | Hibern8 is distinct from host IRQ/clock/regulator low power and Qualcomm lane-clock/ICE handling. Native implements the retained-controller link subset. |
| L10 | [adreno/a6xx_gpu.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/gpu/drm/msm/adreno/a6xx_gpu.c): `a6xx_gmu_pm_suspend/resume`; [soc/qcom/rpmh.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/soc/qcom/rpmh.c): `rpmh_flush` | GMU stop and devfreq suspend are explicit. RPMh cached sleep/wake sets are flushed separately from active votes; a sleeping CPU alone proves neither CX collapse nor DDR self-refresh. |
| L11 | [iris/iris_probe.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/platform/qcom/iris/iris_probe.c): `iris_pm_ops`, `iris_pm_suspend/resume`; [camss/camss.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/media/platform/qcom/camss/camss.c): `camss_runtime_suspend/resume`, `camss_pm_ops` | Iris has HFI PM behind force-runtime system callbacks; CAMSS connects system sleep to runtime suspend/resume. Successful per-session cleanup is not a substitute for that system boundary. |
| L12 | [sm8750-xiaomi-piano.dts](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts), [sm8750.dtsi](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/arch/arm64/boot/dts/qcom/sm8750.dtsi) | Wiring, CPU idle states, lid wake source, bonded DSI clock topology, backlight buses and shared supplies. These do not prove firmware register retention on native wake. |
| L13 | [sound/soc/soc-core.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/sound/soc/soc-core.c): `snd_soc_suspend`, `snd_soc_resume`, `soc_resume_deferred`; [qcom_q6v5_adsp.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/remoteproc/qcom_q6v5_adsp.c): `adsp_start`, `adsp_stop` | ASoC blocks userspace, mutes DACs, suspends non-exempt PCMs, drains delayed work and changes component/pin state; resume can be deferred. Remoteproc start/stop are separate lifecycle operations, not proof that shared ADSP is stopped at every system sleep. Native Audio implements its own graph/DMA/amp boundary. |
| L14 | [thermal/qcom/tsens.c](https://github.com/yzddmr6/linux-sm8750-piano/blob/7f8d6bedde479a01f4a05d6d0581b43c7fd20a01/drivers/thermal/qcom/tsens.c): `tsens_suspend_common`, `tsens_resume_common` | This pin arms/disarms thermal wake IRQs when permitted; reinitialization is conditional on suspend-to-RAM. Do not equate native optional threshold masking or B6 ADC sampling with this Linux wake-IRQ policy. |

The current [M5 analysis](dpu-m5-analysis.md) also compares a **newer** Linux pin, `83f0f89bed14`, with a master-first
shared-CTL handoff. Do not attribute that later sequence to L6. Main's native `panelOff` implements master-first handoff;
its relationship to the requested older reference is a deliberate difference, not an exact match.

For Apple 27.3, the auditable boundary is the repository's registered IOKit power states and framebuffer adapter. Apple's
public [`setPowerState` contract](https://developer.apple.com/documentation/kernel/ioservice/1532866-setpowerstate)
requires completion before an implied acknowledgement, or a time budget followed by an explicit acknowledgement. It also
warns that storage callbacks have no deterministic ordering relative to unrelated drivers and may lack disk I/O.
Apple's [power-management guide](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/PowerMgmt/PowerMgmt.html)
requires serialization of device state because PM invokes callbacks outside the driver's work loop. These are public API
expectations, not a reverse engineering claim about 27.3 `powerd`, WindowServer internals, or Apple's platform sleep firmware.
Version-specific behavior beyond the local adapter and recorded results remains `unknown`.

## Ordering: what actually precedes what

The root contract is [PianoPowerContract.h](../../package/include/PianoPowerContract.h): `pianopower::join`,
`powerStates`, `cancelSystemSleep`, `displayScanout`. It registers off/on states through the platform expert. It explicitly
says that unrelated drivers go down in no fixed order. PianoBattery and PianoAdsp are parts of PianoPower, PianoAuto is
portable sensor/agent policy, and PianoWcnRails is a shared helper; none is an additional independently ordered PM node.
PianoDpu is called through the framebuffer worker rather than joining its own two-state PM node.

1. **Request and admission.** IOPMrootDomain handles idle/explicit sleep. Keyboard hall policy reports clamshell state;
   PianoInput/PianoPolicy can request sleep or display activity. Kernel `piano-sleep=1` overrides the ADT sleep prohibition.
   Without it there is no native system s2idle. The 0064 cancellation window opens before children are notified.
2. **Device preparation.** Interested-client notifications and power-tree off transitions run while worker threads still
   execute. Each participant parks/drains its own work and acknowledges. Power-tree ancestry orders child before parent
   on power-down and parent before child on power-up. The explicit storage edge is NativeRamBlock → NativeUfs. There is
   no general edge ordering Power, Sensors, Wi-Fi, Bluetooth, GPU, display, touch and audio against one another.
3. **Display sub-sequence.** NativeGopDisplay combines system and API power. It publishes off, queues backlight blank,
   drains the current presentation job, synchronously asks for confirmed dual-chip blank and touch SPI park, then calls
   PianoDpu's worker. DPU blocks flips, sends off DCS, stops the bonded timing path and suppresses fetch. This sequence
   also runs for display-only idle and must not be mistaken for a system suspend.
4. **Last thread-context boundary.** After device off acknowledgements, the piano branch of `IOCPUSleepKernel` executes
   PreSleep platform actions. PianoA830 lowers shared host votes here, after its own GPU slumber, retaining a display
   floor for running or unknown scanout. This is a shared-resource barrier; sibling callback arrival order is not one.
5. **CPU park/freeze.** `piano_s2idle_enter` baselines wake sources, arms the watchdog and parks other CPUs. Park/freeze
   deadlines can abort entry. After quiesce, driver threads, deferred callbacks and normal polling do not run. The boot
   CPU polls wake state and enters its allowed PSCI idle state. The 0064 cancellation path also returns through normal
   recovery without needing an actual suspended interval.
6. **Wake detection and time.** The boot CPU detects a permitted edge/latch, timer, or pending peripheral IRQ. A power-key
   wake waits for release, bounded at 3 s. Native rebases absolute time to exclude sleep, advances calendar time, releases
   CPUs and restores timers. A pending device interrupt runs after release. Kernel polling is distinct from driver polling.
7. **Shared resources first.** PostResume platform actions restore GPU-owned rails and DDR votes before ordinary device
   on callbacks. A failed vote restore is retried at GPU kOn; failure keeps its submission gate shut.
8. **Device resume.** Drivers get on callbacks without a fixed sibling order. NativeUfs restores the link before releasing
   its child. Many other on callbacks acknowledge after scheduling recovery, so root-domain wake completion alone does
   not establish usable Wi-Fi, touch, sensors, audio or an optically lit display.
9. **Display wake sub-sequence.** On the stopped-engine M5 path, DPU verifies quiescence, resets both hosts preserving
   CTRL/CLK_CTRL, enters a command window, sends sleep-out/display-on, restores host/video/fetch/CTL state, checks engine
   motion and replays PCC/Night Shift. NativeGopDisplay then releases its touch hold, publishes on, relights backlight
   and releases presentation. Failure may latch backlight-only behavior for the boot; its name is not proof of recovery.
10. **Post-wake services.** Charger policy resync, fresh ALS/orientation sessions, radio restart/authentication and audio
    rearm complete on their workers. Evaluate these endpoints separately against the same wake number and time window.

Kernel evidence: [0062](../../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch),
`IOCPUSleepKernel`, `piano_s2idle_enter`, `piano_wake_baseline/poll`, `piano_s2idle_loop`, `piano_wait_power_release`;
[0064](../../patches/k27/0064-k27-piano-llcc-island-live-1-and-piano-sleep-cancel-1.patch),
`piano_s2idle_cancel`, `willNotifyPowerChildren`, cancellation-window policy. These are source patches, not evidence that
a particular RAM boot loaded them. Verify the boot's identity and counters before grading its behavior.

## Path & Check Matrix

`ours-OK`: source implements the named step, conditional on its gate and success path.
`ours-missing`: the named mechanism/barrier is absent, including intentional retained-power differences from Linux.
`unknown`: runtime outcome or an uninspected OS/firmware dependency is not established. Missing low-power teardown does
not by itself establish an unsafe retained-power path. Every failure/timeout row stands independently of its success row.

### Root, wake inputs and CPU boundary

| ID / step | Status | Exact native evidence and check |
| --- | --- | --- |
| R1: Join off/on power tree | `ours-OK` | [PianoPowerContract.h](../../package/include/PianoPowerContract.h), `join`, `powerStates`: PM init, platform registration, two states, initial on. Confirm each enabled driver's join result; a compiled callback is insufficient. |
| R2: Admit/cancel system sleep | `ours-OK` | [0062](../../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch), `IOCPUSleepKernel`; [0064](../../patches/k27/0064-k27-piano-llcc-island-live-1-and-piano-sleep-cancel-1.patch), `piano_s2idle_cancel`; [contract](../../package/include/PianoPowerContract.h), `cancelSystemSleep`. Cancel is opt-in, may be refused, and still requires safe local rollback. |
| R3: Stop awake power-key poller | `ours-OK` | [PianoInput.cpp](../../package/PianoInput/PianoInput.cpp), `PianoInput::setPowerState`, `park`, `pollThread`: park between samples, wake rereads keys and suppresses a held key until released. |
| R4: Input park timeout | `unknown` | Same `setPowerState`: 500 ms then implied ACK even if not parked. A frozen thread is not proof that its pre-freeze sample completed. Retain the timeout line and source baseline. |
| R5: Lid/power-button policy | `ours-OK` | [PianoKeyboard.cpp](../../package/PianoKeyboard/PianoKeyboard.cpp), `lidNotify`, `acceptHall`; [PianoPolicy.cpp](../../package/PianoPolicy/PianoPolicy.cpp), `perform`; [PianoInput.cpp](../../package/PianoInput/PianoInput.cpp), `askPolicy`. Gates distinguish report-only, display wake and actual system sleep. |
| R6: Hall debounce | `ours-OK` | [CoverPower.h](../../package/PianoKeyboard/CoverPower.h), `HallDebounce::kConfirmMs`, `sample`, `waitMs`: a latched edge requires 20 ms confirmation; without a latch it uses two-sample polling. `sleepPark` directly resamples/reset-debounces after wake. The frozen kernel's lid rule is two open poll samples, not this 20 ms worker rule. |
| R7: Baseline wake sources | `ours-OK` | [0062](../../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch), `piano_wake_baseline`, `piano_wake_poll`: PON INT_RT_STS via SPMI, GPIO80, GPIO95 latch, Type-C status. Mask bits 1/2/4/8/16 mean power/lid/USB/volume-down/keyboard. Lid arms only if closed at entry; GPIO97 is never the keyboard wake line. |
| R8: PDC interrupt-driven PON/lid wake | `ours-missing` | [0062](../../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch), `piano_s2idle_loop`, and [PianoInput.cpp](../../package/PianoInput/PianoInput.cpp), `pollThread`, use polling; no equivalent to L3's end-to-end PON/PDC wake routing here. Short presses can fall between polls. Existing peripheral SPI wake support does not close this gap. |
| R9: CPU park, freeze, watchdog and abort | `ours-OK` | [0062](../../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch), `piano_s2idle_enter`, `piano_s2idle_loop`; [0064](../../patches/k27/0064-k27-piano-llcc-island-live-1-and-piano-sleep-cancel-1.patch), cancel handling. Require actual entry/wake or explicit aborted/cancelled classification, not just a root sleep request. |
| R10: Time and held-key suppression | `ours-OK` | [0062](../../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch), `piano_wait_power_release`, `piano_s2idle_take_wake_utc`, `clock_wakeup_calendar` hook. Compare wall time and awake-time deadlines; never infer SSC QTimer behavior from XNU absolute time. |
| R11: Linux-like complete RPMh SLEEP/WAKE sets | `ours-missing` | [PianoA830.cpp](../../package/PianoA830/board/PianoA830.cpp), `votesDown/votesUp`, and [0062](../../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch), `piano_s2idle_enter`, lower active votes/poll counters; they do not implement L10 `rpmh_flush` for all retained resources. |
| R12: CX-off / DDR self-refresh actually reached | `unknown` | Same kernel entry/wake RPMh `aosd/cxsd/ddr` samples; [contract](../../package/include/PianoPowerContract.h), `llccIslandReady`; [PianoA830.cpp](../../package/PianoA830/board/PianoA830.cpp), LLCC platform query. LLCC reservation and a successful timer wake are prerequisites/evidence fragments, not residency proof. |
| R13: Full wake versus optical dark wake | `ours-OK` | [0062](../../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch), IOPMrootDomain wake branch, claims `piano.<source>` and calls `requestUserActive`. This native path requests a full wake. M5's “dark wake” describes a failed visible picture; it does not establish an intentional Apple maintenance/dark-wake policy state. |

### Charging, battery and ADSP

| ID / step | Status | Exact native evidence and check |
| --- | --- | --- |
| P1: Policy sleep handover | `ours-OK` | [PianoPower.cpp](../../package/PianoPower/PianoPower.cpp), `start`, `setPowerState`, `stepPolicy`: policy-enabled power-tree join, 3 s async budget, worker handover capped at 2 s. CP/MiPPS cleanup readiness also gates acknowledgement. |
| P2: Handover fault prevents suspend | `ours-missing` | Same `stepPolicy`: timeout sets `PianoChargeHandoverFault` and acknowledges anyway, without `cancelSystemSleep`. Unlike L2's failing prepare return, sleep can proceed with an unconfirmed charger plan. Keep this as a gate failure, not a successful handover. |
| P3: Preserve 80/75 hysteresis | `ours-OK` | [ChargePolicy.c](../../package/PianoPower/ChargePolicy.c), `piano_charge_soc_hold`, `protection`, `forget`, `piano_charge_decide`: stop at/above configured stop, resume at/below resume, keep prior hold between; wake forgets applied transport state but preserves battery hysteresis. Higher pack SOC is supplied through [ChargeLink.h](../../package/PianoPower/ChargeLink.h), `ChargeLink::fill`. Stale gauges do not grant a resume. |
| P4: Sleep policy versus awake hold | `ours-OK` | [ChargePolicy.c](../../package/PianoPower/ChargePolicy.c), `handover`; [PianoPower.cpp](../../package/PianoPower/PianoPower.cpp), `setHold`, `stepPolicy`: return source to 5 V/class limits, remove charging idle-sleep hold during handover, stop charging for protected sleep under the configured policy. Do not confuse the PM assertion with the SOC hold bit or with BH-2 health presentation. |
| P5: Pause gauge/report traffic | `ours-OK` | [ChargeLink.h](../../package/PianoPower/ChargeLink.h), `SleepReports::update`; [PianoPower.cpp](../../package/PianoPower/PianoPower.cpp), `run`: after handover ACK, gauge/Type-C/RTC reporting pauses until wake. MCA/DSP protocol ownership remains with PianoPower. |
| P6: Wake replay and B6 freshness | `ours-OK` | [PianoPower.cpp](../../package/PianoPower/PianoPower.cpp), `stepPolicy`, `relinkMca`, `sampleSafety`, `publishCharge`; [ChargeLink.h](../../package/PianoPower/ChargeLink.h), `chargerReadingCounts`, `step`: wakes force resync/replay, validate pack/board ages and charger-PD generation. Thermal and Power siblings can resume in either order; stale data must not be accepted as fresh because root wake finished. |
| P7: Battery service role | `ours-OK` | [PianoBattery.cpp](../../package/PianoPower/PianoBattery.cpp), `AppleSmartBattery::apply`, `pianoBatteryUpdate`, used by `PianoPower::publishBattery`: OS battery reporting, not a separate charger or wake-source owner. A health stop is not `FullyCharged`. |
| P8: Shared ADSP health and quiet | `ours-OK` | [PianoAdsp.cpp](../../package/PianoPower/PianoAdsp.cpp), `piano_adsp_health`, `piano_adsp_quiet_set`, `piano_adsp_quiet`; [PianoAdspContract.h](../../package/include/PianoAdspContract.h): fatal state, reports, link/charger generations and quiet bits reach Sensors/Audio. `piano_adsp_wake` wakes its service thread; it is not a hardware system-wake routine. |
| P9: ADSP subsystem suspend/power collapse | `ours-missing` | [PianoAdsp.cpp](../../package/PianoPower/PianoAdsp.cpp), `piano_adsp_start`, service loop and health API, have no independent system off/on handler; [ChargeLink.h](../../package/PianoPower/ChargeLink.h), `SleepReports`, explicitly retains DSP traffic. Sensor/audio stream teardown does not shut down shared charger firmware. Whether deeper DSP idle occurs autonomously is `unknown`. |
| P10: 27.3 battery-health persistence | `unknown` | [PianoBattery.cpp](../../package/PianoPower/PianoBattery.cpp) supplies ordinary battery fields and a default-off logical health pack; it does not establish a BH-2 persistent-storage implementation. The [recorded opt-in BH-2 RAM result](../results-2026-11-27.md#ram-qualification) reports `kIOReturnIOError` writing health history. OWNER-0748 authorizes host-side analysis; the [27.3 trace](power-bh2-powerd-analysis.md) identifies error normalization and a separate missing NVRAM controller, but the raw failed operation remains unknown. Persistence and the fix-image gate stay HOLD; 80/75 hysteresis is a separate contract. |
| P11: Require a new gauge sample before wake decisions | `ours-missing` | [PianoPower.cpp](../../package/PianoPower/PianoPower.cpp), `setPowerState`, `stepPolicy`, `run`: wake increments `fWakes` but does not invalidate `fPackMs` or force `reportDue`; `packsFresh` accepts up to 45 s of absolute-time age. Because absolute time excludes sleep, a pre-sleep gauge sample can still pass that test after a long sleep. A wake resync is not a gauge-sample epoch barrier. Whether a live decision actually precedes the next report needs correlated evidence. |

### Display, backlight and touch handshake

| ID / step | Status | Exact native evidence and check |
| --- | --- | --- |
| D1: System/API display admission | `ours-OK` | [NativeGopDisplay.cpp](../../package/NativeGopDisplay/NativeGopDisplay.cpp), `start_hardware`, `do_power_state_change_gated`: family PM registration; display on requires both power values. Off publication and work enqueue run under the display gate. |
| D2: Drain presentation and confirm prerequisites | `ours-OK` | Same file, `copyLoop`, `setPanelPower`; [PianoBacklight.cpp](../../package/PianoBacklight/PianoBacklight.cpp), `callPlatformFunction`: drain in-flight copy/flip, synchronous dual blank request (800 ms), then touch-panel park, before `PianoDpuPanel`. Refusal skips panel transition. |
| D3: Explicit system PM ACK waits for full physical M5 transition | `ours-missing` | Same `do_power_state_change_gated` queues copy-worker work and returns; it does not add a delayed PM acknowledgement tied to DPU completion. Exact private family behavior is `unknown`. GPU PreSleep's bounded scanout wait is not an unbounded M5 completion barrier. Check transition completion before freeze in console. |
| D4: Dual KTZ8866 blank | `ours-OK` | [Ktz8866.h](../../package/PianoBacklight/Ktz8866.h), `blank`, `setEnable`, `writeEnable`, `bothDark`; [PianoBacklight.cpp](../../package/PianoBacklight/PianoBacklight.cpp), `run`: attempts both ICs and tracks BL_EN readback, with bounded retries. Known cached enable state may satisfy an idempotent request; counters/logs distinguish a new hardware readback. |
| D5: System backlight timeout | `unknown` | [PianoBacklight.cpp](../../package/PianoBacklight/PianoBacklight.cpp), `setPowerState`: 800 ms wait, then implied ACK even if worker did not answer or an IC is not dark. M5's confirmed blank can refuse, while system sleep still continues. |
| D6: Serialize DPU transition with flips | `ours-OK` | [DpuSlices.cpp](../../package/PianoDpu/DpuSlices.cpp), `panelSleep`, `panelWake`, `serve`; [DpuPanel.h](../../package/PianoDpu/DpuPanel.h), `panelOff`, `panelOn`: single worker/flip transition ownership and bounded reply handling. Timeout recovery is boot-latched and may retain a prior trace; inspect serials. |
| D7: DCS off and bonded timing stop | `ours-OK` | [DpuPanel.h](../../package/PianoDpu/DpuPanel.h), `sendDcs`, `panelOff`, `stopEngine`, `submitPanelRoute`: `0x28`, 20 ms, `0x10`, 100 ms; master-first stop, CTL handoff, slave stop, then prefetch suppression. Main differs from L6's older slave-first request order. |
| D8: Cold panel/PHY/PLL shutdown and startup | `ours-missing` | Same `panelOff/panelOn`, versus L5/L6: native retains supplies, panel reset, PHY and PLL; it does not run cold PHY reset/resync, panel vendor init, DCS PWM/CABC setup or PPS replay. Absence establishes a retention assumption, not a proven requirement to add these writes. |
| D9: Retained-path host reset and wake DCS | `ours-OK` | Same `panelOn`, `resetDsiHosts`, `setDsiMode`, `timingQuiescent`: check both stopped, preserve CTRL/CLK_CTRL, clear operation bits on both, reset DSI0 then DSI1, command-only window, `0x11`, 120 ms, `0x29`, 10 ms. Only the allowed short commands are sent. |
| D10: Restore route, motion and colour | `ours-OK` | [DpuPanel.h](../../package/PianoDpu/DpuPanel.h), `startEngines`, `panelOn`; [DpuSlices.cpp](../../package/PianoDpu/DpuSlices.cpp), `panelWake`, `nightShiftUpdate`, `pccUpdate`: saved host state, fetch and route restored, timing/flush/lockstep checked, colour replay before worker completion. |
| D11: Confirm panel accepted DCS / pixels emitted | `unknown` | Same `sendDcs` reports local host DMA completion and ACK/FIFO latches, not a panel BTA response. [Snapshot](../../package/PianoDpu/DpuSnapshot.h), `take`, observes registers, not emitted HS traffic or panel decoder state. Moving INTF counters do not certify a picture. |
| D12: Release touch, relight, allow swaps | `ours-OK` | [NativeGopDisplay.cpp](../../package/NativeGopDisplay/NativeGopDisplay.cpp), `setPanelPower`, `jobDoneAction`; [Ktz8866.h](../../package/PianoBacklight/Ktz8866.h), `applyLit`: match both codes before enabling either, blank on failed enable. Boot fallback after a DPU wake failure must be reported separately. |
| D13: Optical M5 dark wake resolved | `unknown` | [display status](display.md), [M5R evidence](dpu-m5-analysis.md#m5r-dark-wake-and-pinned-linux-comparison): restored compression route, both INTF counters and full external PWM coexist with a dark panel and `gop-shot` returning `EBUSY`. Source pixels during the dark exposure, PLL/PHY state and panel state remain unqualified. |
| D14: ALS validity on backlight resume | `ours-OK` | [PianoBacklight.cpp](../../package/PianoBacklight/PianoBacklight.cpp), `restartAmbient`, `nativeSettle`, `applyAmbient`; [AutoLight.c](../../package/PianoAuto/AutoLight.c), `pal_conditions`, `pal_event`: invalidate old target/revision, settle manual-intent detection and require current stream data. Observe actual fresh ALS samples before calling auto-brightness resumed. |

### Input, sensors, B13 calibration and autorotation

| ID / step | Status | Exact native evidence and check |
| --- | --- | --- |
| I1: WN8030 release before park | `ours-OK` | [PianoKeyboard.cpp](../../package/PianoKeyboard/PianoKeyboard.cpp), `sleepPark`, `applyCover`, `releaseAll`; [CoverPower.h](../../package/PianoKeyboard/CoverPower.h), `CoverPower::sleep`, `releasesInput`: releases keyboard/consumer/pointer and resets filters for release or blank, including release-only when MCU blanking is off. L4 has the equivalent explicit bridge input release. |
| I2: MCU blank, IRQ mask and wake latch | `ours-OK` | Same `blank`, `sleepPark`; [CoverPower.h](../../package/PianoKeyboard/CoverPower.h), `latchDecision`, `latchCfg`: optional command `0x25=0` then sleep GPIO3, mask data GPIO97, arm raw rising GPIO95 only when policy/cover permits. Latch GIC enable stays off; kernel polls it. |
| I3: Keyboard off timeout / failed MCU command | `unknown` | Same `setPowerState` waits 900 ms then ACKs regardless; `blank` logs an unacknowledged command and still asserts GPIO3. No unconditional hardware-off guarantee follows from the callback's return. |
| I4: Keyboard wake and stale cover state | `ours-OK` | Same `sleepPark`, `unblank`: read/clear GPIO95, restore config, reread real hall, clear hall simulation, reread display; release sleep GPIO, bounded readiness wait, drain frames, power command/LED restore. An off display keeps MCU blank. |
| I5: Touch cancel, DMA latency release, IRQ mask | `ours-OK` | [PianoTouch.cpp](../../package/PianoTouch/PianoTouch.cpp), `setPowerState`, `powerStep`, `lost`, `burst`, `park`: cancel contacts/gestures, release CPU latency hold, mask GPIO162 and park the frame thread. `serve` also acknowledges the independent M5 panel hold. |
| I6: Touch system-off timeout | `unknown` | Same `setPowerState`: after 600 ms it ACKs even if not parked. Native M5's separate park acknowledgement is stronger than simply recording system-off. Freeze itself cannot demonstrate a cancelled SPI transfer. |
| I7: Touch resume/re-arm | `ours-OK` | Same `restart`, `powerStep`, `serve`: probe firmware/reset state; GENI reinit on failed probe; bounded download attempts if lost; relearn reference on system resume, reset tracker/gestures and resume edge-based reads. Failure leaves frames stopped until a later resume. System and panel holds must both permit work. |
| I8: Disable sensor streams before sleep | `ours-OK` | [PianoSensors.cpp](../../package/PianoSensors/PianoSensors.cpp), `setPowerState`, `runStreams`: 2 s async budget, 800 ms disable window; [SensorsStream.c](../../package/PianoSensors/SensorsStream.c), `ss_sleep`, `apply`, `ss_force_off`: gate light/acceleration, clear edge, close remaining ports when permitted. |
| I9: Charger quiet overlaps sensor off | `unknown` | Same `runStreams`: at the disable timeout, `h.quiet` prevents forced close, logs `not off: PianoPower asks for quiet`, then ACKs. This preserves charger transaction exclusion but leaves SSC stream quiescence unconfirmed. Callback order cannot be assumed to avoid it. |
| I10: B13 calibration lifetime | `ours-OK` | [PianoSensors.cpp](../../package/PianoSensors/PianoSensors.cpp), RPC/pack service; [SensorsPack.c](../../package/PianoSensors/SensorsPack.c), pack parser; [SensorsStream.c](../../package/PianoSensors/SensorsStream.c), `restart`: stream restart does not replace the unit's calibration with generic data. Keep the same pack identity across the boot; calibration bytes and identifiers stay out of public evidence. |
| I11: Fresh orientation after wake | `ours-OK` | [SensorsStream.c](../../package/PianoSensors/SensorsStream.c), `apply`, `restart`, sample-session handling; [AutoRotate.c](../../package/PianoAuto/AutoRotate.c), agent policy: clear edge/dwell on gating and restart; sample-clock mode rejects discontinuity/duplicate/gap effects before new dwell. Session and sample counters must advance after wake. |
| I12: Agent/display/touch agreement on live wake | `unknown` | [piano-rotated.m](../../package/PianoAuto/daemon/piano-rotated.m), `runPolicy`, `pushTouch`: apply only with active console and awake display, use display readback before touch update. Native mode normally uses panel axes and compositor transform. [Auto-rotate gate](auto-rotate.md#fix-image-gate) still requires physical edge/cover/corner checks; keep it off in daily policy. |

### Radios and shared WCN ownership

| ID / step | Status | Exact native evidence and check |
| --- | --- | --- |
| W1: Default system-sleep park | `ours-OK` | [PianoWifi.cpp](../../package/PianoWifi/PianoWifi.cpp), `readArgs`, `setPowerState`, `parkThread`, `parkRadio`, `restart`: level 8 defaults to `piano-wifi-sleepoff=2`; stop the station on its owner thread, lower WLAN_EN/assert PERST, return rails, park until the matching wake. 4 s PM budget, 3.5 s park bound. |
| W2: Park timeout guarantees radio off | `unknown` | Same `parkRadio`: if the ladder has not parked, do not race its MMIO; log timeout and ACK while leaving shutdown to it. The radio may remain powered through that sleep. Monotonic `gParkedFor` prevents missing a quick park/wake. |
| W3: Transparent association/link retention | `ours-missing` | Same `setPowerState`, sleepoff=0 branch: no complete firmware suspend/MHI M3→M0 retention protocol. [Wi-Fi history](wifi.md) records firmware death after the keep-on sleep experiment. This is not the default or an approved substitute for rejoin. |
| W4: Wake ladder and retained-profile retry | `ours-OK` | Same `setPowerState`, `restart`, `level7`: wake count releases park, fresh ladder starts at level 2; profile mode can use last-network fast restart, rejoin backoff and supplicant authentication. Lower bring-up levels or sleepoff=1 are one-way stop configurations, not equivalent recovery paths. |
| W5: Native macOS auto-reconnect after sleep | `unknown` | Same `restart`: `gFastNext` explicitly excludes `joinRequest().nativeOwner`; [WifiUserControl.h](../../package/PianoWifi/WifiUserControl.h), `WifiJoinRequest`, and `level7` control one attempt per native association generation. Radio-ready/scans are not authorization of the WPA port. Need the same-wake SET20/result, EAPOL/key/authorization and resulting link evidence. Toggle-specific missing-SET20 evidence in [the rejoin audit](wifi-toggle-rejoin.md) is a diagnostic precedent, not proof of the same cause on sleep. |
| W6: Bluetooth system-off and resume | `ours-OK` | [PianoBluetooth.cpp](../../package/PianoBluetooth/PianoBluetooth.cpp), `setPowerState`, `quiesce`, `run`, `levels`; [BtBringup.h](../../package/PianoBluetooth/BtBringup.h), `safeOff`: abort pump/bring-up, BT_EN low, UART/clock/rail release, reset transport framing, report down; on queues a fresh ladder. Host HID pairing/reconnection remains a separate live endpoint. |
| W7: Sibling order cannot drop peer rail | `ours-OK` | [PianoWcnRails.h](../../package/include/PianoWcnRails.h), `piano_wcn_rails_get/put`, `piano_wcn_pmu_lock`; [PianoA830.cpp](../../package/PianoA830/board/PianoA830.cpp), single `piano_wcn_rails_state`/`piano_wcn_pmu_owner` definitions: per-radio owner **bits**, idempotent acquire, shared lock, rails before enable, last releaser only withdraws helper-added enables. This is not a count increment for every get. |
| W8: Failed rail lock/vote and residual consumption | `unknown` | Same helper: bounded lock acquisition, no owner on failed get, retained enables if put cannot take lock, withdraw only with release flag and no owners. Residual UP/added bits may legitimately remain with zero owners. Console rail state and readbacks, not owner count alone, establish the outcome. |
| W9: Both wake endpoints usable | `unknown` | [PianoWifi.cpp](../../package/PianoWifi/PianoWifi.cpp), `level7`, restart-last publication; [PianoBluetooth.cpp](../../package/PianoBluetooth/PianoBluetooth.cpp), `transportResult`, `pump`: PM on acknowledges before radio bring-up/auth completes. Require no lock timeout, completed ladder, usable transport/link, and no loss of the other radio. |

### Storage, GPU and media

| ID / step | Status | Exact native evidence and check |
| --- | --- | --- |
| S1: Data child holds/syncs before UFS | `ours-OK` | [NativeRamBlock.cpp](../../package/NativeRamBlock/NativeRamBlock.cpp), `joinPower`, `setPowerState`, `experimentPower`: read-write store joins NativeUfs as power child; hold/sync or veto. A volatile/read-only RAM boot can omit this persistent-store branch and cannot qualify it by observation alone. |
| S2: UFS opt-in and panic exclusion | `ours-OK` | [NativeUfs.cpp](../../package/NativeUfs/NativeUfs.cpp), `joinPower`, `setPowerState`, `native_ufs_log_panic`; [RootPower.h](../../package/NativeUfs/RootPower.h), `hostSuspend`: only `native-ufs-suspend=1` with usable SM8750 transport; reject not-ready, poisoned or `gHostPanic` before queue hold/link transition. This precondition check does not prove atomic exclusion against a panic starting later. [RootCommands.h](../../package/NativeUfs/RootCommands.h), `hostSlotTake`, separates panic transport from normal held admission; no hardware panic injection belongs in this plan. |
| S3: Queue hold → drain → cache → Hibern8 | `ours-OK` | [RootPower.h](../../package/NativeUfs/RootPower.h), `hostHoldSet`, `hostDrain`, `hostSuspend`: hold new requests; drain within 1000 ms, check doorbells/fatal state, sync writable cache, save/disable AHIT, check lane FSM, enter Hibern8 if needed. AHIT-already-hibernated is an accepted distinct path. |
| S4: Aborted entry rollback/cancel | `ours-OK` | Same `hostSuspend`, `hostLinkRestore`, `ufsCancelWanted`; [NativeUfs.cpp](../../package/NativeUfs/NativeUfs.cpp), `cancelSleep`: undo link/AHIT/hold; optional cancellation for veto, in-flight I/O, sync or link failure. Refused cancel leaves UFS up; failed undo is a root-link failure, not a successful sleep. |
| S5: Resume link before queues | `ours-OK` | [RootPower.h](../../package/NativeUfs/RootPower.h), `hostResume`, `hostLinkUp`, `hostLinkRestore`: Hibern8 exit when required, lane check, NOP OUT, restore AHIT, release host queue, then power child can release Data requests. Five bounded retry attempts; quarantine stops retry and exhausted recovery panics rather than releasing an unusable disk. |
| S6: Full UFS host/PHY low power | `ours-missing` | Same header's ownership contract and [GccUfs.h](../../package/NativeUfs/GccUfs.h): controller/PHY/clocks remain as firmware left them. L9 additionally handles IRQs, clocks, regulator modes, lane clocks and ICE. Hibern8 success does not establish those savings. |
| G1: GPU submission gate and full slumber | `ours-OK` | [PianoA830.cpp](../../package/PianoA830/board/PianoA830.cpp), `suspendConfigure`, `suspendJoin`, `suspendGpu`, `pmWork`; [Adreno830Start.cpp](../../package/PianoA830/adreno830/Adreno830Start.cpp), `adreno830_suspend_gpu`: opt-in running GPU, bounded quiet/recovery wait, gate submissions and enter slumber. 10 s async PM budget. Idle slumber alone does not prove system suspend participated. |
| G2: Shared votes after off, before on | `ours-OK` | Same board `votesDown`, `votesUp`, `callPlatformFunction`; same start `adreno830_suspend_votes`, `adreno830_resume_votes`: PreSleep waits for scanout up to configured bound (default 300 ms), retains DDR/rail floor on running/unknown display; PostResume restores rails before DDR. Default rail floor is NOM_L1, not zero. |
| G3: Failed GPU entry and resume | `ours-OK` | Same `suspendGpu`, `resumeGpu`, [A830Suspend.h](../../package/PianoA830/adreno830/A830Suspend.h), state machine: retain/restore votes, stop participating after failed entry, optionally cancel; restore votes before reopening GPU. Eager wake default has 100 ms grading bound; lazy wake is a different configuration. |
| G4: Shared floor protects all retained clients | `unknown` | [PianoPowerContract.h](../../package/include/PianoPowerContract.h), shared-vote warning; G2 only checks display state, not Iris, camera or shared DSP readiness. A floor appropriate for display alone is not proof that every active client tolerates reduced votes. Zero-rail experiments need all-client evidence. |
| M1: Iris session teardown | `ours-OK` | [PianoVideo.cpp](../../package/PianoVideo/PianoVideo.cpp), `piano_video_native_close`, `encodeThread`; [VideoNativeState.h](../../package/PianoVideo/VideoNativeState.h), session ownership: STOP/shutdown or quarantine retains backing on uncertainty. This protects session lifetime, not system sleep. |
| M2: Video system admission/drain/resume | `ours-missing` | [PianoVideo.cpp](../../package/PianoVideo/PianoVideo.cpp), `class PianoVideo`, `start`: no `setPowerState`, PM join or system sleep admission barrier, including native level 6/VT path. An active decode/encode can outlive preparation and be frozen with firmware still running. L11 has explicit HFI PM. Current source cannot certify media-active s2idle. |
| M3: Camera stop and admission fence | `ours-OK` | [PianoCamera.cpp](../../package/PianoCamera/PianoCamera.cpp), `setPowerState`, `cancelLiveLocked`, live worker/start arbitration: publish `gSuspended`, cancel live capture, then wait for `gSequenceLock` and sequence cleanup; new captures reject suspended/unsafe hardware. On clears admission; clients must request a new stream. |
| M4: Camera PM completion bounded/safe on failure | `unknown` | Same `setPowerState` waits synchronously for the sequence lock with no PM-specific deadline or async budget; live cleanup marks `gHardwareUnsafe` on faults. The callback does not convert that fault to a sleep cancellation. Need actual clean-off result and timing, not just return from lock acquisition. |
| M5: Audio sole PM owner and readiness | `ours-OK` | [PianoAudio.cpp](../../package/PianoAudio/PianoAudio.cpp), `start`, `piano_audio_power_ready`, `setPowerState`, `powerThread`: only `piano-audio-sleep=1` at live level joins; CPU readiness assertion stays until cancel capability is confirmed. 12 s PM budget covers bounded 8 s worker drain. Without this opt-in, live audio has no equivalent system callback. |
| M6: Audio silence, DMA and amps before ACK | `ours-OK` | [AudioDsp.cpp](../../package/PianoAudio/AudioDsp.cpp), `piano_audio_power_request`, `liveRun`; [AudioLive.c](../../package/PianoAudio/AudioLive.c), `audio_live_sleep`, `audio_live_parked`: close HAL readers/admission, finish existing commands, fade/drain/stop graphs, wait for STOP/UNMAP and clock release, mute/power down all amps, zero DMA/reset rings only after ownership is safe. |
| M7: Quiet/failure cannot force unsafe unmap | `ours-OK` | Same `audio_live_sleep_fail`, shared-quiet path; [PianoAudio.cpp](../../package/PianoAudio/PianoAudio.cpp), `powerThread`: fail-safe mute, retain uncertain DMA/mappings, hold readiness assertion and request cancellation. Cancellation refusal is a qualification failure; the assertion alone is not proof of vetoing every explicit sleep. |
| M8: Audio wake reanchor and health gate | `ours-OK` | Same live `audio_live_rearm`, DSP `liveRun`; [PianoAudioDevice.cpp](../../package/PianoAudio/PianoAudioDevice.cpp), `piano_audio_devices_power`: healthy ADSP guard required, reset ring/drift state, restore pins, reanchor HAL timestamps; do not replay stale PCM. `on-ready` and amp-safe state must precede a claimed audio recovery. |
| M9: Live media/ADSP end-to-end recovery | `unknown` | [PianoAudio.cpp](../../package/PianoAudio/PianoAudio.cpp), `powerThread`; [PianoCamera.cpp](../../package/PianoCamera/PianoCamera.cpp), live completion; [PianoVideo.cpp](../../package/PianoVideo/PianoVideo.cpp), native session path: no combined live qualification establishes safe interaction with arbitrary sibling PM order, charger quiet, reduced GPU host votes and M5. |

### Other participants needed for a complete boundary

| ID / step | Status | Exact native evidence and check |
| --- | --- | --- |
| X1: Thermal/B6 suspend/resume | `ours-OK` | [PianoThermal.cpp](../../package/PianoThermal/PianoThermal.cpp), `setPowerState`: park SCMI/TSENS worker, optionally mask threshold interrupts, restore masks/unpark. 500 ms timeout ACKs regardless, so a timeout leaves actual quiescence `unknown`. Fresh board temperatures after wake are needed by P6. |
| X2: USB gadget and console | `ours-OK` | [PianoDwc3.cpp](../../package/PianoDwc3/PianoDwc3.cpp), `setPowerState`, `sleepStep`; [UsbSleep.h](../../package/PianoDwc3/UsbSleep.h), sleep state machine: configured debug gadget may remain connected, mask its event IRQ, otherwise disconnect; verify glue clocks before resume MMIO and defer QSCRATCH writes while asleep. 500 ms worker timeout and failed halt remain unconfirmed endpoints. |
| X3: IR DMA cancellation | `ours-OK` | [PianoIR.cpp](../../package/PianoIR/PianoIR.cpp), `setPowerState`, `releaseDma`: stop/release GENI DMA and request sleep cancel if not idle; on releases the asleep gate. Cancellation support is required to grade a failed drain safely. |
| X4: Torch off, no automatic relight | `ours-OK` | [PianoTorch.cpp](../../package/PianoTorch/PianoTorch.cpp), `setPowerState`, `offNow`: sleep forces off; on resumes the state machine without authorizing a new flash. Register readback and optical torch qualification are separate. |
| X5: Policy/display-follow consumers | `ours-OK` | [PianoPolicy.cpp](../../package/PianoPolicy/PianoPolicy.cpp), input/root-domain dispatch; [PianoDisplayFollow.h](../../package/include/PianoDisplayFollow.h), display-follow helper: policy uses state messages; it does not create a hardware-wide quiescence barrier. |
| X6: Optional USB-host/DP hardware sleep | `ours-missing` | [PianoUsbHost.cpp](../../package/PianoUsbHost/PianoUsbHost.cpp), [PianoXhciHost.cpp](../../package/PianoXhciHost/PianoXhciHost.cpp), [PianoDp.cpp](../../package/PianoDp/PianoDp.cpp): no local `pianopower::join`/system `setPowerState` implementation. Imported Apple family behavior is `unknown`; do not generalize gadget-console recovery to an active external host or DP link. |
| X7: B6 ADC conversion drain/park | `ours-missing` | [PianoThermalAdc.cpp](../../package/PianoThermal/PianoThermalAdc.cpp), `adcThread`, runs `gBoard.round` and periodic sleep independently of `PianoThermal::setPowerState`; there is no ADC worker off/on handshake. CPU freeze eventually stops the AP thread but does not certify conversion/SDAM transaction completion before freeze. Runtime effect is `unknown`. `sampleUs` uses continuous time and [BoardTemps.h](../../package/PianoThermal/BoardTemps.h), `fillBoardTemps`, rejects old/invalid readings; do not confuse this clock with P11's gauge clock. |

## Concrete cross-driver risks to resolve

**M5 dark wake.** A backlight-only daily display sleep can keep scanout active while s2idle succeeds. That is not the
stopped-engine M5 test. Conversely M5 may fail after a display-only off/on without any CPU freeze or Wi-Fi restart.
Use kernel suspend counters to separate them. D3 leaves a potential freeze-before-worker-completion boundary to measure;
G2 conservatively keeps a display floor but does not solve a panel protocol failure. Do not diagnose a PLL loss from a dark
picture alone. DSI0 supplies the bonded clock; PHY1 can select that external source with its own PLL unlocked. Compare each
link against its bright control using host clocks, PHY mux/global-clock state, PLL0 lock, lane status and FIFO evidence.
The snapshot's PHY reference-ready bit and host PLL error latch are not direct local PLL lock readings.

**B6 and battery sampling after long sleep.** The thermal ADC thread has no off/on handshake (X7), separately from the
parked thermal governor. Its board snapshot is aged using continuous time and filtered by validity; the battery gauge
cache uses absolute time, which excludes sleep (P11). Thus “age below 45 s” does not prove that a gauge was sampled after
wake. There is no forced wake report before the first policy decision. This is a source-grounded freshness gap; whether
it changes a live charge decision is an inference requiring a sample/decision timeline. Retain `PackSampleMs`, `PackAgeMs`,
board ages/validity, wake count and the first report/ACK rather than assuming sibling resume order supplies new data.

**B13, ALS and rotation.** Retain the same calibration pack and healthy ADSP generations; re-enable sensor consumers only
when system/display/cover gates permit. Backlight needs a new valid session/target after invalidation. Rotation needs a
cleared old edge plus fresh dwell; XNU's rebased time does not establish whether SSC QTimer continued or reset. The MCU
attach bit gates landscape policy, while hall GPIO80 reports open/closed; they are not interchangeable. A successful lid
wake does not prove rotation or ALS recovered. Keep default-off rotation off unless the already-selected RAM profile
explicitly qualifies it. P6/I9/M7 share the same charger quiet/ADSP-health boundary and can interact without a fixed order.

**Wi-Fi auto-reconnect.** First classify display-only sleep, radio user-OFF and real system sleep. For real sleep record
park, wake restart, native/profile ownership, station attempt, authentication and final link separately. Main's native
path excludes the fast retained-profile shortcut; an old profile or a scan list is not proof of a fresh SET20 or authorized
port. A successful radio restart and Bluetooth readiness can coexist with no network connectivity. Historical profile-mode
wake success in [wifi.md](wifi.md) does not close the native autojoin gap in [wifi-toggle-rejoin.md](wifi-toggle-rejoin.md).
Do not kickstart the join helper, toggle power, force a scan or change PS while collecting the first passive failure.

**Storage/media and shared votes.** Root/Data durability is ordered explicitly, but a RAM-only root can bypass the daily
store's sync branch. Iris has no system barrier. Camera/audio have their own teardown gates and may need shared CX/DDR or
ADSP services until they finish. A frozen AP thread does not stop autonomous DMA or firmware. G2's scanout check alone
cannot justify removing shared rails. Timeout ACKs in Power, backlight, touch, input, keyboard, thermal and USB must remain
visible in the outcome; do not collapse them into a single “sleep passed” result.

## Minimal batched read-only RAM observation checklist

This is a plan for the next authorized RAM observation, **not commands executed by this audit**. Use one already-approved
RAM boot with its existing daily sleep policy, source identity, calibration pack and enabled feature gates unchanged.
Use an already-available host console for collection; Wi-Fi loss must not also lose the evidence. Preserve raw capture
privately and publish only sanitized field changes/counts/verdicts. Do not put boot-argument output, registry dumps, unit
calibration, network identities or raw logs into tracked files.

1. **Once at baseline:** record kernel/build identity, merged boot arguments, `pmset -g custom`, `pmset -g assertions`, and
   the IOPower plane. Check actual sleep, cancel, UFS, GPU, audio, backlight, touch, sensor, Wi-Fi and M5 gates. If a feature
   or sysctl is absent, label its check unavailable; do not enable it during collection. An idle-sleep assertion may mean
   no natural suspend occurs; that yields an incomplete observation, not permission to override it.
2. **Repeat one short batch** while bright, during a naturally occurring display-dark interval if the console is alive,
   immediately after the next ordinary wake, and once more after asynchronous recovery settles. Mark phases using host
   timestamps and kernel suspend counts. No userspace batch can run while all CPUs are frozen: system-sleep interior
   evidence comes from the existing entry/wake console, not an SSH sample taken “during s2idle”.
3. **Keep the existing console window** from before off through worker recovery. Use the exact fields and line prefixes
   below. A missing transition window, cached snapshot or unchanged trace serial cannot establish ordering.

The following reads are sufficient as a shared batch; no scan/join, PM write, test injection, media start or brightness
change belongs in it. Optional unknown sysctls should remain visible as errors, rather than silently treated as zeros.

```sh
date -u
sysctl kern.osrelease kern.osversion kern.boottime kern.piano_bootargs
sysctl machdep.piano_sleep machdep.piano_cpuidle
sysctl machdep.piano_touch_native machdep.piano_audio_ledger
sysctl machdep.piano_sensors_faults machdep.piano_smem_state machdep.piano_smem_crash
for cls in NativeGopDisplay PianoDpu PianoBacklight PianoPower AppleSmartBattery \
           PianoSensors PianoKeyboard PianoWifi PianoBluetooth PianoA830 \
           NativeUfs NativeRamBlock PianoCamera PianoVideo PianoAudio PianoThermal
do
    ioreg -r -c "$cls" -l -w0
done
ioreg -r -n IOPMrootDomain -d 1 -l -w0
date -u
```

At baseline also run `ioreg -p IOPower -l -w0` to confirm the storage parent/child and each enabled PM participant.
`PianoBattery.cpp` publishes class `AppleSmartBattery`; retain the actual service/class match on the candidate boot.
The battery fields can also be read through `ioreg -r -c IOPMPowerSource -l -w0`. Do not create a missing service to fill a
checklist cell.

**Snapshot exception:** `machdep.piano_dpu_snapshot` is store-free only when the existing boot has
`piano-dpu-snapshot=1`. If only `piano-dpu-clkread=1` is enabled, its snapshot handler can issue RPMh ACTIVE queries that
require stores; omit it from this strictly read-only plan. With snapshot mode confirmed, including when both flags are
present, run this pair in each stable display phase:

```sh
sudo sysctl -n machdep.piano_dpu_snapshot
sleep 0.15
sudo sysctl -n machdep.piano_dpu_snapshot
```

[DpuSnapshot.cpp](../../package/PianoDpu/DpuSnapshot.cpp), `snapshotSysctl`, `refresh`, `dpuSnapshotStart`, caches for
100 ms, enforces root access and suppresses ACTIVE queries in snapshot mode.
[DpuSnapshot.h](../../package/PianoDpu/DpuSnapshot.h), `m5LinkClocked`, `take`, checks gates before loads and separately
guards PLL reads with digital-top/bias power bits. Require advancing serial/time, valid gates and mapped reads. Preserve
`unread`, gated skips and all-ones values as unavailable evidence. These reads cannot bracket a 1 µs reset or prove a
40 ms transition history. The transition trace, rather than two stable snapshots, carries that order.

| Boundary | Exact keys / sysctls / console to retain | Required interpretation |
| --- | --- | --- |
| Root and wake cause | `machdep.piano_sleep`, `machdep.piano_cpuidle`; IOPMrootDomain `piano-wake-reason`; `IOCPUSleepKernel enter`, `piano-sleep: suspend`, `woke after`, `cancelled`, `aborted`, entry/wake `RPMh sleep` lines | One matched suspend/wake or explicit cancel/abort. Read armed mask, poll interval, reason and residency deltas. No asserted deep-sleep result from elapsed time alone. |
| Display state/order | NativeGopDisplay `piano-display-power`; PianoDpu `piano-dpu-panel`, `piano-display-scanout`, `piano-dpu-panel-trace`; `NativeGopDisplay: display off/on`, `panel sleep refused`, `panel wake failed`, `PianoDpu: panel:`, `PianoDpu: m5` | Off trace finishes before freeze; wake trace serial is new; distinguish safe rollback, boot-latched refusal and physical M5 execution. Property “awake” alone is insufficient. |
| DSI/PHY/PLL | `machdep.piano_dpu_snapshot`: both INTF EN/STATUS/frame/line, CTL active/master/flush/fetch/DSC route, DSI CTRL/CLK_CTRL/VID_CFG/timing/compression/TRIG/IRQ/lane/ACK/FIFO, PHY `clk-cfg0/1`, `rbuf-ctrl`, `ctrl0/1/2/3/5`, lane configuration/control, all 14 timing words, PLL `local-lock` | Match each link to its own bright sample; interpret PHY1 external clock with PLL0, not PHY1 lock alone. Stable register equality cannot exclude transient reset loss or missing panel PPS. |
| Dual backlight and ALS | PianoBacklight `piano-backlight-control`, `piano-backlight-primary-lit`, `piano-backlight-secondary-lit`, both `-code` and `-readbacks`, `piano-backlight-auto-active`, `piano-backlight-auto-fault`; `BL_EN 0x08`, `read back ok`, PWM `wake` lines | Both dark before M5, matched verified code/enables at wake; retain failed/timeout messages. Full PWM is electrical readback, not optical proof. Check fresh ALS state before grading automatic mode. |
| Charger/battery/B6 | PianoPower `PianoCharge` dictionary: `HealthEnabled/Stop/Resume/Hold/Desired/Pending/Applied`, `EnableReadback`, `ConfigRevision`, `HandoverFault`, `PendingSlots`, `LastACKProperty/Value/Error`, `BoardValid`, `Board0AgeMs` through `Board10AgeMs`, `PackSampleMs`, `PackAgeMs`, `Pack0SOC/Valid`, `Pack1SOC/Valid`; top-level `PianoChargeHandoverFault`; battery `IsCharging`, `FullyCharged`, capacity fields; PianoThermal `BoardAdcState`, `BoardAdc`, `BoardTemperaturesDeciC` | Configuration stays 80/75, prior hold persists in the middle band, ACKed disable during handover, replay after wake and fresh valid thermal/pack data. Require a post-wake sample, not just small awake-time age. No threshold crossing is fabricated by this plan. |
| Charger console | `policy: system sleep: handing the charger over`, `policy: sleep handover done: acknowledged`, `not complete after 2 s`, `buck: handover pending=1 fault=timeout recovery=required`, `policy: awake again`, `policy: wake`, `buck-health:` | A timed-out handover is a failure even if the OS sleeps/wakes. Require no new unsafe writes while reports are paused, and coherent desired/applied/readback after replay. |
| ADSP and sensors | `machdep.piano_smem_state`, `machdep.piano_smem_crash`, `machdep.piano_sensors_faults`; PianoSensors `piano-sensors-dsp`, `-guard`, `-guard-reason`, `-pack`, `-channel-ups`, `-streams`, `-edge`, `-seq`, `-light-session`, `-light-valid`, `-light-fault`, `-light-valid-samples`, `-light-sample-ms`; `streams: system sleep: disabling them`, `streams: ready for the sleep`, `streams: awake` | Same pack, no new fatal/guard strike, valid link/charger generation, stream off or explicit quiet exception, fresh samples/session and cleared stale edge. Passive ADSP exports exist only with the already-enabled sensor trace profile; their absence leaves health details unknown. |
| Cover, touch and rotation | PianoKeyboard `piano-keyboard-hall`, `-attached`, `-status`; PianoTouch `piano-touch-rotation`; `machdep.piano_touch_native`; `sleep N: scanning stopped`, `gpio95 latch`, `wake N: serving again`, `power: system off`, `panel sleep: touch SPI parked`, `power: system on`; existing rotation-agent log | Release events precede park; no timeout, sticky key or contact; real hall resampled, latch restored, touch rearmed. If rotation is already enabled, require fresh dwell and displayed angle/readback before touch update. Physical corner agreement remains a separate attended check. |
| Wi-Fi restart/reconnect | PianoWifi `piano-wifi-level6/7/8`, `piano-wifi-link`, `piano-wifi-restarts`, `piano-wifi-restart-last`, `piano-wifi-user-radio`; `power: the radio is parked for system sleep`, `did not park`, `power: the wake`, `restart N begins (the wake from a system sleep)`, `Wi-Fi is back: the port opened`, `FAILED at stage`, `crash:` | Default clean restart, fresh attempt/auth generation and authorized port after same wake. In native mode include actual Apple80211 ASSOCIATE/SET20/result and supplicant handshake lines. Keep unrelated user-OFF/SET22 distinct. Do not treat scans, firmware-ready or PM ACK as reconnect. |
| Bluetooth/WCN | PianoBluetooth `piano-bt-state`, `piano-bt-host-ready`, `piano-bt-h4-resyncs`; `system sleep: the radio is off`, `wake: the ladder climbs again`, both radios' `wcn rails` lines, `owners now`, `put: state`, `enables withdrawn`, `PMU lock ... given up` | Correlate WLAN bit 0, BT bit 1, UP bit 31 and added-enable bits, without inventing a WCN IORegistry key/sysctl. Either suspend order preserves a live peer; residual enables and lock timeouts are recorded explicitly. |
| Root/Data queue | `NativeRamBlock: suspend`, `store ... sync FAILED`, `NativeUfs: power: suspend`, `queue held and drained`, `ABORTED`, `NativeUfs: power: resume`, `NOP OUT answered`, `queue released`, child `requests released` | Sync/hold before link-down; link/NOP verified before host and child release. No retry exhaustion/quarantine. An absent persistent store is not a tested daily store. |
| GPU/shared votes | PianoA830 `piano-gpu-suspend`; `gpu suspend #`, `REFUSED`, `gpu suspend: votes down`, `gpu resume: votes back`, `gpu resume #`, `WAKE FAILED`; PreSleep/PostResume action lines | GPU down before shared lowering, display floor retained if scanning/unknown, votes up before dependent resume, wake bound satisfied. Log buffering can delay printed GPU lines; use counters/actions, not apparent print order alone. |
| Camera/video/audio | PianoAudio `piano-audio-sleep-ready`, `piano-audio-live`; `machdep.piano_audio_ledger`; `sleep-contract1: drain`, `off-ready`, `off-unsafe`, `on-ready`, `FAILED`, `HAL reanchor`; camera live completion `cleanOff`/fault evidence; Iris session close/quarantine lines | Audio off-ready, safe mute and no uncertain unmap; healthy rearm with new HAL anchor. Camera clean stop within PM budget. No video system-off marker exists: an active Iris session leaves M2 open. No new media workload is started by passive observation. |
| Observation transport/thermal | `PianoDwc3: sleep`, `wake ... glue`, QSCRATCH deferral/refusal lines; `PianoThermal: ... sleep`, worker/mask status and next zone/board update | USB kept/disconnected/stuck is explicit; a console gap alone is not a CPU freeze timestamp. No MMIO on unclocked glue. No park timeout; fresh temperature publication before charger resumes high current. |

After spontaneous Wi-Fi recovery, reading existing interface/link counters can corroborate publication; network usability
still needs a separately authorized traffic check. Passive observation alone cannot prove DHCP, gateway reachability,
DNS or preserved TCP sessions. Likewise, this batch cannot prove physical lid/button timing, acoustic silence or optical
display recovery. Reuse already-recorded optical/source captures when available; an `EBUSY` capture is missing source-pixel
evidence, not a black frame. Do not trigger a capture or new panel command as part of the read-only batch.

## Host-side qualification and completion criteria

The following existing harnesses are appropriate before proposing any future fix image. They run on the host with
synthetic inputs, using the repository test runner; this documentation-only audit does not claim to have rerun them.
No new driver tests or fixtures are introduced here.

| Area | Existing source / runner | Cases to retain or extend for a concrete future change |
| --- | --- | --- |
| Root cancellation and source policy | [piano PM host harness](../../tooling/k27/piano-pm), [self-tests runner](../../scripts/ci/self-tests.py) | Cancel before/during park, cancellation disabled, aborted entry with ordinary on callbacks, initially held power key, closed-at-entry lid, GPIO95 latch and stale cancel reset. |
| Charger/B6/ADSP | [power_host.cpp](../../package/PianoPower/tests/power_host.cpp), [battery_host.cpp](../../package/PianoPower/tests/battery_host.cpp), [thermal_host.cpp](../../package/PianoThermal/tests/thermal_host.cpp) | 80/75 boundaries and 76–79 hold memory across wake/generation, stale/failed gauges, long sleep with small absolute-time gauge age, first decision before next report, pending handover timeout, thermal continuous-time age/generation, independent ADC conversion at freeze, shared quiet and CP/MiPPS cleanup. Require plan/ACK/readback distinctions; model missing ADC PM barriers explicitly. |
| M5/backlight | [dpu_panel_host.cpp](../../package/PianoDpu/tests/dpu_panel_host.cpp), [dpu_snapshot_host.cpp](../../package/PianoDpu/tests/dpu_snapshot_host.cpp), [backlight_host.cpp](../../package/PianoBacklight/tests/backlight_host.cpp) | Bonded stop/CTL handoff/wake/rollback; command-only window; timeout/late reply; independent IC faults and last common code; zero-store snapshot and gated/PLL-unread behavior; pending ALS revision at wake. A synthetic successful DSI transfer cannot certify panel acceptance. |
| Input and orientation | [keyboard_host.cpp](../../package/PianoKeyboard/tests/keyboard_host.cpp), [touch_host.cpp](../../package/PianoTouch/tests/touch_host.cpp), [touch_native_host.cpp](../../package/PianoTouch/tests/touch_native_host.cpp), [sensor runner](../../package/PianoSensors/tests/run-host-tests.py), [auto runner](../../package/PianoAuto/tests/run-host-tests.py) | 20 ms bounce boundaries, release-only and blank branches, already-blanked suspend, pending SPI/contacts, system-on while panel hold remains, fresh ALS sessions, SSC continuous/jumped/reset clocks and cover attach versus hall state. |
| Wi-Fi/BT shared rails | [wifi_host.cpp](../../package/PianoWifi/tests/wifi_host.cpp), [wcn_rails_host.cpp](../../package/include/tests/wcn_rails_host.cpp) | Repeated/quick sleep-wake, monotonic parked generation, park timeout, user-OFF concurrent with wake, native fresh association versus profile retry; get/put permutations, failed lock/get, peer owner retention and final release of only added enables. |
| Storage/GPU | [NativeUfs tests](../../package/NativeUfs/tests), [test_a830suspend.cpp](../../package/PianoA830/adreno830/tests/test_a830suspend.cpp), [test_start.cpp](../../package/PianoA830/adreno830/tests/test_start.cpp) | Child veto, panic exclusion before holds, in-flight drain, already-Hibern8/AHIT restore, failed entry undo, NOP/retry exhaustion; GPU busy/recovery, display unknown, vote rollback, cancel disabled and eager/lazy wake. No panic is injected on hardware. |
| Media and other participants | [audio host runner](../../package/PianoAudio/tests/run-audio-host.sh), [camera host runner](../../package/PianoCamera/tests/run-camera-host.sh), [video_host.cpp](../../package/PianoVideo/tests/video_host.cpp), [usb_sleep_host.cpp](../../package/PianoDwc3/tests/usb_sleep_host.cpp) | Audio pending MEM_MAP/quiet/drain timeout, retained DMA, amp failure and HAL reanchor; camera cancellation/start arbitration; document absent Iris PM rather than simulate an imaginary callback; USB clock-off deferral and disconnect/keep behavior. |

For example, `python3 scripts/ci/self-tests.py --only PianoPower` selects the existing charging suite;
`--only PianoDpu`, `--only PianoSensors` and the other package substrings select their respective existing tests.
Passing host tests establishes modeled state transitions and bounds only. It cannot establish live PDC routing,
autonomous DMA quiescence, PLL lock, panel PPS retention, an airportd autojoin decision, or hardware power consumption.

The one-boot batch can close observation cells only for the branches it actually exercises. A later focused RAM
qualification must cover ordinary and aborted/cancelled wake, both lid/power wake sources, outstanding storage/media
work and repeated cycles before claiming the entire path qualified. That work is not authorized or performed by this
document. Keep independent conclusions for root wake, M5 optical wake, B13/rotation, charging handover and native Wi-Fi
reconnect; none substitutes for the others.

## Fix-image gate

Status: HOLD

Analysis-only: no code change or fix image is proposed by this document. Daily sleep/display/charging policy remains
unchanged. Outstanding gates are confirmed hardware quiescence before PM acknowledgement, M5 DSI/PHY and bright-source
evidence, native Wi-Fi same-wake auto-reconnect, fresh post-wake battery/B6 samples, calibrated ALS/orientation, storage/media boundary coverage and
shared-vote/quiet interactions. A proposed implementation needs a supported failure mechanism and the corresponding
host and RAM evidence before this gate can change.

**OWNER-0748 (2026-11-27) supersedes the earlier BH-2 analysis deferral.** Host-side 27.3 `powerd` analysis is approved;
the [storage trace and Fix-image gate](power-bh2-powerd-analysis.md#fix-image-gate) remain HOLD for the underlying failed operation.
This battery-health persistence failure is separate from 80/75 charging policy and supplies no authorization for deployment
or tablet access.
