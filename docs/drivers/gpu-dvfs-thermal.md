# Adreno 830 DVFS, thermal guard and Energy Mode budget

Analysis date: 2026-11-27. Target: Xiaomi Pad 8 Pro (`piano`, SM8750). Source baseline:
`7e17a4baa3e0df3a5b9f3f0ee2a14f178f7d7093`. This is static analysis and an observation proposal;
no tablet access, image build, policy change or installation is part of this work.

The installed `v13.1y12q` ten-minute Metal run supports smooth **967–1050 MHz** operation under its recorded
conditions. The full table reaches **1100 MHz**, but temperature and independent ceilings decide how much is available.
The default GPU skin table is **docked even on battery**: source changes and Energy Mode do not select another table.
There is no shared watt budget for GPU, CPU, display and charging in the inspected policy path. Those two gaps prevent
extending the installed result to battery-only high-refresh sustain or simultaneous fast charging.

## Evidence and reference boundary

The current [GPU status](gpu.md#daily-status-2026-11-27), [thermal status and sensor faults](thermal.md#sensor-faults),
[installed sustain results](../results-2026-11-27.md#sustained-metal-3d-installed-v131y12q) and
[benchmark contract](../tools/metal-sustain.md) supply historical hardware evidence. Older bring-up sections in `gpu.md`
and `thermal.md` describe earlier 342/607 MHz ceilings, 70/60 °C rules and pending tests; they are not the fullopp daily
contract. Current source citations below take precedence for the static behavior. A source check is not a new device pass.

The Ubuntu reference is read from Git objects at **`7f8d6bedde479a01f4a05d6d0581b43c7fd20a01`**, independently of the
reference checkout's current HEAD or working files. Paths in the following table are relative to that kernel repository.
Linux is a behavioral specification only; no GPL implementation is reproduced here. The Android scene numbers in our
headers are a separate recorded stock-policy input, not a policy implemented by the Ubuntu board's ADC thermal zones.

| Pinned Ubuntu path / symbol or node | Comparison with our path |
| --- | --- |
| `arch/arm64/boot/dts/qcom/sm8750.dtsi`: `gpu`, `gpu_opp_table`, `gmu`, `gpu_speed_bin` | GPU OPPs have frequency, RPMh level, peak bandwidth, ACD data and supported-hardware masks. GMU clock OPPs are 500/650 MHz. Our board table carries corresponding facts, with KGSL-style bus indices and its own bin selection. |
| `drivers/gpu/drm/msm/adreno/a6xx_gpu.c`: `a6xx_set_supported_hw`, `fuse_to_supp_hw`; `adreno_gpu.c`: `adreno_read_speedbin` | Linux filters OPPs by fuse-derived supported hardware. Our `A830Votes::selectLevels` uses the board's bins; numeric bin IDs from the two representations must not be equated without their mapping. |
| `drivers/gpu/drm/msm/adreno/a6xx_gmu.c`: `a6xx_gmu_pwrlevels_probe`, `a6xx_gmu_rpmh_arc_votes_init`, `a6xx_gmu_rpmh_dep_votes_init`, `a6xx_gmu_acd_probe`, `a6xx_gmu_set_freq` | Both construct firmware tables and send a performance/bandwidth index rather than directly programming a GPU PLL per demand decision. Linux records an idle target without waking the device and drops its initial bus vote after GMU bandwidth ownership starts. Our host-floor and HFI profiles are explicit, separate contracts. |
| `arch/arm64/boot/dts/qcom/sm8750.dtsi`: `thermal-zones`, `tsens0`–`tsens3`; `sm8750-xiaomi-piano.dts`: CPU trip and passive-poll overrides | Four controllers and 47 sensors; GPU cooling uses TSENS2 sensors 1–8. Board overrides make the selected CPU hotspot passive polls 10 ms. Our CPU fullopp 95/87 °C rule and GPU 95/90 °C ladder are native policy, distinct from GPU DT passive/critical trips of 105/125 °C. |
| `drivers/thermal/qcom/tsens.c`: `get_temp_tsens_valid`, `tsens_hw_to_mC`, `tsens_set_trips`, `tsens_suspend`; `tsens-v2.c` | VALID retry, signed temperature decode, threshold handling and suspend masking are reference contracts. Our polling and fault coverage must be assessed separately from cached diagnostic temperatures. |
| `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts`: `thermal-sensor-*`, `pmk8550_adc`, board `thermal-zones` | Eleven ADC thermistors; the board zones have no mitigation trips. They do not themselves implement a virtual skin governor or a source-dependent GPU cap. |
| `drivers/iio/adc/qcom-spmi-adc5-gen3.c`: `adc5_gen3_configure`, `adc5_gen3_do_conversion`; `qcom-vadc-common.c` | ADC conversion transport, Gen3/Gen4 scale and resistance conversion inform our independently derived NTC readout. ADC sampling requires conversion writes; reading the published snapshot does not request another conversion. |
| `drivers/cpufreq/scmi-cpufreq.c`: `scmi_cpufreq_set_target`; `drivers/firmware/arm_scmi/perf.c`; `drivers/mailbox/qcom-cpucp-mbox.c`: `qcom_cpucp_mbox_send_data` | CPU clocks belong to CPUCP/SCMI performance domains. Linux distinguishes a frequency request from the eventual physical transition. Our acknowledged LEVEL_SET and later LEVEL_GET likewise do not constitute a per-core clock measurement. |

## Complete ladder and GMU contracts

The piano-specific source is [PianoGpuInputs.h](../../package/PianoA830/board/PianoGpuInputs.h),
`PIANO_GPU_BIN2_LEVELS`, `PIANO_GPU_BIN1_LEVELS`, `PIANO_GPU_DDR_KBPS`, consumed through
[PianoA830Board.cpp](../../package/PianoA830/board/PianoA830Board.cpp), `fillBoard`, `bootArgument`, and
[A830Votes.cpp](../../package/PianoA830/adreno830/A830Votes.cpp), `selectLevels`, `build`.
The shared platform [Sm8750GpuLevels.h](../../package/PianoA830/adreno830/platform/sm8750/Sm8750GpuLevels.h)
is useful corroboration, but the piano board's generated input is authoritative for this board. In particular, its bin 1
contains 13 distinct points to 1050 MHz; the shared platform's bin 1 retains a duplicate 1050 MHz entry.

The recorded piano fuse `0xe8` selects bin 2: **14 ascending active levels**. GMU level 0 is off, and active level `g`
indexes table entry `g-1`. The common prefix ends at 900 MHz; a successful fused/full-table selection is necessary for the
higher levels. The table below gives requested corners and bus levels, not measured voltages, watts or memory throughput.

| GMU level | GPU MHz | Requested RPMh vlvl | DDR min / nominal / max | Nominal DDR kB/s | ACD datum |
| --- | ---: | --- | --- | ---: | --- |
| 1 | 160 | `0x32` | 2 / 2 / 2 | 2,136,718 | none |
| 2 | 222 | `0x34` | 2 / 2 / 2 | 2,136,718 | none |
| 3 | 342 | `0x38` | 2 / 3 / 3 | 5,285,156 | none |
| 4 | 389 | `0x3c` | 2 / 4 / 6 | 6,074,218 | `0xc8285ffd` |
| 5 | 443 | `0x40` | 2 / 4 / 6 | 6,074,218 | `0xc02e5ffd` |
| 6 | 525 | `0x50` | 2 / 4 / 6 | 6,074,218 | `0xc02e5ffd` |
| 7 | 607 | `0x80` | 4 / 6 / 8 | 8,171,875 | `0xa82e5ffd` |
| 8 | 660 | `0x90` | 4 / 6 / 8 | 8,171,875 | `0x882c5ffd` |
| 9 | 734 | `0xc0` | 6 / 8 / 10 | 12,449,218 | `0xa82a5ffd` |
| 10 | 832 | `0xe0` | 7 / 10 / 10 | 16,500,000 | `0x882a5ffd` |
| 11 | 900 | `0x100` | 7 / 10 / 10 | 16,500,000 | `0x882a5ffd` |
| 12 | 967 | `0x140` | 11 / 11 / 11 | 18,597,656 | `0x882a5ffd` |
| 13 | 1050 | `0x1a0` | 11 / 11 / 11 | 18,597,656 | `0xa8295ffd` |
| 14 | 1100 | `0x1c0` | 11 / 11 / 11 | 18,597,656 | `0x88295ffd` |

Bin 1 ends at level 13/1050 MHz. Other platform bins contain 1150/1200 MHz; neither belongs to this piano bin-2 ladder.
The pinned Ubuntu OPP table matches these frequency/corner/nominal-bandwidth facts for the applicable hardware mask,
including the separate 900 MHz bandwidth row for a 900 MHz maximum SKU. It does not encode our three bus bounds per point.

[PianoGpuAndroidPreset.h](../../package/include/PianoGpuAndroidPreset.h), `kAndroidCurve`, `presetActive`, supplies the
default active-driver curve: R3b daily/fullopp, governor 2, fused table, max level 14, 1100 MHz maximum, **222 MHz demand
floor**, ACD enabled, HFI profile options 0, rails enabled and host DDR boot/floor 11. Thus 160 MHz exists and remains the
safety floor even though ordinary preset demand starts at 222 MHz. Any explicitly supplied preset argument disables the
whole preset; partial experimental arguments cannot be assumed to inherit the other defaults.

At S0, `A830Votes::build` parses command-db resource tables. `arcVote` rounds the requested corner up to an available
primary `gfx.lvl` corner, then selects the secondary `gmxc.lvl` (or non-dummy fallback `mx.lvl`) against that rounded
corner. The vote encodes corner and both indices. Consequently `0x1c0` can round to `0x1c4` when that is the available
corner; it is not a literal voltage. `dependencyVote` independently derives the MX index; the optional BX dependency uses
the rounded GX corner. `voteTables`, `perfMessage`, `bwMessage`, `acdMessage` construct GX/dependency, GMU CX/MX,
SH0/MC0/ACV and CN0 tables and ACD enable/data messages. GMU clocks are 500 MHz at `0x40` and 650 MHz at `0x80`.

[A830Hfi.cpp](../../package/PianoA830/adreno830/A830Hfi.cpp), `A830Hfi::start`, `perfVoteLocked`, sends the boot tables and
M9 performance vote. A runtime vote is an acknowledged `GX_BW_PERF_VOTE` containing GPU level and bandwidth word. HFI
profiles differ in no-AB encoding; `A830Votes::bwWord` and `A830Dcvs::vote` must agree with the running profile. ACD must
actually initialize and be acknowledged: `A830Dcvs::setAcd(false)` clamps to **734 MHz** despite a higher requested maximum.
Firmware tables persist across slumber/recovery; a demand vote does not construct a new rail table.

## Closed-loop path and guard ownership

The demand loop is in [A830Dcvs.cpp](../../package/PianoA830/adreno830/A830Dcvs.cpp), `window`, `decide`, `wanted`,
`ceiling`, `steer`, `vote`, `serviceTick`. `Adreno830Start.cpp` configures governor 2 with **10 ms timer windows**, an
**80 ms idle rule**, and no separate 734 MHz sustain ceiling (`sustainKhz=0`). The GMU busy-counter delta and elapsed
window produce busy permille. Demand is current kHz × busy / 750. At 900 permille busy the upward decision takes at least
two levels, with a 1.25 demand margin; ordinary increases use 1.10. Decreases require 4 windows, or 6 after a raise, with
the configured down margin. The accumulated 50 ms busy-burst rule can request the top. Input/wake/launch floors remain
requests under every ceiling. These constants are native KGSL-style behavior, not proof of an identical stock governor.

```text
Metal submissions + GMU busy counter
  -> demand policy / boost request
  -> minimum of knob-and-ACD, thermal snapshot, external slots, boost cap
  -> power-state qualification/lease clamp
  -> clamp again after power lock and immediately before HFI enqueue
  -> acknowledged level + DDR vote -> busy/temperature/residency feedback

TSENS2 gpuss0..7 + fresh board msm/quiet + virtual skin/XO
  -> independent 100 ms guard -> atomic cap + 250 ms permit lease
PianoThermal all-zone protection -> external slot 0
Energy Mode through PianoPolicy -> external slot 1
```

[PianoGpuCeiling.h](../../package/PianoA830/board/PianoGpuCeiling.h), `CeilingSlots::set`, and
[PianoA830.cpp](../../package/PianoA830/board/PianoA830.cpp), `ceilingSet`, own four independent ceiling slots. Slot 0 is
PianoThermal, slot 1 the power profile, slot 2 the optional sweep, slot 3 spare. The lowest nonzero kHz wins; clearing one
cannot clear another. `adreno830_dcvs_set_ceiling` wakes the demand loop and updates the qualification external clamp.
`A830PowerState::limitQualification` in [A830PowerState.h](../../package/PianoA830/adreno830/A830PowerState.h) also bounds
targets, wakes and restart votes. `A830Hfi::perfVoteLocked` checks again after mutex waits, immediately before enqueue.

The independent owner is [Adreno830Start.cpp](../../package/PianoA830/adreno830/Adreno830Start.cpp),
`qualificationThread`, driving [A830Qualification.h](../../package/PianoA830/adreno830/A830Qualification.h), `tickR3b`,
`checkLease`, `sample`, and [A830R3bThermal.h](../../package/PianoA830/adreno830/A830R3bThermal.h), `tick`, `skinTick`.
It polls all eight GPU zones every 100 ms, fetches msm/quiet every round, and refreshes the skin API snapshot every ten
rounds (nominally one second). Two distinct fresh, advancing board rounds and a healthy, cool GPU permit daily automatic
arming. Before opening, the cap cannot exceed 342 MHz; missing health can require 160 MHz. Daily mode has no practical
soak expiry, but retains the short heartbeat lease and sticky fault latch.

| Fullopp die/XO condition | Action in the guard |
| --- | --- |
| Raw GPU ≥ 95 °C or filtered GPU ≥ 95 °C, while running and healthy | Step down one GMU level per ≥ 500 ms, down to level 3. Filter update is one-quarter of the difference at each eligible round. A late round does not catch up with multiple drops. |
| Healthy, running, valid skin, raw < 95 °C and filtered ≤ 90 °C | Recover one level after 2 s continuously cool, up to the qualified top, still under skin/external limits. |
| Raw GPU ≥ 100 °C; incomplete/implausible GPU sample; stale/invalid msm or quiet; expired heartbeat | Revoke high operation; sensor/temperature faults use the low transaction and sticky qualification path. Cooling alone does not re-arm high levels for that boot. |
| Any valid raw GPU zone ≥ 105 °C | Independent unsafe exit, before board lookup/filter/HFI. It is not the ordinary soft ladder. |
| XO ≥ 78 °C / release < 70 °C | Cap to level 9/734 MHz or a lower skin/die cap. |
| Valid XO ≥ 90 °C | Hard guard exit. Legacy msm 62/65 °C and quiet 37/40 °C hold/exit thresholds do not apply in fullopp. |

These are software conditions, not guaranteed physical response deadlines. The daily path explicitly disables the
qualification-only five-second protection-deadline unsafe exit (`A830Qualification::deadlineMissed`); the raw-105 exit
and PianoThermal critical path remain. Scheduling stalls, a wedged firmware/transport, and actual low-vote effectiveness
still require evidence.

### Docked and mobile skin bands

`A830R3bThermal::skinTick` uses the **virtual board-derived skin proxy**, not a calibrated enclosure surface reading.
`Perf.h::virtualSkin` computes 0.500 × battery + 0.131 × quiet − 0.009 × charger + 0.089 × Wi-Fi + 0.232 × msm + 1.6 °C.
All five inputs are required except the documented fresh DSP-held charger-pin case, which renormalizes the other four
weights to the original total weight. XO is an additional mandatory guard input.

| Table | Entry temperature → maximum GMU level (MHz) | Release rule |
| --- | --- | --- |
| 0, Android normal / mobile comparison | ≥ 25 °C → 13 (1050); ≥ 45 °C → 9 (734); below both unlatched triggers → 14 (1100) | First row clears strictly below 23 °C; second strictly below 42 °C. |
| 1, docked, default | ≥ 35 → 13 (1050); ≥ 45 → 12 (967); ≥ 47 → 11 (900); ≥ 49 → 10 (832); ≥ 51 → 9 (734); ≥ 53 → 7 (607); ≥ 55 °C → 6 (525) | Each band clears strictly below its trigger minus 2 °C: 33, 43, 45, 47, 49, 51, 53 °C. Before the first band, cap 14. The table intentionally skips level 8 at 53 °C. |

Both tables keep the same die ladder, XO limits and fault rules. They are not automatically tied to AC or battery.
`PianoA830::start` defaults `piano-gpu-skin-table` to 1; `sysctlGpuSkinTable` exposes
`machdep.piano_gpu_skin_table`. The agent and `pianopolicy::Engine` do not write that selector. Thus unplugging AC can leave
the 967 MHz band active at 45–46.9 °C, whereas **explicitly selected** table 0 would cap at 734 MHz. Energy Mode High Power
does not bypass either table; Low Power supplies a still-lower independent slot. Automatic source-to-table selection is
`ours-missing`, not a claim that the mobile table is absent.

### Incomplete TSENS/ADC coverage and recovery

[Tsens.h](../../package/PianoThermal/Tsens.h), `Monitor::poll`, retries VALID three times and separates fresh masks from
cached usable values (up to three stale polls for diagnostics). `Cluster.h::clusterThermal` requires **every** CPU cooling
zone to be fresh. `Governor::thermal` takes the slowest cluster level immediately on invalid coverage or cut, and starts
the normal recovery timer only with complete readings below release. The documented daily fullopp rule uses
`piano-cpufreq-hot=95`, down steps at the fast polling interval (default 10 ms) and recovery strictly below 87 °C, one
level per 100 ms. Fullopp overrides the release point/rate, not the configured hot threshold: `readArgs` defaults that
threshold to 90 °C if no argument supplies it. It is a separate CPU loop.

[Policy.h](../../package/PianoThermal/Policy.h), `PolicyEvent::gpuLimited`, limits the GPU on **any incomplete all-zone
poll**, including missing zones outside the GPU group. `Policy::update` prevents state relaxation on incomplete coverage
and confirms critical temperature only from fresh samples over at least two updates and 1 s. Defaults are warm 85 °C,
passive at each zone's 105/118 °C trip bounded to 5 °C below critical, and critical 125 − 10 = 115 °C; thus a 118 °C
passive zone is bounded to 110 °C with that margin. A missing sensor alone does not request shutdown.
At `piano-thermal=2`, `PianoThermal::protect` applies the configured passive GPU ceiling,
default **160 MHz**, in slot 0, or parks for the boot if reversible ceilings are unsupported; critical always parks and
may halt if shutdown is enabled. Level 1 is readout, so this all-zone actuation must not be assumed without its level gate.

[BoardTemps.h](../../package/PianoThermal/BoardTemps.h), `sampleAgeMs`, `usableSample`, `fillBoardTemps`, `fillGpuTemps`,
`fillSkinTemps`, and [PianoThermalAdc.cpp](../../package/PianoThermal/PianoThermalAdc.cpp), `sampleUs`, `hookSkin`,
`hookSkinTemps`, define the ADC boundary. All eleven channels in
[AdcChannels.h](../../package/PianoThermal/AdcChannels.h), `kChannels`, are range/enable/age checked. Ages use continuous
time, including suspend. TSENS controller counts are 15/7/16/9: CPU cooling zones occupy TSENS0 sensors 1–12 and TSENS1
sensors 1–4; GPU zones occupy TSENS2 sensors 1–8; TSENS3 includes NSP and DDR. The ADC map comprises `batt`, `charger`,
`conn`, `conn2`, `msm`, `quiet`, `xo`, `wifi`, `display`, `backlight` and `flash`. Battery/charger/conn/quiet use Gen4;
the other seven use Gen3. [PmicAdc.h](../../package/PianoThermal/PmicAdc.h) and
[Ntc.h](../../package/PianoThermal/Ntc.h) implement the bounded conversion and derived resistance/temperature curve;
Gen4's midscale `0x8000` must not be mistaken for Gen3's invalid-code marker.
Charging roles and CPU skin allow three periods (15 s at the usual 5 s period); GPU msm/quiet
and GPU skin allow two (10 s). The GPU guard additionally requires the board API period to equal 5000 ms and both sample
timestamps to advance for fresh rounds. An API returning successfully is not the same as valid data.

Missing skin or XO clamps to **734 MHz or the previous lower skin cap**, preserves hysteresis across table changes and
blocks upward ladder recovery; fresh valid evidence is needed to relax it. Missing msm/quiet or one of eight GPU zones
is stronger: it breaks health and revokes the permit. With CPU skin protection enabled, missing skin caps cpu0–5 at
748.8 MHz and cpu6–7 at 1017.6 MHz in every Energy Mode, and suppresses bursts/launch/input boosts. These fallbacks are
implemented, including partial-coverage handling; they are not an unimplemented feature merely because the heading says
“incomplete coverage.” Existing host fixtures include `thermal_host.cpp::testSensorFaults`,
`adc_host.cpp::testSnapshotFaults` and `test_a830r3b.cpp` table/fault cases. They were inspected, not rerun for this doc edit.

Residual limits: the GPU thread reuses its skin snapshot between API refreshes without rechecking its ages each 100 ms
round; detection of expiration therefore includes refresh/scheduling delay. The two-period ADC rule does not make skin
telemetry 100 ms fresh. Published temperatures can remain available after their validity expires. No supplied hardware
capture establishes fault-to-ACK timing for all missing-zone, suspend-age and table-change combinations.

[A830Recovery.cpp](../../package/PianoA830/adreno830/A830Recovery.cpp), `requestQualificationLow`, `runPending`,
`strictOff`, `recoverLocked`, handles the hard path: close high admission, attempt an acknowledged level-1/bus-2 vote
while active, or pin the next wake low if already asleep. Failed low voting escalates to recovery. **Strict qualification**
uses `strictOff`; **daily mode** uses the ordinary snapshot/GBIF halt/GMU force-off/restart path (`strictMode` explicitly
excludes daily). Daily does not take the same immediate unsafe exit on force-off failure, and can proceed to a restart
attempt; failed restart ends in Lost. This is a material difference when assessing physical protection evidence.
Qualification-enabled restarts use 160 MHz/bus 2; the sticky latch limits subsequent operation to at most 342 MHz even
when cool. A true HFI vote failure makes frequency unknown until low/off/restart evidence re-establishes it. Recovery
may replay innocent clients in daily mode; a third recovery inside the configured window (default 60 s) is final.
The independent raw-105 path reaches `PianoA830.cpp::adreno830_qualification_unsafe`, which requests PSCI SYSTEM_OFF
directly, then SYSTEM_RESET if it returns; it bypasses the normal filesystem/IOKit halt path. None of this proves a pending
low request has physically lowered clocks before acknowledgement. `sysctlGpuRecovery` exposes faults/recoveries;
a successful recovery is still a failed sustain qualification.

## Energy Mode transitions and the budget actually enforced

[piano-policy-agent.sh](../../tooling/native/piano-policy-agent/piano-policy-agent.sh), `energy_modes`, `write_mode`,
reads both powerd notify states and mirrors them to `machdep.piano_policy_lpm` / `machdep.piano_policy_hpm` at start and
on either notification. Its write order gives Low Power precedence when entering that mode. The two writes are ordered,
not atomic; unavailable sysctls are logged. The notify wait is event driven. Its separate `readpower`/`slim` background
Save state uses a 30 s debounce and 120 s minimum uptime; **that delay does not delay the GPU cap**. The production PAUSE
list is empty, so Save is not evidence of a measured background watt saving.

[PianoPolicy/Policy.h](../../package/PianoPolicy/Policy.h), `view`, `Engine::step`, and
[PianoPolicy.cpp](../../package/PianoPolicy/PianoPolicy.cpp), `gpuCall`, `perform`, route the request to
`piano-thermal-profile` and GPU slot 1. The following assumes acting policy, CPU governor/perf enabled, the fullopp path,
and the documented daily `piano-policy-profile=2` behavior (`lpmOnly`). An explicit configuration can be more restrictive.

| Energy Mode | CPU profile / scene | GPU slot 1 | Source implications |
| --- | --- | --- | --- |
| Low Power | Profile 1 / normal scene; battery caps normally ≤ 1785.6 / 1958.4 MHz, with skin, low-SOC, hard and die limits on top | 342 MHz when the GPU maximum is higher (`gpuNeeded`) | Same cap on AC and battery. Wins if both low/high flags are set. |
| Automatic | Profile 2 / normal scene, skin steps and bounded onset/launch/input boosts | Cleared (0) | With profile policy 2, Balanced on battery too. Under the older source-driven policy, battery/unknown selects Saver instead. |
| High Power | Profile 3 / fullopp nolimits scene: top request 3532.8 / 4320 MHz below 51 °C virtual skin; from 51 °C cap 748.8 / 1017.6, clear below 50 °C | Cleared (0) | Available on either source when PianoThermal advertises profile 3; otherwise Balanced fallback. GPU guard is unchanged. |

The actual CPU ceiling is the minimum in [Governor.h](../../package/PianoThermal/Governor.h), `ceilingFor`: allowed
sustained/top level, configured hard cap, effective source/profile power cap, scene/perf cap and thermal limit. For example,
the source defaults in `PianoThermal.cpp` are AC 3532.8/4089.6 MHz; a 4320 MHz High Power request cannot exceed a still-lower
configured AC cap. Record active configuration and firmware-reported levels rather than interpreting the mode name as a
guaranteed clock. `Perf.h::SocLimit` further caps CPU frequencies at ≤ 3% / ≤ 1% SOC, with 4% / 2% release points.

`PianoThermal::cpuApply` sends `scmi::levelSet` through [Scmi.h](../../package/PianoThermal/Scmi.h), `Channel::call`,
`describeDomain`, `levelGet`, `levelSet`: two firmware-discovered CPUCP performance domains, shared-memory serialization,
channel-0 doorbell, indexed performance levels. An above-sustained refusal disables that allowance for the boot; twenty
ordinary refusals stop further changes for that domain. A transport error is sticky in `Channel::lastError`:
`PianoThermal::cpuTick` then stops all further SCMI messages for the boot, leaving the clusters at their last state.
The logical slowest-level fallback cannot guarantee a physical reduction over a failed channel. Fullopp periodically
compares LEVEL_GET with the requested state while the channel works. Firmware power costs from DESCRIBE_LEVELS are
not a calibrated whole-device watt model or a GPU power cap.

Lid and source policy are independent. `Policy.h::view` requires an attached/known cover, sleep enabled and armed,
cover-open wake, and an open seen since boot before battery close can sleep. AC close normally locks only unless demand
sleep is explicitly opted in. The agent's `lid` invokes display sleep when IOPMrootDomain reports a closed cover that
does not cause system sleep. Display-off reduces rendering opportunity; it does not choose another GPU skin table.
Missing source information uses battery caps in source-driven mode, while an explicit Balanced/High profile can select
external-style CPU caps regardless of the measured source.

There is **no numeric Low/Auto/High watt allocation** in these modules. Their “budgets” are frequency/current ceilings,
scene hysteresis and workload demand. No common controller subtracts display power or charger loss from a GPU allowance;
AC connected also does not establish a sufficient PD contract, non-discharging packs or spare thermal capacity.

## What sustained installed operation proves

The published `inst-y12q-obs4` record identifies High Power (`powermode 2`), a 60 Hz daily build and an onscreen
1920 × 1280 workload with 257 draw calls per frame. Over 600.000 s it completed 35,985 frames, mean 59.98 fps,
60/60 undisturbed ten-second windows, minimum window 59.80 fps, worst-window completion p99 19.97 ms and zero stalls
over 50 ms. GPU execution p99 was 5.46–7.25 ms across windows. These are Metal completion measurements:
`presented=0` supplies no positive drawable presentation cadence.

There were 591 distinct sampled driver states: 108 at level 13/1050 MHz and 483 at level 12/967 MHz; the observed
**`peak_level=13`** is the sampled run maximum, not the full table's top. Virtual skin rose **41.2 → 45.8 °C**, ending
at docked `band=2`, `cap=12`; raw GPU die rose **70.6 → 84.1 °C**. The source-grounded explanation is:

1. The docked table allows level 13 from 35 °C, then level 12 from 45 °C, with hysteresis avoiding immediate oscillation.
2. The reported die trajectory stays below the 95 °C soft ladder. It gives no reason to attribute the final cap to die heat.
3. High Power clears the Saver slot but leaves every guard clamp in force. Demand can remain below the instantaneous cap.
4. `recoveries`, `jobs_failed`, `failures`, `watchdog`, `holds` and HFI vote failures stayed zero. No recovery was needed to
   produce the observed continuity.

This is consistency with the implemented loop, not a per-frame causal reconstruction. The diagnostic state is published
nominally once a second, so it does not establish every 10 ms vote, an independent physical clock trace, or the time at
which each frame encountered a changed cap. `machdep.piano_gpu_state` reports `at_us`, `lvl`, `mhz`, `cap`, `cap_mhz`,
`why`, `bind`, `want`, `table`, `band`, `step`, `ext`, skin/raw/filtered/XO temperatures, validity and latch/high state.
`A830Bind.h::a830BindWhy` says `demand` when voted level is below the cap; `bind` identifies the cap's owner.
The state formatter in `qualificationThread` omits the transient boost cap from its cap calculation, so a boost cap can
make this diagnostic less complete than the actual `A830Dcvs::ceiling`. Temperatures with invalid flags are not fresh proof.

The benchmark process recorded `METAL_SUSTAIN_END rc=0`, but its SSH wrapper timed out at 650 s and the separate remote
exit receipt was absent. Recovered frames, state, counters and console passed the reported numeric/fault/confound checks;
the original checked-in grader still needs that receipt for automated PASS. Published results do not pair this run with
an AC contract, per-pack temperatures/current or battery-only control. A docked table label alone proves none of those.

## High OPP, 120 Hz and charging interaction

The installed build remains 60 Hz. M3's prospective daily 120 Hz use is separate from already recorded RAM timing
experiments; neither should be presented as installed 120 fps qualification. At 3200 × 2136 × 4 bytes, scanout alone is
**3.281 GB/s at 120 Hz**, versus 1.640 GB/s at 60 Hz, before composition, render targets and other masters. This is a byte-rate
calculation, not measured DRAM traffic. `PianoA830.cpp::adreno830_board_idle` uses `A830IdleScanoutLevel` with twice the 120 Hz
active read rate and selects DDR level 5 (6,671,875 kB/s) as its default idle display floor; explicit floor arguments and
other shared votes can keep it higher. Levels 12–14 already request nominal DDR level 11; raising render frequency within
that group does not create another listed bandwidth step or prove extra fabric capacity.

[M3](dpu-m3.md), [bandwidth analysis](dpu-m3-bandwidth.md) and [120 Hz throughput](m120-throughput.md) distinguish
physical link timing, acknowledged interconnect votes, new-surface throughput and drawable feedback. The later RAM
throughput analysis reports about 51/s app cadence and level 12/967 MHz under a skin band despite 120 Hz timing, with
presentation still unresolved. Its capture and workload differ from the installed sustain run. An 8.33 ms refresh period
cannot be validated by the installed run's approximately 20 ms completion p99 or by a single GPU execution percentile.
Doubling actual new-frame work may raise GPU/CPU/DDR duty, shorten idle periods and heat the board sooner; it need not
double power. No inspected governor explicitly reserves a thermal budget when refresh rises.

On **battery only**, all input power comes from the packs. High/Automatic need not select Saver, and the docked table
can persist. Pack discharge heating and reduced runtime require direct current/temperature evidence; the skin proxy and
GPU die are not interchangeable with either pack temperature. On **AC/PD buck**, external energy can supply the system
and charge the packs together, while charger/conversion heat reduces board headroom. At 80% hold the input may continue
to supply the system without charging; that is a useful separate comparison, not thermally equivalent to active charging.
Installed evidence documents fixed 9 V/1.6 A, about 14.4 W input, and up to 5 V/3 A on a capable source. These input
ceilings are not available GPU watts after conversion loss, display/CPU draw and battery charging.

Keep **each pack strictly below 45 °C** as the observation's hard boundary. Stop before a rising trajectory reaches it;
do not deliberately test a thermal trip. The actual buck policy is not a universal exact-45 °C cutoff:
[ChargePolicy.c](../../package/PianoPower/ChargePolicy.c), `temps`, `hv_step`, `protection`, uses whole-degree conversion
and HV eligibility up to 45 °C, while its separate general health hot stop is 55 °C. Its CP policy exits at 45 °C.
Therefore this stricter observation boundary cannot be delegated to an assumed buck shutdown at 45.0 °C. Require fresh
raw per-pack and battery-NTC telemetry and retain the warmer reading when they disagree.

The **80/75% charge hold** remains invariant (`piano_charge_soc_hold`, `protection`): higher pack SOC at 80% stops
charging, 76–79% retains hold, at/below 75% can resume through the other gates, missing SOC cannot authorize resume.
**CP/MiPPS remains off**: `PianoPower.cpp::readArgs` defaults `piano-power-cp=0`; do not arm a pump for GPU qualification.
The existing fixed-PD buck path is the AC comparator. PPS buck and direct-pump paths have separate pending qualifications
in [charging policy](charging-policy.md#daily-status-2026-11-27).

Charging already provides some thermal feedback of its own: `board_check` retains connector/charger hot holds across
missing readings (60/50 °C connector, 85/75 °C charger trigger/release); `pps_thermal_cap` reduces the prospective PPS
allowance without fresh cool connector, charger and skin evidence. PPS skin ≥ 42 °C removes the higher cap and ≥ 44 °C
stops PPS until below 42 °C. Those optional PPS rules are not a fixed-PD GPU budget, nor permission to enable PPS.
At the sustain run's 45.8 °C proxy, the PPS rule would already be restrictive if that separate path were active.

## Path & Check Matrix

`ours-OK` means the named native path exists and its stated contract is supported by source (hardware evidence is named
separately). `ours-missing` means an identified integration/control/evidence interface is absent. `unknown` means the
available source or published capture does not establish the property. No status here authorizes an image change.

| Stage / check | Status | Exact source anchor and evidence boundary |
| --- | --- | --- |
| Board identity and full 160–1100 MHz table | `ours-OK` | `PianoGpuInputs.h::PIANO_GPU_BIN2_LEVELS`; `A830Votes.cpp::selectLevels`; `gpu.md` records fuse `0xe8`. Bin/table selection is not a live clock measurement. |
| 1050 MHz bin versus 1100 MHz bin and active-level numbering | `ours-OK` | `PianoGpuInputs.h::PIANO_GPU_BIN1_COUNT`, `PIANO_GPU_BIN2_COUNT`; `A830Votes.cpp::copyLevels`; level 0 is off. |
| Default curve, 222 MHz demand floor, safety floor 160 MHz | `ours-OK` | `PianoGpuAndroidPreset.h::kAndroidCurve`, `presetActive`; `A830PowerState.h::limitQualification`. Explicit args disable the whole preset. |
| GX/CX/MX/BX and DDR/CNOC firmware contracts | `ours-OK` | `A830Votes.cpp::arcVote`, `dependencyVote`, `voteTables`, `bwMessage`; command-db-derived rounding and separate GMU/host encoders. |
| ACD handshake and ACD-off ceiling | `ours-OK` | `A830Hfi.cpp::A830Hfi::start`, `acdLocked`; `A830Dcvs.cpp::setAcd`; no ACD means ≤ 734 MHz. |
| Demand measurement and bounded request | `ours-OK` | `A830Dcvs.cpp::window`, `decide`, `wanted`; `Adreno830Start.cpp::stageDcvs` configures governor 2. |
| Independent clients cannot clear each other's ceiling | `ours-OK` | `PianoGpuCeiling.h::CeilingSlots::set`; `PianoA830.cpp::ceilingSet`; thermal slot 0 / profile slot 1. |
| Cap remains across wake, power-lock waits and HFI enqueue | `ours-OK` | `A830PowerState.h::limitQualification`; `A830Dcvs.cpp::vote`; `A830Hfi.cpp::perfVoteLocked`. |
| Eight-zone die guard and two advancing board rounds | `ours-OK` | `A830R3bThermal.h::tick`; `A830Qualification.h::tickR3b`; `Adreno830Start.cpp::qualificationThread`. |
| Soft die ladder, hysteresis and independent raw hard exit | `ours-OK` | `A830R3bThermal.h::useAndroid`, `tick`; `A830Qualification.h::sample`, `tickR3b`; 95/90, 100 and 105 °C have different roles. |
| All 47 TSENS zones, fresh versus cached coverage | `ours-OK` | `PianoThermal/Tsens.h::kMap`, `Monitor::poll`, `allFresh`; `Cluster.h::clusterThermal`; `Policy.h::Policy::update`. |
| Missing CPU zone and incomplete all-zone GPU fallback | `ours-OK` | `Governor.h::thermal`; `Policy.h::PolicyEvent::gpuLimited`; `PianoThermal.cpp::protect`, gated by thermal level 2; `thermal_host.cpp::testSensorFaults`. |
| Eleven ADC channels and age across suspend | `ours-OK` | `AdcChannels.h::kChannels`; `PianoThermalAdc.cpp::sampleUs`; `BoardTemps.h::sampleAgeMs`, `usableSample`, `fillBoardTemps`. |
| GPU msm/quiet freshness, fixed period and lease | `ours-OK` | `BoardTemps.h::fillGpuTemps`; `A830R3bThermal.h::tick`; `A830Qualification.h::checkLease`; stale health revokes high operation. |
| Virtual skin, charger-pin exception and required XO | `ours-OK` | `Perf.h::virtualSkin`; `BoardTemps.h::fillSkinTemps`; `A830R3bThermal.h::skinTick`; `adc_host.cpp::testSnapshotFaults`. |
| Missing skin/XO cannot relax a previous lower band | `ours-OK` | `A830R3bThermal.h::skinTick`; retains previous lower skin cap across invalid input and table changes. |
| CPU skin failure across all Energy Modes | `ours-OK` | `Perf.h::skinFaultCap`; `PianoThermal.cpp::cpuTick` final invalid-skin clamp; `Burst::tick` requires valid skin. |
| Docked and mobile table implementations | `ours-OK` | `A830R3bThermal.h::Docked`, `skinTick`; `PianoA830.cpp::sysctlGpuSkinTable`. Bands are proxy temperatures. |
| Automatic AC/battery selection of GPU skin table | `ours-missing` | `PianoA830.cpp::start` defaults to 1; `PianoPolicy/Policy.h::Engine::step` emits profiles/ceilings; agent `energy_modes` writes LPM/HPM only. No source-to-table action in this path. |
| Powerd Low/Auto/High → profile and GPU slot | `ours-OK` | Agent `energy_modes`; `PianoPolicy/Policy.h::view`, `Engine::step`; `PianoPolicy.cpp::gpuCall`; installed mode plumbing recorded in `thermal.md`. |
| System Settings picker reaching the same mode states | `unknown` | `thermal.md` daily status reports `pmset` verification and pending hands-on picker evidence. |
| CPU scene/power/hard/die caps → CPUCP | `ours-OK` | `Perf.h::sceneTable`, `SocLimit`; `Governor.h::ceilingFor`; `PianoThermal.cpp::cpuTick`, `cpuApply`; `Scmi.h::Channel::call`, `levelSet`, `levelGet`. |
| Lid, measured source and display sleep | `ours-OK` | `PianoPolicy/Policy.h::view`, `Engine::step`; agent `lid`, `readpower`; observed physical close/wake behavior is a separate qualification. |
| Hard fault → low ACK or recovery/restart, sticky latch | `ours-OK` | `A830Recovery.cpp::requestQualificationLow`, `runPending`, `recoverLocked`; `A830Recovery.h::strictMode`; `A830PowerState.h::latchQualification`. Strict and daily off-failure handling differ. |
| Physical fault-to-low bound for every sensor/transport failure | `unknown` | `A830Qualification.h::deadlineMissed` disables the qualification-only deadline in daily mode; `A830Recovery.h::strictMode` excludes daily; `qualificationThread` refreshes skin less often than die. No complete hardware fault timing capture supplied. |
| Voted-frequency / cap / binding / fault visibility | `ours-OK` | `PianoA830.cpp::sysctlGpuState`, `sysctlGpuRecovery`; `Adreno830Start.cpp::qualificationThread`; `A830Bind.h::a830BindWhy`. State cap omits transient boost cap. |
| Installed ten-minute 967–1050 MHz continuity | `ours-OK` | `results-2026-11-27.md`, `inst-y12q-obs4`: 591 samples, peak level 13, zero faults/recoveries. Scope and absent wrapper receipt are explicit above. |
| Same workload sustained on battery and on confirmed charging AC | `unknown` | Installed published results omit paired source/contract/pack evidence. `PianoThermal.cpp::readPowerSource` alone reads connectivity, not spare watts. |
| 120 Hz physical timing → 120 new presented frames/s | `unknown` | `dpu-m3.md`, `m120-throughput.md`, `presented-time.md`; physical timing, producer completion and presentation are separate populations. |
| Shared GPU/CPU/display/charging watt allocator | `ours-missing` | `A830Dcvs.cpp::ceiling`; `Governor.h::ceilingFor`; `PianoPolicy/Policy.h::view`; `ChargePolicy.c::piano_charge_decide` enforce independent caps, without a common energy allocation. |
| CPU physical cap after SCMI transport failure | `unknown` | `Scmi.h::Channel::call`, `lastError`; `PianoThermal.cpp::cpuTick`, `cpuApply` stop messages/retain the last state on failure. No physical low-clock receipt follows from the governor's logical cap alone. |
| 80/75 hold and CP-off buck observation invariants | `ours-OK` | `ChargePolicy.c::piano_charge_soc_hold`, `protection`, `strict_voltage_ready`; `PianoPower.cpp::readArgs`; `charging-policy.md` installed status. |
| Pack < 45 °C during high-OPP + 120 Hz + active PD charging | `unknown` | `ChargePolicy.c::temps`, `hv_step`, `pps_thermal_cap` do not prove this combination; no paired capture establishes thermal equilibrium or margin. |

Paths abbreviated in the matrix resolve to the linked source directories above: `package/PianoA830/board/`,
`package/PianoA830/adreno830/`, `package/PianoThermal/`, `package/PianoPolicy/` and `package/PianoPower/`.
Host fixture paths are under each component's `tests/`. These are exact file/symbol citations, not inferred Linux
equivalents. `stageDcvs` and `cpuTick` are the respective startup/runtime integration points.

## Bounded read-only-first RAM observation checklist

This checklist is for a separately authorized future operator session. Nothing here was executed on the tablet.
Use an already qualified RAM image, existing console capture and the
[sustain workload contract](../tools/metal-sustain.md), preserving the installed image and its policy. No fault injection,
OPP sweep, raw MMIO/SPMI access, pump arming, refresh experiment or persistent mode/profile edit is needed for the baseline.

1. **Read-only inventory, 60 s maximum.** Record build/source identity, active thermal/governor/perf/fullopp arguments,
   selected Energy Mode, measured source, PD contract, charge/hold state, CP-off state, active display timing and existing
   console health. Read `machdep.piano_gpu_state`, `machdep.piano_gpu_recovery`, `machdep.piano_gpu_skin_table`,
   `machdep.piano_cpufreq_state`, policy LPM/HPM, `pmset -g custom` and `pmset -g ps`; inspect only the targeted
   PianoThermal/PianoPolicy/PianoPower registry properties. Do not publish complete registry dumps or identifiers.
   If a required safety/validity field is unavailable, stop the load proposal and record the gap.
2. **Freshness baseline, 60 s idle per source.** GPU state once a second, retaining `at_us` to distinguish repeated snapshots;
   capture existing TSENS valid/fresh masks and raw/filtered hottest GPU temperature, CPU hot/limit fields, board ADC
   state/rounds/valid/disabled masks, per-channel ages, skin input/used mask, virtual skin and XO. Keep both pack
   temperatures, battery NTC and charge/discharge current alongside source/contract/hold. Do not force ADC conversions.
   API or cached-value success is insufficient without advancing timestamps and valid masks.
3. **Separate source comparisons.** At fixed 60 Hz and fixed brightness/workload, plan one battery-only interval and one
   confirmed fixed-PD buck interval, each **at most 10 minutes**, with an initial **30 s smoke**. The battery interval must
   have no charging/debug cable supplying power; use an already available observation route. Keep the same table and
   Energy Mode initially so source is the comparison variable. Label `table=docked` on battery honestly. Allow at most
   **5 minutes unloaded cooling** between intervals; abandon the pair if temperatures do not return near the baseline.
   Never discharge or charge to chase an 80/75 threshold. Distinguish an already-held AC interval from active charging.
4. **Per-window checks and stop conditions.** Use ten-second frame windows and one-second state records, with before/after
   fault counters and bounded console markers. Retain level residency, `want`, `cap`, `why`, `bind`, table/band/step/ext,
   skin/XO/raw die, source and pack traces on the same monotonic timeline. Stop at any GPU error, stall watchdog,
   recovery/fault/vote-failure increment, lease/ADC/TSENS validity loss, unexpected latch, contract instability, or missing
   pack safety telemetry. Stop for **either pack or battery NTC approaching 45 °C**, raw GPU reaching the 95 °C ladder,
   or a charger/connector hold. Do not heat through a hard boundary to demonstrate fallback. Preserve the real process
   exit and wrapper receipt; grade recovered evidence with its limitations when either is missing.
5. **Energy Mode observation, separate ≤ 3 minute phase per source.** First observe any already selected mode read-only.
   Only in a session that separately authorizes temporary mode switching, use the normal Energy Mode control for
   Low → Automatic → High → original, allowing **≤ 30 s per state** under the same light workload. Preserve and restore
   the original source-specific settings; verify notification flags, policy profile, slot effect and actual clocks instead
   of writing driver sysctls. Low should constrain GPU to ≤ 342 MHz unless a lower cap binds; leaving Low must not clear
   thermal slot 0. High must retain die/skin caps. Confirm the table selector stays unchanged on source/mode transitions.
   A missing System Settings picker receipt remains unknown even when `pmset` plumbing is observed.
6. **Close and bound the report.** Restore the original mode, stop the workload and observe **60 s unloaded recovery**;
   compare counters and freshness, record the warmest pack and skin/die slopes, and stop after these two source intervals
   and the optional mode phases. No indefinite soak or automatic retry. Retain only sanitized relative artifact names and
   numeric summaries in public notes. Report Metal completion cadence separately from positive presentation timestamps.
   Reserve any table-0 comparison, 120 Hz combination or fault-injection validation for its own reviewed RAM protocol.

Expected thermal fallback behavior can first be checked in the existing host fixtures without inducing hardware faults.
A battery/AC comparison that ends because of heat or missing telemetry is useful evidence and does not justify changing
caps. The 60 Hz result alone cannot release a 120 Hz plus charging gate.

## Fix-image gate

Status: HOLD

Scope: Analysis only. No policy changes, source fixes, image builds, tablet operations, installation or push.

Path: Metal/GMU demand → intersected GPU ceilings → power/HFI clamp and ACK → independent TSENS/ADC guard;
powerd Energy Mode → agent → PianoPolicy → CPUCP profile and GPU slot; charging remains a separate guarded loop.

Checks: Source paths and installed 60 Hz sustain evidence support the bounded `ours-OK` rows. Automatic source-dependent
GPU table selection and a shared watt allocator are `ours-missing`. Battery-only versus charging sustain, complete physical
fault-response timing, pack margin and 120 Hz presented throughput remain `unknown`.

Next evidence: A separately authorized, bounded, read-only-first RAM comparison with fresh TSENS/ADC/pack telemetry,
source/contract attribution, cap ownership, mode transitions and complete workload receipts. Preserve pack < 45 °C,
80/75 hold and CP-off throughout. This document proposes no fix image and does not release any existing gate.
