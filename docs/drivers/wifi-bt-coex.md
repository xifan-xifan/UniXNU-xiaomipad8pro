# B9: WCN7860 Wi-Fi / Bluetooth coexistence analysis

Analysis date: 2026-11-27. **Fix-image gate: HOLD.** This is a source review and a future RAM qualification plan.
No tablet commands, radio changes, driver edits, firmware modifications or image builds were performed for this analysis.
The installed **v13.1y12q** remains the daily baseline, with working Wi-Fi `en0` and **`piano-wifi-ps=0`**.
Bluetooth's **`BT_PREFLIGHT READY` (32/32 after 48 s)** establishes controller/daemon readiness and the LE event-mask fix;
it does not qualify simultaneous traffic, pairing, HID reconnect or audio.
[Current Wi-Fi status](wifi.md#daily-status-2026-11-27) · [Bluetooth status](bluetooth.md#daily-status-2026-11-27) ·
[B9 backlog](../BACKLOG-hardware.md) · [Support matrix](../STATUS.md)

The source supports three distinct conclusions. Shared supply ownership is implemented. Station power save follows the
same basic WMI ordering as Linux, but is a traffic-idle policy, not a BT airtime reservation. Explicit host coex/PTA control
is absent in both inspected ath12k and PianoWifi; this does **not** establish that firmware coexistence is disabled.
One additional Linux host action **is** absent in PianoWifi: preserving the advertised dual-MAC configuration while clearing
asynchronous SBS. Its effect on piano's BT reception remains unmeasured.

## Source boundary and reproducibility

Darwin source baseline: `794278b5c96d196657fa8a6bc33171bee87684a0`, the starting HEAD of this analysis branch.
Local source links below refer to files unchanged by this documentation change. Existing hardware results are cited as
historical evidence; no new RF measurements are asserted.

The inspected `linux-sm8750-piano` checkout is at **`b5100d5f2e0bb80e1191d521a48596674608eb3d`** in
[yzddmr6/linux-sm8750-piano][linux-pin] (the public repository URL redirects to `yzddmr6/linux-sm8750`). All Linux
file/line references below use this revision unless explicitly marked otherwise. The inspected radio, power-sequencing
and piano DT files match that commit. This is a board reference fork, not a claim about every mainline ath12k version.
In this tree the requested hardware table is **`ath12k/wifi7/hw.c`**; there is no top-level `ath12k/hw.c`.

The older Ubuntu v1.4.0 pin recorded in [the toggle/rejoin analysis](wifi-toggle-rejoin.md#reference-comparison) is
**`83f0f89bed14a0cb7d582a26e72bced0387870f4`**. Comparing that revision with the inspected HEAD shows that `mac.c`,
`hci_qca.c`, `btqca.c` and `pwrseq-qcom-wcn.c` are unchanged. The important newer delta is
[commit `658c139dd9f3f51acd019545febb65f7a7956be2`][linux-sbs-change], dated November 19: it adds the WCN7860
asynchronous-SBS override in `core.c`, `wifi7/hw.c`, `wmi.c` and `wmi.h`. Do not attribute that override to the older release
pin. The DT also differs between those revisions; DT line references here describe the inspected HEAD.

The negative coex finding comes from searching **all** of `drivers/net/wireless/ath/ath12k/` for coex, BTCOEX, PTA and MCI,
then tracing WMI builders/callers, core startup, MAC start, vdev creation/start, station PS and WCN7860 hardware selection.
The corresponding BT review covers `hci_qca.c`, `btqca.c` and `btqca.h`, not proprietary firmware internals.
On our side, the review covers `package/PianoWifi/`, `package/PianoBluetooth/`, `package/include/PianoWcnRails.h`,
the Apple80211 front end and the tracked macOS 27.3 Type0 adapter/patcher sources. It does not prove absence of private
coex machinery elsewhere in Apple's binaries. Linux is read as a specification only; no Linux implementation is copied.

## Linux WLAN: commands actually sent

The WCN7860 hw2.0 entry specifies two possible radios behind `single_pdev_only`, internal sleep clock, station PS,
idle PS, suspend and ASPM support. One exposed pdev does not mean one physical RF path.
[wifi7/hw.c:727–812][linux-hw]

WMI IDs below were evaluated from this revision's enums, including implicit increments;
`WMI_TLV_CMD(group)` is `(group << 12) | 1` ([wmi.h:64][linux-id-rule]).
An enum, TLV tag or advertised service is not evidence of a transmitted command.

| Mechanism | Exact identifier / value | What this reference does |
| --- | --- | --- |
| Dedicated coexistence configuration | `WMI_COEX_CONFIG_CMDID = 0x29003`; `WMI_TAG_COEX_CONFIG_CMD = 0x20a` | Declared in [wmi.h:643–647][linux-coex-ids] and [1741–1748][linux-mac-tags]. No builder, call site, WCN7860 enable sequence or payload/config-type definition was found in ath12k. There is no justified parameter value to transplant. |
| Coex services / reporting | Service numbers include `COEX_FREQAVOID=22`, `COEX_SAR=50`, `COEX_GPIO=85`, three-way legacy/override `202/203`; antenna-isolation and BT-activity tags also exist | ABI declarations in [wmi.h:2066–2250][linux-services]. No corresponding host PTA enable/configuration path was found. A firmware service bitmap would establish availability only. |
| Generic pdev / vdev setters | `WMI_PDEV_SET_PARAM_CMDID = 0x04003`; `WMI_VDEV_SET_PARAM_CMDID = 0x05008` | Ordinary configuration containers. No BTCOEX-enable, PTA wiring/priority or MCI parameter send was found. Do not invent `WMI_PDEV_PARAM_*` or `WMI_VDEV_PARAM_*` values from another ath family. [wmi.h:323–393][linux-setters] |
| Hardware mode | `WMI_PDEV_SET_HW_MODE_CMDID = 0x0401e`; SoC pdev `0`, mode `DBS=1`, no band-to-MAC entries | Sent after unified-ready for `single_pdev_only`. DBS permits dual-band operation; it is not a BT-priority switch. [core.c:987–1011][linux-core-start], [wmi.c:4392–4422][linux-hw-mode], [wmi.h:193–199][linux-mode-enum] |
| Dual-MAC configuration, newer fork delta | `WMI_PDEV_SET_MAC_CONFIG_CMDID = 0x0401f`; tag `0x204`; SoC pdev `0` | Immediately after hardware mode, WCN7860's `disable_async_sbs=true` invokes `ath12k_wmi_disable_async_sbs`. It keeps advertised concurrent-scan bits and all advertised firmware-mode bits except bit 26 (`0x04000000`), which is cleared. [core.c:1000–1011][linux-core-start], [wmi.c:4424–4463][linux-mac-config] |
| Idle device PS | Pdev parameter `WMI_PDEV_PARAM_IDLE_PS_CONFIG = 0x2c`, value `1` through `0x04003` | MAC start enables IMPS when `idle_ps=true`, as it is for WCN7860. Distinct from associated-station PS and from BT arbitration. [mac.c:9870–9878][linux-idle-ps] |
| Associated-station PS | `WMI_STA_POWERSAVE_PARAM_CMDID = 0x09002`, tag `0x06d`; `WMI_STA_POWERSAVE_MODE_CMDID = 0x09001`, tag `0x06c` | Uses a **vdev ID** in each payload. Despite the helper's name `ath12k_wmi_pdev_set_ps_mode`, it sends the STA command, not a pdev parameter. [wmi.c:1646–1674][linux-ps-mode], [1809–1839][linux-ps-param] |

The actual bring-up ordering is HTC/HIF and WMI connect → service-ready → REO setup → WMI INIT → unified-ready → DBS →
the newer async-SBS override → subsequent data-path setup. MAC start programs PMF_QOS (`0x22=1`), DYNAMIC_BW (`0x0a=1`),
ARP_AC_OVERRIDE (`0x23=0`) and MESH_MCAST_ENABLE (`0x8f=1`), alongside DFS/stats/monitor/data-path setup and idle PS.
These are ordinary WLAN operations, not a hidden BTCOEX command.
[core.c:924–1013][linux-core-start] · [mac.c:9756–9880][linux-mac-start]

The newer override takes both defaults from `WMI_SERVICE_READY_EXT_EVENTID`, stores them, and constructs the MAC-config
payload from those defaults. The exact numeric words depend on firmware and were **not** measured here. The helper's
return is not used to fail startup; a send failure leaves firmware defaults in place. The response event/tag are declared
but have no handler found in this tree. Thus static presence of the send does not establish firmware acceptance or improved
Bluetooth. SBS refers here to both MACs in the 5/6 GHz band; the code does not describe BT antenna switching.
[wmi.c:5110–5113][linux-mac-defaults] · [wmi.h:2552–2562][linux-sbs-bit] · [wmi.c:4428–4463][linux-mac-config]

### Station power save with BT active or idle

There is no BT-active/idle test in this path. Station policy comes from mac80211's `vif_cfg->ps` on `BSS_CHANGED_PS`, gated
by `supports_sta_ps`, and calls `ath12k_mac_vif_setup_ps`. On enable it sends parameter **3, INACTIVITY_TIME**, in milliseconds,
then PS mode **1**. It uses `conf->dynamic_ps_timeout`; if zero, it substitutes one beacon interval converted from TU to ms,
because firmware dislikes zero. On disable it sends mode **0**. The code does not select a universal 200 ms Linux default.
[mac.c:4522–4575][linux-ps-policy] · [mac.c:5119–5123][linux-ps-change]

At STA vdev creation the initial sequence is:

| STA PS parameter | Parameter ID | Initial value | Meaning |
| --- | --- | --- | --- |
| RX_WAKE_POLICY | `0` | `WAKE=0` | Wake for buffered receive traffic. |
| TX_WAKE_THRESHOLD | `1` | `ALWAYS=1` | Wake for transmit attempts. |
| PSPOLL_COUNT | `2` | `NO_MAX=0` | No maximum poll count; ignored with RX policy WAKE. |
| PS mode | command `0x09001` | `DISABLED=0` | Start the station with doze disabled. |
| INACTIVITY_TIME | `3` | Later policy-selected timeout | Written before enabling associated-station PS. |
| UAPSD | `4` | Per-access-category mask when configured | `ath12k_mac_conf_tx_uapsd` updates the mask and selects POLL_UAPSD or WAKE. No BT condition. |

Sources: [mac.c:10576–10611][linux-sta-init], [mac.c:8280–8317][linux-uapsd],
[wmi.h:4836–4875][linux-ps-values]. Vdev inactivity-count parameters are a different ABI from STA INACTIVITY_TIME.
No evidence here connects them to a BT-active PS override.

Doze can reduce WLAN duty cycle during quiet periods and let an AP buffer traffic. Sustained TX/RX or frequent probes can
keep the station awake even with PS enabled. Neither this source nor `PS_MODE=1` guarantees BLE scan windows, connection-event
latency or a fixed WLAN/BT airtime split. Firmware scheduling and RF performance need measurement in both traffic directions.

## Linux BT, firmware and shared power

### BT setup and the opaque configuration boundary

`hci_qca` identifies WCN7860, powers the controller, reads its SoC/ROM version, applies the WCN7860 peripheral baud sequence
and changes the UART rate before `qca_uart_setup`. The piano DT requests **3.2 Mbaud**. `btqca` loads
`qca/gngbtfw<rom>.mbn` as an ELF patch, waits 10 ms, queries the board ID and selects a `gngbtnv` board/ROM-specific NVM file
or its default according to the filename-selection rules. It downloads the NVM, disables logging, resets and reads build
information/address state. Actual firmware and NVM bytes were not decoded for coex semantics here.
[hci_qca.c:2012–2083][linux-bt-setup] · [btqca.c:826–939][linux-bt-download] ·
[btqca.c:952–1009][linux-bt-final] · [piano DT:2012–2023][linux-bt-dt]

| BT host operation | Exact command / tag | Coexistence interpretation |
| --- | --- | --- |
| EDL version, firmware segments, board ID, build info | Vendor opcode `0xfc00`; subcommands `0x19`, `0x1e`, `0x23`, `0x20` | Firmware provisioning and identification. Unknown whether the loaded firmware internally configures PTA. |
| UART speed | Vendor opcode `0xfc48`, plus WCN7860's raw peripheral-baud exchange | Host transport timing, not WLAN priority. [hci_qca.c:1342–1374][linux-bt-baud] |
| NVM patching | Tag `2`: read default address; tag `17`: set IBS bit 7 and baud code; tag `27`: set deep-sleep bit 0 | These are the only interpreted/modified NVM tag cases in `qca_tlv_check_data`. Other NVM content passes through; an unknown coex tag is not proved absent. [btqca.c:363–449][linux-nvm] |
| Address access | `0xfc0b`, subcommand `1`, tag `2`; WCN7860 can also use `0xfc14` address write | Identity configuration, not a coex command. [btqca.c:660–680][linux-bt-address], [btqca.h:8–34][linux-bt-ids] |
| Logging / shutdown | `0xfc17` subcommand `0x14`; pre-shutdown `0xfc08` | No explicit coex/PTA/WLAN-priority configuration was found in these host paths. [btqca.h:8–34][linux-bt-ids] |

There is no `hci_qca` WCN7860 branch that changes WLAN priority when scanning or connecting. Its RF-coex comment at
`hci_qca.c:1441–1449` explains **WCN3990** power pulses; it is not a WCN7860 programming sequence.
The BT firmware/NVM and WLAN firmware/board data may establish internal arbitration. ath12k's board-data download path
does not expose a documented coex parameter map either ([qmi.c:3238–3258][linux-bdf]). A known-good NVM filename or
successful download proves provisioning, not the contents or effectiveness of its RF policy.

### Rails, enables, clock and PCIe

At the inspected DT revision, the base piano DTS leaves the PMU and UART disabled; the desktop DTS enables the PMU, UART14,
PCIe0 and its PHY. Comments about older diagnostic variants are not evidence that the base DTS alone runs both radios.
[piano DTS:336–353][linux-pmu-dt] · [desktop DTS:277–291][linux-desktop-dt]

| Shared PMU input | Supply / nominal name | Linux pwrseq and our helper |
| --- | --- | --- |
| vddio | L3F, 1.8 V | Shared first input. |
| vddio1p2 | L2F, 1.2 V | Shared second input. |
| vddaon | S5F, nominal 0.88 V | Shared AON input; not a Wi-Fi-only rail. |
| vdddig | S4D, nominal 0.88 V | Shared digital/WLMX input. |
| vddrfa1p2 | S7I, nominal 1.31 V | Shared RFA input; supply name is not its exact voltage. |
| vddrfa1p8 | S3G, nominal 1.86 V | Shared RFA input. |

The DT PMU outputs for AON/RFA are referenced by both consumers. WLAN additionally references WLCX/WLMX/PCIe outputs;
BT references BTCX/BTCMX. Those output nodes identify the common sequencer; they are not six independent host-controlled
BT/WLAN regulator sets. The two **PCIe PHY** LDOs, L1F and L3G, are separate from these six PMU inputs.
[piano DTS:328–395][linux-pmu-dt] · [1553–1574][linux-wlan-dt] · [2012–2023][linux-bt-dt]

Linux `pwrseq-qcom-wcn` has a WCN7860 list of exactly those six inputs. Common regulator/optional-clock dependencies precede
the target's enable GPIO; the WCN7860 post-enable delay is **100 ms**. The core counts shared unit enables, so disabling one
target does not disable a dependency still used by the other. WLAN uses TLMM **GPIO16**; BT uses PM8550VE-F **GPIO3**.
`pci-pwrctrl-pwrseq` selects target `wlan` for PCI `17cb:110e`. `hci_qca`, when the BT node has no `enable-gpios` as on piano,
obtains target `bluetooth`; its older six-regulator device-data table is not the selected DT power path.
[pwrseq-qcom-wcn.c:78–250,434–448][linux-pwrseq] · [sequencing/core.c:800–876][linux-pwrseq-counts] ·
[pci-pwrctrl-pwrseq.c:49–67,125–128][linux-pci-pwrseq] · [hci_qca.c:2524–2561][linux-bt-pwrseq]

The sequencer supports an optional clock, but this piano PMU node declares **no clock or XO-enable GPIO**. WCN7860's
ath12k entry selects an internal sleep clock; QMI host capability sends that selection with host C-state/global-reset bits.
This does not prove that the package's RF XO is internally generated, nor document its physical sharing/gating.
BT's IBS UART clock voting, its 3.2 Mbaud serial clock and WLAN's PCIe reference clock are different mechanisms.
[pwrseq-qcom-wcn.c:550–558][linux-pwrseq-clock] · [qmi.c:2380–2395][linux-qmi-clock] · [wifi7/hw.c:738–741][linux-hw]

ath12k saves and clears PCIe L0s/L1 ASPM before firmware download. It restores the saved mask only on a supported chip and
the multi-MSI path; the single-MSI path deliberately leaves ASPM disabled to avoid MHI M2 problems. This is PCIe link power
management, not station PS, PTA or BT power-off. A supported flag alone does not establish live L0s/L1 residency.
[pci.c:911–957][linux-aspm] · [1159–1168][linux-aspm-start] · [1450–1469][linux-aspm-boot]

For suspend, this ath12k core drains TX, powers down HIF in suspend-late and powers it back up/restarts on resume. In contrast,
`hci_qca`'s normal suspend path exchanges IBS sleep and waits for RX asleep; that callback is not a blanket regulator-off.
Explicit BT power-off can disable its pwrseq target. Shared dependencies remain owned until the last requesting consumer
releases them. HIF power-down alone is not evidence of a WLAN target release or all PMU inputs turning off.
[core.c:156–253][linux-suspend] · [hci_qca.c:2734–2845][linux-bt-suspend] ·
[hci_qca.c:2343–2346,2398–2404][linux-bt-power]

## Our end-to-end path

`ours-OK` means that the named hop exists and has the stated static behavior; it is not a coexistence performance pass.
`ours-missing` means no implementation for the stated function was found. `unknown` marks a firmware, RF, daemon or
runtime boundary that this review cannot certify. An absent dedicated coex command is recorded even where Linux also
does not send one, without treating it as an established defect.

| Hop | Evidence and behavior at the Darwin baseline | Classification |
| --- | --- | --- |
| Common ownership words | [PianoWcnRails.h:53–61,480–528](../../package/include/PianoWcnRails.h) binds both kexts to `piano_wcn_rails_state` / `piano_wcn_pmu_owner`, defined in [PianoA830.cpp:666–669](../../package/PianoA830/board/PianoA830.cpp). WLAN bit 0, BT bit 1, UP bit 31; added-enable bits 8–15. | **ours-OK** |
| Refcount model and serialization | One idempotent owner bit per radio, not an unbounded get/put counter. Nested PMU lock holds are counted; cross-kext waits are bounded, normally 30 s. `get` verifies already-UP rails before adding an owner; otherwise votes/readbacks precede ownership. [PianoWcnRails.h:193–245,288–300,385–430](../../package/include/PianoWcnRails.h) | **ours-OK** |
| Supply programming | Six inputs in DT order, mode NPM, in-range voltage policy, enable, 5 ms settle and readback. Two shared-system PHY LDOs are read only. Initial vote detects lower/dropped rails; the already-UP fast path checks readable/enabled, not a complete voltage/status/analog-health invariant. [PianoWcnRails.h:94–102,288–380](../../package/include/PianoWcnRails.h) | **ours-OK** for sequencing; analog stability **unknown** |
| Wi-Fi ON | Outer PMU lock spans helper `get(WLAN)` and `powerUp`; `powerUp` repeats its six rail votes under that lock, then PERST low, CLKREQ mux, WLAN_EN low 150 ms/high, 50 ms + 10 ms settle and SW_CTRL read. It does not raise/lower BT_EN. [PianoWifi.cpp:6394–6410](../../package/PianoWifi/PianoWifi.cpp), [WifiBringup.h:371–394](../../package/PianoWifi/WifiBringup.h) | **ours-OK**; delay differs from the inspected Linux 100 ms |
| BT ON | PMU lock → helper `get(BT)` → validate EE0 BT_EN peripheral → configure/hold low 100 ms → high/readback → unlock → 50 ms + 10 ms settle. WLAN_EN/SW_CTRL are observed, not driven. [BtBringup.h:549–619](../../package/PianoBluetooth/BtBringup.h) | **ours-OK**; startup timing still needs paired RF qualification |
| Wi-Fi OFF with BT ON | Station/scan teardown precedes `safeOff` (WLAN_EN low, PERST asserted), release of Wi-Fi-only NoC vote and `put(WLAN)`. BT ownership prevents shared enables being withdrawn. [PianoWifi.cpp:5465–5477,6678–6688](../../package/PianoWifi/PianoWifi.cpp) | **ours-OK** |
| BT OFF with Wi-Fi ON | `safeOff` stops UART, returns pads to inputs, lowers its own BT_EN, withdraws its own GCC votes, then `put(BT)`. WLAN ownership retains the rails. The level-5 transport's power-off callback returns **ENOTSUP**; a Settings power-off indication is not proof of this physical path. [BtBringup.h:509–544](../../package/PianoBluetooth/BtBringup.h), [PianoBluetooth.cpp:1135–1144,1546–1561](../../package/PianoBluetooth/PianoBluetooth.cpp) | **ours-OK** for teardown; runtime BT-OFF control **ours-missing** on this callback |
| Last owner OFF | `put` removes only the caller bit. It withdraws only helper-added enables, only with no owners and RELEASE requested; voltage/mode and preexisting enables stay. Wi-Fi release defaults 0; BT defaults 1, additionally suppressing release when Wi-Fi is configured but absent from the owner word. A PMU-lock timeout drops the owner bit atomically and withdraws nothing. Both radios OFF therefore does not imply all rails OFF. [PianoWcnRails.h:435–464](../../package/include/PianoWcnRails.h), [PianoWifi.cpp:168,1304–1306](../../package/PianoWifi/PianoWifi.cpp), [PianoBluetooth.cpp:119,729–734](../../package/PianoBluetooth/PianoBluetooth.cpp) | **ours-OK**, conservative retention |
| s2idle sleep/wake | Default level-8 `piano-wifi-sleepoff=2` parks WLAN and restarts from level 2 on wake, preserving profile state; bounded park acknowledgement avoids powering off under the worker's MMIO. BT's power hook quiesces/releases BT and reruns its ladder on wake. Shared bits protect either callback order. `sleepoff=0` retains WLAN and has a historical firmware-death result, not a qualified coex alternative. [PianoWifi.cpp:3203–3233,6188–6210,6698–6727](../../package/PianoWifi/PianoWifi.cpp), [PianoBluetooth.cpp:1544–1579](../../package/PianoBluetooth/PianoBluetooth.cpp) | **ours-OK** for ownership path; joint reconnect/event delivery **unknown** |
| Firmware and board data | WLAN MHI/QMI firmware, board/regulatory/auxiliary data and BT ROM/board-selected patch/NVM are provisioned by independent paths. Shared rails do not compare the two firmware policies. [WifiWlfw.h](../../package/PianoWifi/WifiWlfw.h), [BtFirmware.c](../../package/PianoBluetooth/BtFirmware.c), [QcaCodec.h:480–591](../../package/PianoBluetooth/QcaCodec.h) | **ours-OK** for loader path; matching internal coex policy **unknown** |
| WLAN WMI bring-up | WMI INIT/ready → DBS mode 1 → six ordinary pdev params → regulatory channel list → STA vdev CREATE/scan. [WifiLink.h:348–365,418–443,494–540](../../package/PianoWifi/WifiLink.h), [WifiAssoc.h:225–272](../../package/PianoWifi/WifiAssoc.h) | **ours-OK** |
| Explicit WMI coex/PTA at bring-up or vdev START | No coex builder, command ID, PTA pins/priority configuration, MCI path or BT-active message to WLAN was found. Ordinary VDEV_START carries channel, streams, beacon/DTIM and flags, not a coex configuration. [WifiWmi.h](../../package/PianoWifi/WifiWmi.h), [WifiAssoc.h:317–352](../../package/PianoWifi/WifiAssoc.h), [WifiSta.h:464–479](../../package/PianoWifi/WifiSta.h) | **ours-missing**; also absent in inspected Linux, so necessity **unknown** |
| Newer Linux MAC-config override | No `0x0401f` MAC-config builder/send, retention of both advertised default words for this operation, or async-SBS bit-26 clear was found. DBS `0x0401e` alone does not perform it. [WifiHw.h:612,700–707](../../package/PianoWifi/WifiHw.h), [WifiLink.h:418–443](../../package/PianoWifi/WifiLink.h) | **ours-missing** relative to Linux HEAD, not the older Ubuntu pin |
| Idle device PS | No pdev `IDLE_PS_CONFIG 0x2c=1` send found in the six-entry [pdevParamPlan:256–272](../../package/PianoWifi/WifiAssoc.h). This is a separate Linux difference, not a missing BTCOEX enable. | **ours-missing**; relevance to associated BLE workload **unknown** |
| Station initial PS | After vdev CREATE and before START: TX/RX formats, NSS, RX_WAKE `0=0`, TX_WAKE `1=1`, PSPOLL `2=0`, PS mode 0; optional band TX-power limits follow. [WifiSta.h:458–479](../../package/PianoWifi/WifiSta.h), [WifiHw.h:948–956](../../package/PianoWifi/WifiHw.h) | **ours-OK**, matches Linux's initial PS values |
| Authorized-station PS request | Boot `piano-wifi-ps=0`; root writable `machdep.piano_wifi_ps` accepts only 0/1. With a live authorized port, `psTurn` sends INACTIVITY_TIME `3=200 ms` by default, then mode 1, or mode 0 on disable; a new association starts off. `psidle` accepts 1–10000 ms, otherwise returns to 200. [PianoWifi.cpp:315–321,3332–3347,4621–4652,5097](../../package/PianoWifi/PianoWifi.cpp) | **ours-OK** for ordering; firmware-applied state **unknown** |
| PS transport/measurement contract | Busy sends retry next turn. A negative send logs failure, resets the step and sets the local `applied` marker to the request to suppress further attempts until a change. No PS response/readback is used. A sysctl value is a request; even the success line is transport submission, not proof of doze residency. [PianoWifi.cpp:4636–4652](../../package/PianoWifi/PianoWifi.cpp) | Confirmed PS-state feedback/retry after error **ours-missing** |
| BT-dependent station PS/UAPSD | No coupling from BT scan/connection activity to `gPsWanted`, and no STA UAPSD parameter-4 programming found. Linux's PS callback also has no BT condition, but Linux has a separate UAPSD configuration path. | **ours-missing** for those policies; need for them **unknown** |
| BT NVM/transport sleep | ROM/board-selected `gngbtfw`/`gngbtnv` download; only address/HCI/sleep tags interpreted. Level 5 forces IBS off; deep sleep is cleared. This differs from Linux enabling IBS/deep sleep. Other NVM bytes survive unchanged. [QcaCodec.h:238–319,558–575](../../package/PianoBluetooth/QcaCodec.h), [PianoBluetooth.cpp:123–128,858–864](../../package/PianoBluetooth/PianoBluetooth.cpp) | **ours-OK** for current H4 transport contract; BT sleep-policy parity **ours-missing**, coex contents **unknown** |
| BT coex/PTA/WLAN priority | No explicit host coex command or cross-radio schedule in the BT setup/H4 path. Generic NVM loading may carry undocumented RF settings. [QcaCodec.h](../../package/PianoBluetooth/QcaCodec.h), [HciH4.h](../../package/PianoBluetooth/HciH4.h) | Host control **ours-missing**; internal firmware control **unknown** |
| BLE scheduling and event path | `bluetoothd` selects standard scan/create/update-connection HCI parameters over Type0 → `/dev/cu.BLTH` → H4 → GENI UART → controller. PianoWifi receives no scan window, connection interval, latency or supervision timeout from this path. LE event-mask OR covers bits 6/11; optional link-ready injection has separate counters. [Type0.c:79–164](../../tooling/native/piano-bluetooth/userland/Type0.c), [HciH4.h:139–238](../../package/PianoBluetooth/HciH4.h), [PianoBluetooth.cpp:1310–1336](../../package/PianoBluetooth/PianoBluetooth.cpp) | **ours-OK** for transport/mask; RF scheduling and actual HID delivery **unknown** |
| macOS coex hooks | Apple80211 power/scan/association handlers exist, but no BT-activity/coex handler routes to WMI. Apple80211 SET POWERSAVE (request 5) is accepted with no effect; the root PS sysctl is the implemented control. Type0 deliberately disables Apple's vendor scan extension, choosing supported standard HCI. No Apple coex bridge to this Qualcomm transport is implemented. [WifiApple80211.h:932–984](../../package/PianoWifi/WifiApple80211.h), [PianoWifi80211.h](../../package/PianoWifi/PianoWifi80211.h), [Type0.c:79–107](../../tooling/native/piano-bluetooth/userland/Type0.c) | Qualcomm coex bridge **ours-missing**; private daemon/IO80211 policies outside this path **unknown** |
| PCIe/sleep clock | QMI advertises internal sleep clock and required feature bits; no external sleep-clock pin is driven. Root-complex bring-up trains the link/sets target speed, but no Linux-style saved L0s/L1 disable/restore sequence was found. No BT_EN write is part of that path. [WifiHw.h:422–430](../../package/PianoWifi/WifiHw.h), [WifiBringup.h:400–623](../../package/PianoWifi/WifiBringup.h) | QMI selection **ours-OK**; ASPM policy parity **ours-missing**, live residency/XO interaction **unknown** |
| RF front end / antennas | Both functions share the WCN7860 package and PMU inputs. The reviewed source supplies no board antenna-switch schematic or internal RF/PTA arbitration table. A 5 GHz association also does not prohibit WLAN scans on 2.4 GHz. | **unknown** for exact physical sharing/isolation |

There is no basis to equate macOS's class name `IO80211Controller` with Apple's Broadcom hardware coex implementation.
Our front end is an IONetworkingFamily/Apple80211 compatibility path; it does not thereby inherit a Qualcomm RF coordinator.
The approved 27.3 daemon patch and Type0 adapter solve transport/capability/event issues. The tracked patcher
([native_macos27.py](../../tooling/native/piano-bluetooth/patcher/native_macos27.py)) contains no WCN7860 WMI coex operation.
No additional decoded-dyld claim is needed to establish the absent driver endpoint; complete private-framework behavior
would require a separate version-locked binary trace.

### What changes between 2.4 GHz and 5 GHz

BLE uses 2.4 GHz, so 2.4 GHz WLAN adds direct spectral contention as well as any shared-chip scheduling constraints.
5 GHz WLAN traffic does not occupy those same BLE channels. It still uses the shared package, supplies and firmware resources;
antenna switches, isolation/desense, MAC scheduling and PTA restrictions may still couple the functions.
**It is unknown whether this board shares a particular RF switch/LNA/PA/antenna path across the two bands.** The source does
not establish that 5 GHz uses the *same 2.4 GHz RF front end*, or that it shares *only* baseband/PTA. Both categorical claims
would exceed the DT and driver evidence. The distinction needs a schematic/vendor specification or controlled band/load data.

WLAN off-channel scanning is another variable. With the Apple80211 path enabled, `piano-wifi-bgscan` defaults to 120 s.
`bgScanTurn` copies the regulatory scan channel list, not only the associated band, and runs a passive scan.
A 5 GHz association therefore is not a clean “no 2.4 GHz WLAN activity” control unless scan intervals are identified.
[PianoWifi.cpp:337–341,5234–5268](../../package/PianoWifi/PianoWifi.cpp)

The historical [BT radio-health note](bluetooth.md#86-radio-health-bt-radio1-2026-11-26-host-tested-tablet-result-pending)
records mostly very weak advertisements, with stronger windows aligned to WLAN background scans, and Classic inquiries with
no results. That is evidence for a timed correlation, not proof of an antenna being monopolized or of a PS defect.
Ambient-device advertisements, differing peers, receiver power states, scan selection and software event delivery are
confounders. The old “shared rails/Wi-Fi coexistence” bring-up milestone established concurrent power/UART operation;
it did not certify Wi-Fi traffic plus BLE HID reliability.

### Interaction with Wi-Fi toggle/rejoin

[wifi-toggle-rejoin.md](wifi-toggle-rejoin.md) records a long OFF/ON failure whose retained trace reached fresh firmware,
vdev and scans but lacked a new airportd SET20 association. PS writes and radio toggles shared that run, but WMI PS=0 delivery
before teardown, daemon edge receipt, candidate admission and the helper's stale receipt remain distinct questions.
A PS write alone neither creates a new association generation nor grants native join ownership. Physical OFF/ON discards the
vdev and restarts from PS off; native rejoin still requires a fresh SET20.

Do not use “BT got stronger while Wi-Fi was off” as a pass if ON never regained an authorized station and DHCP. Do not combine
PS=0 and OFF in one command for the main coexistence comparison: first observe the PS submission while the same association
is live. The existing toggle reproducer intentionally preserves that historical race and belongs in its own capture.
Its fix-image gate is not cleared by this analysis.

## Future read-only and paired RAM qualification

This is a plan, not authorization to touch the tablet in this task. Use an **existing approved, frozen RAM image**, volatile
Data and USB management for any later run. Provision only the test network/peer in RAM. Record the image/source/firmware
identities, native-owner settings and initial scalar state; retain daily defaults, disk profiles, saved networks, bonds and
the installed image. No build or fix image is proposed here. Rebooting out of the RAM session discards its settings.

### 1. Read-only baseline before any runtime write

1. Reuse [BT preflight and session capture](../tools/bt-session.md): ready status, daemon/controller powered state, runtime
   hash match, actual LE event-mask bits, positive patch count and capture freshness. Capture transport error/backpressure
   counters and link-ready-assist/injection counters separately. Keep the assist at its daily default (off); injected
   length/PHY events cannot count as RF/controller-delivery success.
2. On WLAN read PS requested value, native ownership, current association generation, band/channel, negotiated PHY/width/NSS,
   RSSI, authorized-port state, fresh DHCP-lease presence, firmware restart count and existing TX/RX/retry/not-acked counters.
   Reduce interface/keychain reads to scalar facts. An old IP address or requested PS value is not proof of a live link or doze.
3. Align both radios' existing logs/counters to monotonic elapsed time and phase markers. Record only state/counts/timing and
   anonymous peer labels. Do not retain identifiers, raw packets, calibration, network names, credentials or private host
   paths in public results. Read rail owner/put/verify and power-transition logs already exposed; do not add MMIO/SPMI pokes.
4. Identify the known-positive BLE peer from existing observations, if available. Starting a scanner sends HCI commands:
   defer that active probe to the RAM matrix below. There, verify one authorized peer at a fixed position and TX setting.
   The existing [test peripheral](../../tooling/native/piano-bluetooth/le-test/le-peripheral.m) must be started in its
   authorized local GUI session, as documented in
   [Bluetooth §8.5](bluetooth.md#85-le-link-ready-2026-11-21-host-tested-tablet-result-pending).
   Plain advertising/RSSI observation requires no pairing. No known-positive or incomplete capture makes the cell invalid,
   not evidence of radio starvation.

### 2. Paired band / PS / load matrix

Use the same AP/test server and peer positions, legal fixed channels, similar RSSI, and the same WLAN PHY/width/security
where both bands support them. Verify actual association band after each transition; band steering must not silently change
the cell. Use memory-generated traffic to an authorized nearby server, not an Internet endpoint or disk throughput test.
The public record needs no server address. Hold background scanning and the Bluetooth runtime/mask configuration constant.

| Axis | Settings | Required observation |
| --- | --- | --- |
| WLAN band | 2.4 GHz; 5 GHz | Actual band/channel and PHY, not only AP configuration. |
| Station PS | 0 baseline; 1 trial; return to 0 | In RAM only, `sudo sysctl -w machdep.piano_wifi_ps=1` or `=0` on an authorized link; require the corresponding `level 8: power save ...` submission on an actual change and no send-failure line before timing. For initial PS=0 use the STA setup's PS-off submission; an idempotent 0 write need not produce a `psTurn` success line. Leave `psidle=200` throughout. Firmware doze remains unconfirmed without independent evidence. |
| BT workload | Powered but no intentional scan/connection; BLE advertisement observation | Distinguish daemon background activity from a truly idle controller through the HCI timeline. Use the same plain test peer for the scan cells. |
| WLAN workload | Quiet; low-rate ping; sustained uplink TCP; sustained downlink TCP | Rate/loss/latency and radio counters per phase. A further bidirectional phase can follow only after one-way captures are sound. |

This gives **eight settings** (two bands × two PS values × two BT workloads). For each setting, a proposed 180 s block is
45 s quiet, 15 s one-per-second ping, 60 s bounded uplink and 60 s bounded downlink, with one-per-second ping during load.
Quiet means no measurement ping: frequent pings themselves can prevent the 200 ms idle timeout. In the BLE cells, use bounded
`piano-bt-probe --le-rssi 60` windows during each traffic phase, with a separate quiet-period window, and mark their start/stop
gaps. This probe filters the known test service, requests duplicate advertisements and reports counts/RSSI without pairing.
Its supported duration is 5–120 s. `--scan 20` instead runs Classic inquiry and LE scan **sequentially**; it is not an isolated
BLE-only workload. [probe.m](../../tooling/native/piano-bluetooth/probe.m)

Repeat three matched blocks per setting, interleave PS **0 → 1 → 0** controls within each band, and alternate band order
between repetitions. Allow equal cooldown/settle periods and log temperature/power state so drift is visible. The 180 s block
can include a 120 s WLAN background-scan cycle; tag scan start/end and compare scan versus non-scan intervals separately.
If a valid block contains no scan, extend observation or repeat; do not force an extra scan in just one PS cell. Saturated
TCP may erase doze opportunities, so an idle-only PS benefit would not demonstrate a traffic-load coex fix.

For every phase retain aggregate throughput, ping loss and median/p95/max RTT, WLAN retry/not-acked/error and restart deltas,
BLE report count/rate, RSSI buckets and maximum report gap from the time-aligned event summaries if available. CoreBluetooth
callback counts, controller HCI report counts and radio reception are different observations: duplicate filtering, UART
backpressure, trace losses and daemon handling can change their relationship. Compare the same boundary on each side.
The existing `piano-bt-radio-*`, `piano-bt-h4-*` properties and console records provide address-free counts.

Before running, predeclare these provisional decision rules: a valid comparison has complete captures, the same intended
association/peer, confirmed PS submission and no unaccounted radio restart. Unexpected HCI reset, framing error, firmware
crash, sustained loss of the authorized link, or disappearance of the known-positive peer ends the load and fails that phase.
A repeatable >20% throughput loss relative to its matched control or a new multi-second BLE report gap warrants investigation;
these are proposed screening thresholds, not existing qualification results or a vendor guarantee. Keep absolute latency and
gap distributions, since aggregate throughput can conceal a missed BLE connection event. Stop on thermal-limit reports or
uncontrolled temperature rise, retain scalar evidence, and exit the RAM session if recovery is uncertain.

### 3. Connections, HID and power-transition checks

A non-bonding plain GATT test can be a separately agreed follow-up on volatile Data; do not substitute its success for HID.
For a connection capture, decode standard legacy/extended scan interval/window and scan type, requested create-connection
parameters, and **negotiated** connection interval, peripheral latency, supervision timeout and PHY. Scan values use 0.625 ms
units; connection interval uses 1.25 ms and supervision timeout 10 ms. Report the actual values, not assumed defaults.
[Bluetooth Core 6.0, HCI LE scan/create-connection parameters][bt-hci-spec]
Record command status → connection completion → remote features/length/PHY → first ATT/SMP/HID → disconnect reason.
An HCI `0x08` disconnection after connection and a failed/no connection completion are different failures.
[Session evidence boundaries and HCI specification](../tools/bt-session.md#read-the-capture)

**Real keyboard/mouse pairing and reconnect remain blocked on the owner's 20-minute hands-on session.** Use its existing
[checklist](../tools/bt-session.md), with preflight already ready. A focused proposed allocation is: 0–3 min preflight/markers,
3–9 min pair and check typing/click/scroll on the PS=0 control, 9–15 min repeat the same activity under bounded WLAN load
with one preselected band/PS comparison from the scan results, 15–18 min owner-operated reconnect, 18–20 min stop/capture.
If pairing fails, spend the remaining window capturing that first failure; do not change intervals, masks, NVM and PS together
or claim a coexistence comparison from an unpaired device. Audio is outside this HID qualification.

Physical Wi-Fi OFF/ON, BT-off/BT-only and s2idle checks belong in a **separate RAM session after the steady-state matrix**.
Only use already-supported controls. Wi-Fi toggles need the explicit physical-OFF arm and native SET20/authorization/DHCP
checks in [the toggle plan](wifi-toggle-rejoin.md#ram-boot-read-list). Since the BT transport rejects runtime power-off,
do not treat a daemon/UI off as physical BT_EN low; a BT-disabled/WLAN-only or WLAN-disabled/BT-only approved RAM profile is
the control if available. Require owner bits and own-enable transitions, zero withdrawal while the other bit is held,
continued activity on the surviving radio, and fresh readiness/link recovery after ON. At s2idle record both callbacks,
park acknowledgement, put/get ownership and both recovery timelines; do not infer order or rail collapse from a UI state.
Do not invent a new boot variant or change release/IBS/ASPM defaults to obtain these controls during this task.

Return PS to its captured initial value on exit from each experiment, stop traffic/probes and verify readiness, association,
authorization and DHCP again. Discard the RAM session if ON/rejoin fails; no installed-profile “repair” belongs in this plan.

### What evidence would select one future change

| Paired observation | Interpretation and next discriminating check |
| --- | --- |
| Reproducible improvement with PS=1 on the same live association | Supports an idle/duty-cycle contribution. Check whether it survives saturated TX/RX and reverses on returning to PS=0; it does not prove a missing PTA command or justify a daily default change yet. |
| Difference by band, with scan windows excluded | Supports spectral/isolation or band-dependent scheduling involvement. It does not identify a particular shared switch or antenna. Compare actual PHY, RSSI and traffic airtime. |
| Improvement confined to WLAN background scans | Supports the historical temporal correlation. Preserve band/MAC-state and HCI/daemon timing; scanning changes several RF/scheduling states simultaneously. |
| Independent runtime evidence implicates async SBS / advertised dual-MAC defaults | The newer Linux `0x0401f` sequence is a concrete future candidate, preserving advertised bits except bit 26. Source presence alone is insufficient. A separately reviewed single-change RAM trial would need field capture, transport/firmware evidence and a paired reversal. No such driver change is authorized by this report. |
| HCI event exists but daemon/HID delivery does not | Follow the version-locked event mask, link-ready, ATT/security and HID path before changing RF policy. Record synthetic-assist counters. |
| HCI framing/backpressure error, rail failure or firmware restart precedes degradation | Investigate that earlier boundary; good preflight and weak RSSI do not exclude transport/power failure. |
| Wi-Fi ON has scans but no fresh SET20 | Follow the separate airportd admission/receipt/edge analysis. Throughput on a disconnected station cannot establish coexistence. |

[linux-pin]: https://github.com/yzddmr6/linux-sm8750-piano/commit/b5100d5f2e0bb80e1191d521a48596674608eb3d
[linux-sbs-change]: https://github.com/yzddmr6/linux-sm8750-piano/commit/658c139dd9f3f51acd019545febb65f7a7956be2
[linux-hw]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wifi7/hw.c#L727-L812
[linux-id-rule]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.h#L64
[linux-coex-ids]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.h#L643-L647
[linux-mac-tags]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.h#L1741-L1748
[linux-services]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.h#L2066-L2250
[linux-setters]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.h#L323-L393
[linux-core-start]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/core.c#L924-L1013
[linux-hw-mode]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.c#L4392-L4422
[linux-mode-enum]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.h#L193-L199
[linux-mac-config]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.c#L4424-L4463
[linux-mac-defaults]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.c#L5110-L5113
[linux-sbs-bit]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.h#L2552-L2562
[linux-mac-start]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/mac.c#L9756-L9880
[linux-idle-ps]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/mac.c#L9870-L9878
[linux-ps-mode]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.c#L1646-L1674
[linux-ps-param]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.c#L1809-L1839
[linux-ps-policy]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/mac.c#L4522-L4575
[linux-ps-change]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/mac.c#L5119-L5123
[linux-sta-init]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/mac.c#L10576-L10611
[linux-uapsd]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/mac.c#L8280-L8317
[linux-ps-values]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/wmi.h#L4836-L4875
[linux-bt-setup]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/hci_qca.c#L2012-L2083
[linux-bt-download]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/btqca.c#L826-L939
[linux-bt-final]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/btqca.c#L952-L1009
[linux-bt-baud]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/hci_qca.c#L1342-L1374
[linux-nvm]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/btqca.c#L363-L449
[linux-bt-address]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/btqca.c#L660-L680
[linux-bt-ids]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/btqca.h#L8-L34
[linux-bdf]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/qmi.c#L3238-L3258
[linux-pmu-dt]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts#L328-L395
[linux-wlan-dt]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts#L1553-L1574
[linux-bt-dt]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts#L2012-L2023
[linux-desktop-dt]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-desktop.dts#L277-L291
[linux-pwrseq]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/power/sequencing/pwrseq-qcom-wcn.c#L78-L448
[linux-pwrseq-counts]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/power/sequencing/core.c#L800-L876
[linux-pci-pwrseq]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/pci/pwrctrl/pci-pwrctrl-pwrseq.c#L49-L128
[linux-bt-pwrseq]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/hci_qca.c#L2524-L2561
[linux-pwrseq-clock]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/power/sequencing/pwrseq-qcom-wcn.c#L550-L558
[linux-qmi-clock]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/qmi.c#L2380-L2395
[linux-aspm]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/pci.c#L911-L957
[linux-aspm-start]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/pci.c#L1159-L1168
[linux-aspm-boot]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/pci.c#L1450-L1469
[linux-suspend]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/net/wireless/ath/ath12k/core.c#L156-L253
[linux-bt-suspend]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/hci_qca.c#L2734-L2845
[linux-bt-power]: https://github.com/yzddmr6/linux-sm8750-piano/blob/b5100d5f2e0bb80e1191d521a48596674608eb3d/drivers/bluetooth/hci_qca.c#L2343-L2404
[bt-hci-spec]: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-60/out/en/host-controller-interface/host-controller-interface-functional-specification.html

## Fix-image gate

**HOLD — analysis only. No driver change or fix image.**

Path: **HOLD.** Shared PMU inputs → serialized WLAN/BT ownership and independent enables → WLAN PCIe/MHI/QMI firmware and
board data / BT UART patch and NVM → WMI INIT, DBS/MAC configuration, vdev/STA PS / standard HCI scan and connection policy →
opaque firmware/PTA/RF arbitration → HCI transport and bluetoothd → ATT/security/HID delivery, alongside airportd SET20,
authorized WLAN data and DHCP. All boundaries are mapped above; actual arbitration and end-to-end HID remain unqualified.

Checks: **HOLD.** ours-OK for shared owner protection, loaders, ordinary WLAN setup, initial PS values and runtime PS ordering,
BT transport and LE mask path. ours-missing for an explicit host coex bridge, newer Linux MAC-config override, idle-PS/UAPSD
parity, PS-state feedback and Linux-style ASPM restore policy. Unknown for firmware/NVM coex settings, physical band/antenna
sharing, live PS/ASPM state, simultaneous traffic/HID and combined sleep/wake recovery. Documentation checks are not RF passes.

Root cause: **HOLD / not established.** The recorded weak-BT/scan correlation and missing async-SBS override are candidates;
neither proves that PS=0, a missing WMI coex enable, RF topology, rails or macOS policy caused the observed failures. The
older LE timeout and long Wi-Fi OFF/ON failure also have separate event/daemon boundaries. Require matched RAM evidence
through the first failing hop before selecting a fix.

ONE change: **HOLD / none selected.** No production kext, firmware, NVM, installed boot argument or image change. After
qualification identifies a cause, reopen the gate for exactly one evidenced change and its reversible paired RAM control.
