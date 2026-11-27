# B7 USB-C OTG static analysis and attended qualification

Xiaomi Pad 8 Pro (`piano`, SM8750). **Analysis only; no tablet, electrical, or real-peripheral test was performed for this page.**
The rebased B7 opt-ins provide a guarded, bounded USB2 host loan to Apple's xHCI stack. They do not establish that an OTG adapter
receives safe VBUS, that a peripheral enumerates, or that USB3 works. USB device debug SSH remains the recovery path that every
loan must restore. Wi-Fi SSH must carry the attended session while USB-C is occupied.

## Evidence boundary

Repository sources below were inspected at `c332164f6845db5b423404b4496469449742502b` (the launcher-provided `main` base).
Linux references mean the Ubuntu reference kernel at **`7f8d6bedde479a01f4a05d6d0581b43c7fd20a01`**, read as Git objects at that
revision, independently of its checkout's HEAD or working changes. Linux paths are relative to that kernel repository; other paths
are relative to this repository. Linux is a behavioural specification; no Linux implementation is reproduced here.

The [host design](usb-host.md) contains the earlier H1–H9 ladder, and [host2 qualification](usb-host2-qualification.md) records
previous build/model results and proposed Phase U work. Those reports are not real-device results for this revision. This page
separates source coverage, missing implementation and observations still needed after the rebase. No image was built here and no
previous model or collection result is promoted to a hardware pass.

## End-to-end path

### Reference kernel: CC to class driver

| Step | Pinned Linux file and symbol/node | Behaviour to compare |
| --- | --- | --- |
| Board graph | `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts`: `pmic-glink/connector@0`, `&usb`, `&usb_dp_qmpphy`, `&usb_hsphy`, `dio4485` | Dual data/power roles; orientation GPIO61; HS endpoint to DWC3, SS endpoint to QMP, SBU endpoint to DIO4485. OTG, `usb-role-switch`, `mode-switch`, maximum `super-speed`; M31/eUSB2 repeater and QMP supply relationships. |
| Controller and DMA identity | `arch/arm64/boot/dts/qcom/sm8750.dtsi`: `usb@a600000`, `phy@88e8000` | Controller at `0x0a600000`; QMP combo at `0x088e8000`; USB `iommus = <&apps_smmu 0x40 0x0>`. Linux's DMA API is not Darwin's identity domain. |
| Firmware transport | `drivers/soc/qcom/pmic_glink.c`: `pmic_glink_rpmsg_callback`; `drivers/usb/typec/ucsi/ucsi_glink.c`: `pmic_glink_ucsi_callback`, `pmic_glink_ucsi_notify`, `pmic_glink_ucsi_pdr_notify` | Dispatch GLINK owners, schedule UCSI work, read CCI, notify UCSI, and handle service restart. UCSI is on the PMIC GLINK channel, not a QRTR service. |
| Orientation | `drivers/usb/typec/ucsi/ucsi_glink.c`: `pmic_glink_ucsi_connector_status`; `drivers/soc/qcom/pmic_glink_altmode.c`: `pmic_glink_altmode_sc8280xp_notify`, `pmic_glink_altmode_worker` | GPIO61 high means reverse; disconnected means no orientation. PAN carries port/orientation/mux; worker applies Type-C switch/mux state and ACKs it. The PAN decoder requires the exact notification size. |
| Data role | `drivers/usb/typec/ucsi/ucsi.c`: `ucsi_partner_change`, `ucsi_dr_swap`, `ucsi_pr_swap`; `drivers/usb/dwc3/drd.c`: `dwc3_usb_role_switch_set` | UFP partner selects host; DFP selects device; power and data roles are independent. Explicit role-swap commands are distinct from observing a completed swap. USB-data capability is checked before notifying the controller. |
| Glue and core | `drivers/usb/dwc3/dwc3-qcom.c`: `dwc3_qcom_set_role_notifier`, `dwc3_qcom_vbus_override_enable`, `dwc3_qcom_select_utmi_clk`; `drivers/usb/dwc3/core.c`: `__dwc3_set_mode`, `dwc3_set_prtcap` | Serialize role/PM changes, defer glue writes while suspended, stop the previous role, clear PHY suspend controls during a change, apply revision-dependent reset, select host/device and set PHY mode. QSCRATCH session-valid override is a controller signal, not the physical VBUS source. |
| USB3 lanes | `drivers/phy/qualcomm/phy-qcom-qmp-combo.c`: `sm8750_usb3dpphy_cfg`, `qmp_combo_com_init`, `qmp_combo_usb_power_on`, `qmp_combo_typec_switch_set`, `qmp_combo_typec_mux_set` | Enable supplies/clocks/resets; select software orientation with `SW_PORTSELECT_MUX` and reverse `SW_PORTSELECT_VAL`; select USB3-only or shared DP mode; program SerDes/TX/RX/PCS, start and poll PHYSTATUS clear. Orientation changes can power-cycle/reinitialize the active USB PHY under its mutex. |
| USB2 PHY | `drivers/phy/qualcomm/phy-qcom-m31-eusb2.c`: `m31eusb2_phy_set_mode`; `drivers/phy/qualcomm/phy-qcom-eusb2-repeater.c`: `eusb2_repeater_set_mode` | Pass host/device mode through the USB2 PHY; repeater host mode forces its 19.2 MHz clock, device mode clears that force. |
| Host family | `drivers/usb/dwc3/host.c`: `dwc3_host_init`, `dwc3_host_exit`; `drivers/usb/host/xhci-plat.c`: `xhci_plat_probe` | Create/remove xHCI; DWC3 passes `write-64-hi-lo-quirk`. Root-port enumeration and transfers precede any class-driver claim. Linux class drivers are a reference, not components of the Darwin collection. |
| Enumeration and storage | `drivers/usb/core/hub.c`: `hub_event`, `usb_new_device`; `drivers/usb/storage/usb.c`: `usb_stor_probe1`, `usb_stor_probe2`; `drivers/usb/storage/scsiglue.c`: `usb_stor_host_template`; `drivers/scsi/sd.c`: `sd_probe` | Hub/reset/address/descriptors precede interface matching, USB storage transport, SCSI probing and a usable disk. A working root port does not establish these later stages. |
| HID, Ethernet and audio | `drivers/hid/usbhid/hid-core.c`: `usbhid_probe`; `drivers/hid/hid-input.c`: `hidinput_connect`; `drivers/net/usb/cdc_ether.c`: `usbnet_cdc_bind`; `drivers/net/usb/cdc_ncm.c`: `cdc_ncm_bind`; `sound/usb/card.c`: `usb_audio_probe` | Interface class/protocol and descriptors select HID reports, network transport or UAC streams. These reference drivers are not substitutes for Darwin's corresponding collection dependencies and personalities. |
| Power side | `drivers/power/supply/xiaomi_mca.c`: `xiaomi_mca_sync_config`, `xiaomi_mca_process_notifications`, `xiaomi_mca_otg_cp`; `drivers/power/supply/sc8541_charger.c`: `sc8541_otg_configure`, `sc8541_otg_locked`, `sc8541_otg_restore_mode` | Board CP-present/OTG/CID configuration and firmware notifications coordinate the reverse gate separately from forward 2:1 charging. The reference rejects competing forward/pen ownership and retains OTG ownership after partial failure. |

### B7 control plane and ownership

[PianoPower.cpp](../../package/PianoPower/PianoPower.cpp) `serviceMca` owns `PMIC_RTR_ADSP_APPS`: charger owner `0x800a`, UCSI
`0x800b`, PAN `0x800c`. [PianoMca.c](../../package/PianoPower/PianoMca.c) `receive_pan` queues ACKs for reported ports;
`serviceMca` refreshes the connector contract and calls [PortEvents.c](../../package/PianoPower/PortEvents.c) `piano_port_publish`.
The fan-out copies at most 32 bytes and publishes ready client slots with release/acquire ordering. It is read-only delivery,
not an orientation or power actuator. [Ucsi.h](../../package/PianoUsbHost/Ucsi.h) `decodePort` accepts a PAN prefix of at least
15 bytes and marks the extra fields full only at 32 bytes or more; `portManager` does not require that full flag. This is weaker
than the reference's exact-length check. Require a 32-byte notification in the attended trace;
[PianoUsbHost.cpp](../../package/PianoUsbHost/PianoUsbHost.cpp) `portEvent`/`rxPush` copy it into an eight-entry ring. Overflow can
lose a notification; retained PAN is not independently timestamped. A received/ACKed notification alone proves no lane switch.

[UsbcRelay.c](../../package/PianoPower/UsbcRelay.c) `usbc_relay_request_ok` permits only well-framed UCSI reads/writes, with a
four-entry queue and generation invalidation. [PianoUsbcRelay.cpp](../../package/PianoPower/PianoUsbcRelay.cpp) supplies locking,
delivery and copied observations. This framing filter does not authorize arbitrary UCSI command semantics. In the qualification
profile, charging policy exclusively owns the mailbox: `PianoUsbHost::portManager` takes **read-only shared snapshots**, never
attaches a second command client, and disables its SET_UOR test switch. The legacy
[UcsiClient.h](../../package/PianoUsbHost/UcsiClient.h) `Client` path is for operation without that policy owner.

[UsbcObservation.h](../../package/PianoPower/UsbcObservation.h) `piano_usbc_observe` publishes committed connector status, not
an ACK's MessageIn. [PianoUsbcContract.h](../../package/include/PianoUsbcContract.h) `piano_usbc_observation_fresh` requires owned,
ready, valid, connector 1, valid status length, and monotonic timestamps within expiry. `ChargeContract.h` sets
`PIANO_CONTRACT_FRESH_MS=15000`; `portManager` rechecks shared status every second. Link loss, pending notification, quarantine
or expiry must close the real host gate. PAN uses port 0; UCSI uses connector 1. These are different numbering conventions.

[PortRole.h](../../package/PianoUsbHost/PortRole.h) `portView` combines UCSI, PAN, GPIO61 and, if available, PMIC CC witnesses;
UCSI 1.x has no orientation field. The live manager does not populate a PMIC witness and defaults its SPMI survey off with
PianoPower active, avoiding competing observer access. `UsbHostSurvey.h:decideRole` selects host for UFP/cable+UFP, device for
DFP/disconnect/unknown, and no data role for an analogue audio accessory. This analogue case is separate from digital USB audio.
`hostGate` additionally requires fresh connected status and a known power direction: source by default, sink only with
`piano-usbhost-sinkhost=1`. `RoleSwitch` debounces entry and returns on loss of host eligibility.

Two omissions matter: `hostGate` does not require orientation agreement or the partner USB-data flag, and it has no explicit
DP-only mux exclusion. Orientation disagreement is published, not an interlock. The attended protocol rejects those cases and
leaves DP/Alt Mode unused. Observing a dock's firmware-negotiated swap is supported; an active shared-owner SET_UOR/SET_PDR
negotiation is not part of this path.

### VBUS, SC8541 and charging policy

`PianoMca.c:board` sends the allowlisted CP-present (`0x201c=1`), fixed OTG configuration (`0x201a`) and CID (`0x201b`) per
charger generation. This selects the DSP's existing board power path; `PianoUsbHost` never writes a boost, gate, or SET_PDR.
**CP-present is not CHG_EN and is not proof that forward charging is active or that the reverse gate was configured.** In contrast,
the pinned Linux `xiaomi_mca_sync_config` obtains CP presence through its provider, and notification 10 drives the SC8541 reverse
gate through `xiaomi_mca_process_notifications`. Native `PianoMca.c:piano_mca_receive` treats charger notifications as refresh
requests; it does not implement that ordered reverse-gate notification handler. Native
[ChargePump.h](../../package/PianoPower/ChargePump.h) marks reverse register `0x05` never written, and
[ChargePumpImpl.h](../../package/PianoPower/ChargePumpImpl.h) has no Linux-equivalent OTG gate sequence. This is a concrete
reference gap, not permission to enable the forward pump as a workaround. How much ordinary OTG depends on it remains unknown.

[ChargeLink.h](../../package/PianoPower/ChargeLink.h) `ownBus` gives an observed source Rp precedence (external power); otherwise
fresh UCSI provider status identifies our bus, then PMIC sink Rd is the fallback. Unknown never counts as ours. The opt-in applies
only at host levels 3–6 and needs active policy to affect charging. `ChargeLink::step` cancels unsent policy writes on the first
own-bus observation, including PPS refresh work; an already transmitted firmware request cannot be recalled.
[ChargePolicy.c](../../package/PianoPower/ChargePolicy.c) `decide_inner` runs protection first, takes sourced VBUS as a detach,
lowers retained limits as needed and preserves charge inhibits. `detached` does not lift a stop while `own_bus` is set.
`PianoPower::publishBattery` excludes our VBUS from AC reporting. The existing 80/75 vote remains active; it is not replaced by
the host policy. Its hysteresis is graded only if actual samples cross 80% or 75%, without artificially changing SOC or thresholds.

The detach defaults **ICL 100 mA / FCC 500 mA are sink-charging limits**, not an OTG output rating. Likewise SC8541 BUSOCP's
1000–7000 mA range, 250 mA steps and 3500 mA forward-arm default in `ChargePump.h`/Linux `sc8541_charger.c` describe forward-pump
protection, not a qualified 5 V source-current limit. No native source-current programmer, independent back-feed interlock, or
VBUS discharge confirmation is established by these sources. Rp/status/LEDs and the own-bus guard cannot prove electrical
isolation. Low-load measurement and later dock isolation evidence are required; do not parallel supplies to test this.

The host-release gate does not consume `PianoCharge` own-bus/CP-OFF/readback state; the charging guard runs independently on
PianoPower's worker. There is no cross-driver acknowledgement that a prior in-flight charging request has settled before a
host loan begins. Keep this as a missing automatic interlock and an attended observation, rather than claiming atomic power/data
handoff. Any unexplained charging write or missing guard telemetry ends the trial.

Keep `piano-power-cp=0` and MiPPS off. The native forward pump needs both its boot opt-in and root arm, but boot arguments alone
do not prove hardware OFF: `PianoPower::sampleSafety` reads SC8541 CTRL2 even with the driver disabled. Require fresh
`PianoCharge.SafetyValid & 1` and `CPControl & 0x10 == 0` before and throughout a loan. Do not write the CP arm sysctl.

### Controller, DMA, xHCI and device mode return

The live path is level **4 with handoff 6**, not a standalone level-6 takeover that displaces the debug driver. Both
`piano-usb-handoff=1` and `piano-usbhost-handoff=6` must be set. `PianoUsbHost::start` requires nonzero `back` and shared S1, and
forces host interrupt polling while retaining the gadget's SPI handler. `sharedDomain(false)` accepts a verified domain after
cable removal has dropped CDC/NCM. A separate diagnostic loan requires CDC and NCM active first; its sysctl is absent in the real
profile. It is not a test of VBUS or a real Type-C partner.

[PianoDwc3.cpp](../../package/PianoDwc3/PianoDwc3.cpp) `identitySetup` is the sole shared SID `0x40` initializer, with DWC3 halted.
[SmmuIdentity.h](../../package/PianoUsbHost/SmmuIdentity.h) `planSmmuIdentity`/`applySmmuIdentity` select an unused context bank
and independent ASID, reject ambiguous SID matches/changed owners, audit masked readback and synchronize context/global TLBs.
The write-back, inner-shareable identity window is **[2 GiB, 64 GiB)**, not all addresses. Tables and ownership remain pinned
through loans; `UsbDomain.h:DomainOwner` exposes readiness, not a borrower initialization request. Valid fault mask is
`kFsrFaults=0xc00001fe`; raw FSR `0x400` alone is not a valid fault. Fingerprinting is a no-competing-owner precondition, not an
atomic lock across unrelated kexts. [tests/smmu_identity_host.cpp](../../package/PianoUsbHost/tests/smmu_identity_host.cpp)
models readback quirks, collisions, unwind, held ownership and bounded loans; it is not a DMA test on this tablet.

`ControllerLoan::begin` starts the budget **before release and host startup**. `PianoDwc3::handoffRelease` halts the gadget,
masks its event source and parks the USB thread; only confirmed `kHoReleased` permits lending. `XhciHost.h:hostSwitch` rechecks
USB30 power/clocks, clears QSCRATCH session overrides, selects UTMI-as-PIPE, resets as required, clears PHY suspend bits, writes
`GCTL.PRTCAPDIR=1` and verifies `GSTS.CURMOD=1`. Each glue store checks its clocks again. This is currently a USB2 path.

The optional preliminary `xhciProbe` must complete a No-Op with code 1 and the submitted TRB address, then halt/reset before
Apple starts. `PianoUsbHost::appleUp` publishes `PianoUsbHostStack`, waits for host resources and creates exactly one
`PianoUsbHostNub` client. [PianoXhciHost.cpp](../../package/PianoXhciHost/PianoXhciHost.cpp) overrides desired power to state 3,
64-bit reads low/high and writes high/low. Its ABI is pinned to the collection via
[build-native-piano-xhcihost.sh](../../tooling/k27/build-native-piano-xhcihost.sh), not a portable promise across Apple versions.
The live path uses the nub's 1 ms poll delivery, not a newly acquired host SPI interrupt. `liveUp` requires RUN, not halted,
powered root ports and readable DMA pointers in the identity window.
[AppleDmaReadback.h](../../package/PianoUsbHost/AppleDmaReadback.h) deliberately excludes CRCR's read-as-zero pointer from address
validation. Readable DCBAAP/ERSTBA/ERDP coverage does not establish Apple's command-ring completion or peripheral transfer DMA.

USB2/USB3 port identity comes from xHCI Supported Protocol capabilities (`XhciHost.h:portMajor`), not a guessed port number.
`UsbHostSurvey.h:surveyQmp` only reads the combo PHY. There is no B7 USB3 SerDes/PCS initialization or orientation actuator:
setting `piano-usbhost-qmp=1` is a survey, and setting `-utmi=0` does not implement USB3. USB2 D+/D- do not require SS lane
orientation; both connector orientations still need an attended test. The eUSB2 repeater host clock-force transition and a shared
host/DP/PM ownership scheme are also missing. Avoid sleep during host mode; the host manager has no coordinated PM teardown.

`ControllerLoan::end` handles deadline, disconnect, changed data/power eligibility and stale relay status. `liveDown` calls
`appleDown`: drain callbacks (1 s), terminate Apple's nub on a worker (5 s), then halt/reset xHCI. `hostEnd`/`backToDevice` restore
device role and saved glue/PHY controls before `dwc3Ask(reclaim, 5000)`. `PianoDwc3::handoffPark` waits awake, rechecks clocks and
identity, reruns takeover and replays retained console. If Apple start/stop or DMA safety is unconfirmed, ownership/memory are
held for the boot backstop; software must not force an unsafe reclaim. Therefore the loan timer is a bounded attempt at return,
not a guarantee that USB SSH will recover. The global reset guard remains mandatory. Expiry prevents re-entry on the same cable;
the physical partner must disconnect before another real loan. Registry properties can lag a switch until the next view update:
use the console return and actual gadget re-enumeration as well.

### Class-driver closure

[PianoUsbHostStackA2.json](../../tooling/k27/kext/PianoUsbHostStackA2.json) pulls host/hub/composite, mass storage,
`IOSCSIBlockCommandsDevice`, `IOUSBHostHIDDevice`, IOHIDFamily, CDC ECM/NCM and ASIX. Its three HID fallback personalities
require a `PianoUsbHostNub` ancestor: two HID interface subclass matches (0/1) and USB keyboard/mouse/consumer event matching.
They must not match the gadget's USB-transport input services. Storage needs the chain from `IOUSBHostDevice` through a USB
mass-storage interface and SCSI peripheral to `IOSCSIPeripheralDeviceType00`, block storage and `IOMedia`; a USB descriptor alone
is insufficient. Do not infer UAS support or filesystem success from the BOT/SCSI personality closure.

[check-usbhost-stack.py](../../tooling/k27/check-usbhost-stack.py) `check_a2_consumers` checks linked metadata, the SCSI direct-access
personality and the scoped HID matches. `PIANO_KEXT_PROFILE=usbhost-a2` selects the opt-in stack; a daily collection must remain
inert. USB Ethernet personalities cover specific protocols/chips, not all dongles. AppleUSBAudio is **not** in this A2 descriptor's
library list; another dependency might incidentally pull it in, but USB UAC matching/isochronous operation is unproven. The native
internal speaker driver is not evidence for USB audio. Inspect the actual collection before adding UAC to a later attended arm.

## Path & Check Matrix

`ours-OK` means the stated source-level mechanism exists at the assessed base. It does **not** mean hardware qualified.
`ours-missing` means an identified native implementation/guard/closure is absent. `unknown` means source inspection cannot
settle the runtime or electrical result. Each row grades only its stated check; related missing/unknown checks remain separate.

| ID | Check/event | Mark | Exact evidence and remaining acceptance |
| --- | --- | --- | --- |
| C1 | PAN enable, ACK and fan-out | ours-OK | `package/PianoPower/PianoMca.c:receive_pan`; `PianoPower.cpp:serviceMca`; `PortEvents.c:piano_port_publish`. Require enable answered, port 0 notification ACKed and received by the host manager. |
| C2 | UCSI transport isolation/restart | ours-OK | `package/PianoPower/UsbcRelay.c:usbc_relay_request_ok`, `usbc_relay_link`; `PianoUsbcRelay.cpp:piano_usbc_relay_step`. Only UCSI framing; invalidate old-generation queue. |
| C3 | One UCSI command owner under charging policy | ours-OK | `package/PianoUsbHost/PianoUsbHost.cpp:portManager`; `package/PianoPower/PianoPower.cpp:serviceMca`; `UsbcObservation.h:piano_usbc_observe`. Shared observation, no second reset/ACK sender. |
| C4 | Fresh connector witness and expiry | ours-OK | `package/include/PianoUsbcContract.h:piano_usbc_observation_fresh`; `package/PianoUsbHost/PortRole.h:observePort`; `PianoUsbHost.cpp:portManager`. Recheck every second; reject expired/invalid connector status. |
| C5 | CC orientation decode and disagreement reporting | ours-OK | `package/PianoUsbHost/Ucsi.h:decodePort`, `decodeStatus`; `PortRole.h:portView`; `PianoUsbHost.cpp:publishPort`. UCSI 1.x/PAN/GPIO distinction; no live manager PMIC witness. |
| C6 | Orientation/PAN delivery in both physical orientations | unknown | `package/PianoUsbHost/PianoUsbHost.cpp:rxPush`, `portManager`; `PortRole.h:portView`. No independent PAN expiry and ring can drop; compare fresh observations on each insertion. |
| C7 | Orientation agreement, USB-data flag and DP-only interlocks | ours-missing | `package/PianoUsbHost/PortRole.h:hostGate` tests role/power/freshness only; `UsbHostSurvey.h:decideRole` omits partner USB flag. Require operator rejection; Linux `drivers/usb/typec/ucsi/ucsi.c:ucsi_partner_change` checks USB capability. |
| C8 | Role/power gate, debounce, defaults off | ours-OK | `package/PianoUsbHost/PortRole.h:hostGate`, `RoleSwitch::step`; `PianoUsbHost.cpp:readArgs`, `start`. DFP/empty/unknown stays device; sink-host opt-in independent of data role. |
| C9 | Active shared-owner data/power swap request | ours-missing | `package/PianoUsbHost/PianoUsbHost.cpp:portManager` disables SET_UOR when shared; `Ucsi.h:setPdr` is a codec, not a sent power request. Do not force a swap with the debug computer. |
| C10 | Dock's negotiated UFP/consumer observation | unknown | `package/PianoUsbHost/PortRole.h:hostGate`; `PianoUsbHost.cpp:portManager`. Needs real PD pass-through dock with fresh UFP and sink witness; source Rp alone says nothing about data role. |
| C11 | Strict PAN framing parity with the reference | ours-missing | `package/PianoPower/PianoMca.c:receive_pan` accepts a notification from 13 bytes; `package/PianoUsbHost/Ucsi.h:decodePort` accepts 15 bytes; `PianoUsbHost.cpp:portManager` does not require `full`. Linux `drivers/soc/qcom/pmic_glink_altmode.c:pmic_glink_altmode_sc8280xp_notify` requires its exact struct size. |
| P1 | Allowlisted board power configuration | ours-OK | `package/PianoPower/PianoMca.c:board`, `taken`; `PianoPower.cpp:serviceMca`. Require successful CP/OTG/CID ACKs, not merely configuration settled after exhausted attempts. |
| P2 | SC8541 reverse-gate notifications and ownership | ours-missing | `package/PianoPower/PianoMca.c:piano_mca_receive` only refreshes on charger notify; `ChargePump.h:PIANO_CP_REG_REVERSE` never written. Compare Linux `xiaomi_mca_process_notifications`/`sc8541_otg_locked`. |
| P3 | Actual 5 V source, output current limit, discharge and back-feed isolation | unknown | `package/PianoUsbHost/PianoUsbHost.cpp:liveUp` changes no source switch; `package/PianoPower/PianoPower.cpp:report` exposes diagnostics. No electrical qualification or source-current limit follows from those readings. |
| P4 | Own-bus decision, Rp precedence, queued write/PPS cancellation | ours-OK | `package/PianoPower/ChargeLink.h:ownBus`, `ChargeLink::step`, `cancelQueued`; `ChargePolicy.c:decide_inner`. Require OwnBus input/decision and no new sink voltage/current escalation while sourcing. |
| P5 | Preserve 80/75 stop and exclude our bus from AC | ours-OK | `package/PianoPower/ChargePolicy.c:protection`, `detached`, `decide_inner`; `PianoPower.cpp:publishBattery`. Runtime health-hold and AC observations still required. |
| P6 | CP remains OFF with valid safety readback | unknown | `package/PianoPower/PianoPower.cpp:sampleSafety`, `probeCp`; `ChargePump.h:PIANO_CP_CHG_EN`. Boot CP off plus fresh SafetyValid bit 0 and CHG_EN clear; absent readback fails preflight. |
| P7 | Power-policy/CP acknowledgement before lending | ours-missing | `package/PianoUsbHost/PortRole.h:hostGate`; `PianoUsbHost.cpp:liveUp`, `sharedDomain` check role/domain, not charge state; `package/PianoPower/ChargeLink.h:ChargeLink::step` cancels only unsent writes. No atomic power/data handoff is established. |
| D1 | Sole shared SMMU initializer and legal route | ours-OK | `package/PianoDwc3/PianoDwc3.cpp:identitySetup`, `identityVerify`; `package/PianoUsbHost/SmmuIdentity.h:planSmmuIdentity`, `applySmmuIdentity`. Halt first, independent bank/ASID, audited readback and sync. |
| D2 | Legal unwind, pinned tables and loan model | ours-OK | `package/PianoUsbHost/SmmuIdentity.h:unwindSmmuIdentity`; `package/PianoDwc3/UsbDomain.h:DomainOwner`; `package/PianoUsbHost/tests/smmu_identity_host.cpp:main`. Model covers failures; no live DMA proof. |
| D3 | Device release and host/device glue sequence | ours-OK | `package/PianoDwc3/PianoDwc3.cpp:handoffRelease`; `UsbHandoff.h:handoffCoreFree`; `package/PianoUsbHost/XhciHost.h:hostSwitch`, `glueStore`, `backToDevice`. Require confirmed halt and clocked glue. |
| D4 | No-Op and root-port power/reset machinery | ours-OK | `package/PianoUsbHost/XhciHost.h:xhciReset`, `xhciInit`, `portReset`; `PianoUsbHost.cpp:xhciProbe`, `liveUp`. Require completion code 1/correct TRB, not just a running bit. |
| D5 | Apple subclass attach and 64-bit access order | ours-OK | `package/PianoUsbHost/PianoUsbHost.cpp:appleUp`, `makeNub`; `package/PianoXhciHost/PianoXhciHost.cpp:XHCI_DESIRED_POWER`, `XHCI_READ64`, `XHCI_WRITE64`; `Reg64.h`. Pin ABI/KC, exactly one client. |
| D6 | Borrowed SPI ownership and callback teardown | ours-OK | `package/PianoUsbHost/PianoUsbHost.cpp:start`, `nubPoll`, `nubDrain`, `appleDown`. Live handoff forces polling, drains before reset; no SPI detach assumption. |
| D7 | Apple ring-pointer coverage and actual transfers | unknown | `package/PianoUsbHost/AppleDmaReadback.h:AppleDmaReadback::failure`; `PianoUsbHost.cpp:liveUp`. Require readable-covered=1 and real HID/read traffic without valid SMMU fault; CRCR raw zero is not failure. |
| D8 | Deadline/unplug/role/relay return and same-cable latch | ours-OK | `package/PianoUsbHost/ControllerLoan.h:begin`, `end`, `returned`, `disconnected`; `PianoUsbHost.cpp:portManager`. Deadline begins before release; held state on unsafe return. |
| D9 | Real USB2 enumeration in both orientations | unknown | `package/PianoUsbHost/XhciHost.h:portMajor`; `PianoUsbHost.cpp:portLine`, `usbDevices`, `appleTick`. Require CCS/PED/speed and IOUSBHostDevice under this nub plus class function. |
| D10 | QMP USB3 init and lane orientation writes | ours-missing | `package/PianoUsbHost/UsbHostSurvey.h:surveyQmp` is read-only; `XhciHost.h:HostPolicy` selects UTMI-as-PIPE. Linux `qmp_combo_com_init`/`qmp_combo_usb_power_on` have no native equivalent here. |
| D11 | Repeater host-mode clock force and coordinated host PM/DP ownership | ours-missing | `package/PianoUsbHost/UsbHostHw.h:kRepForceEn5`, `kRepForceVal5` are facts only; `PianoUsbHost.cpp` has no host PM teardown. Linux `eusb2_repeater_set_mode`, `dwc3_qcom_pm_suspend` supply comparison. |
| D12 | Proven device-mode recovery after real loan | unknown | `package/PianoUsbHost/PianoUsbHost.cpp:liveDown`, `appleDown`, `hostEnd`; `package/PianoDwc3/PianoDwc3.cpp:handoffPark`. Require actual CDC/NCM re-enumeration and SSH on unchanged boot, not a RETURN line alone. |
| A1 | A2 SCSI/HID linked closure and scoped matches | ours-OK | `tooling/k27/kext/PianoUsbHostStackA2.json:libraries`, `personalities`; `tooling/k27/check-usbhost-stack.py:check_a2_consumers`. Check the session's actual linked collection. |
| A2 | Keyboard/mouse reports reach input consumers | unknown | Same descriptor's USB HID fallbacks; `package/PianoUsbHost/PianoUsbHost.cpp:usbDevices`. Require IOHIDEventDriver ancestry plus deliberate input/release behaviour; gadget/cover input is not evidence. |
| A3 | MSC/SCSI media, bounded read and readonly filesystem | unknown | A2 `IOSCSIBlockCommandsDevice` dependency and SCSI Type00 metadata check. Require IOMedia/block ancestry and fixture hash; UAS and writes are unqualified. |
| A4 | Hub children and disconnect cleanup | unknown | A2 libraries `AppleUSBHub`, `AppleUSBHostCompositeDevice`; `PianoUsbHost.cpp:usbDevices` counts descendants. Need real hub port/child hotplug; a hub-only descriptor is insufficient. |
| A5 | USB Ethernet protocol/chip driver and traffic | unknown | A2 libraries CDC ECM/NCM/ASIX. Require correct USB interface ancestry, network service and bounded traffic later; enumeration is not link/IP function. |
| A6 | Explicit UAC collection dependency | ours-missing | `tooling/k27/kext/PianoUsbHostStackA2.json:libraries` lacks AppleUSBAudio. Inspect linked metadata before claiming availability; no UAC test is authorized by this analysis. |
| A7 | UAC stream/isochronous audio and recovery | unknown | `package/PianoUsbHost/PianoUsbHost.cpp:appleTick` observes devices, not audio transfers. Need UAC descriptors, audio service, playback/capture, underrun checks and recovery in a later attended arm. |

## One bounded attended RAM protocol

This is a future operator procedure, not an instruction to execute now. Use one staged protocol: empty port, mouse, keyboard,
then a read-only flash fixture, with a verified return to device mode **between every real loan**. Repeat the same protocol in a
fresh RAM boot with the USB-C plug reversed before claiming orientation coverage. Do not add a powered hub, Ethernet or audio
to these first loans. A protocol failure ends the session; no forced role, CP arm, retry loop or installed-image change follows.

### Fixture, image and transport preflight

An operator must be present with the tablet, a known compliant unpowered USB-C OTG adapter, low-power wired USB2 mouse/keyboard,
and a small USB2 flash fixture. No PD pass-through, charger, Y cable, externally powered hub, disk motor or second power source.
Use an inline USB power meter that preserves CC/data, and a pre-characterized fixture budget of at most 100 mA including the
adapter/bridge during these first trials. **100 mA is a conservative test-load ceiling, not a claim about the tablet's output
rating.** Stop on unexpected voltage/current, heating, unstable power or disconnect oscillation; do not provoke overcurrent or
inject voltage to discover a limit. A measured 4.75–5.25 V under this load is the chosen qualification window. Missing reverse-gate
support can leave a peripheral unpowered: record that failure and stop, without changing registers.

Prepare the flash fixture beforehand on another machine: genuine hardware write-protect, or a validated hardware USB write
blocker, a clean supported filesystem, synthetic volume label `OTGQUAL`, and one known 1 MiB file `fixture.bin` with a recorded
SHA-256. Include the blocker in the power budget. Software readonly mounting alone cannot prevent Disk Arbitration/metadata
writes before the command. Without hardware protection, skip storage and record it unqualified. No format, repair, fsck,
benchmark, write test, recursive scan or firmware update. Do not expose personal files.

Use a reviewed 27.3 RAM image and matched `usbhost-a2` KC, with successful native-kext/import/427-slot ABI and linked-stack
metadata checks retained by the image builder. Confirm exactly one PianoXhciHost nub client, SCSI Type00 and all three scoped
HID fallback personalities. Keep the daily fixed refresh, battery publication and Wi-Fi settings; use volatile Data and an
independent global backstop. Merge each key once into the **effective** RAM arguments (including the ADT tail):

```text
data-persist=0 piano-reset-after=900 piano-reset-to=bootloader
piano-usb=2 piano-usb-net=1 piano-usb-handoff=1 piano-usb-smmu=2
piano-usbhost=4 piano-usbhost-handoff=6 piano-usbhost-a2=1
piano-usbhost-smmu=2 piano-usbhost-probe=1 piano-usbhost-attach=1
piano-usbhost-debounce=1000 piano-usbhost-period=1000 piano-usbhost-back=60
piano-usbhost-utmi=1 piano-usbhost-spmi=0 piano-usbhost-irq=0
piano-usbhost-sinkhost=0 piano-usbhost-test-loan=0 piano-usbhost-uor=0
piano-power=2 piano-power-policy=1 piano-power-ucsi=1 piano-power-board=1
piano-power-own-bus=1 piano-power-health=1
piano-power-health-stop=80 piano-power-health-resume=75
piano-power-cp=0 piano-power-mipps=0
```

`piano-usb=2` is sufficient for the gadget path; retain a higher qualified daily value if required by that image. Do not carry
standalone level 5/6 takeover arguments or a delayed gadget start into this protocol. Keep the daily charging limits/thermal
guards; make no charging-mode or threshold changes during the run. Use a prepared RAM dump companion for post-reset pstore
recovery and an external stopwatch. The backstop resets to bootloader, not to a successful in-boot device recovery.

Start with the debug computer attached. Establish **two working root Wi-Fi SSH sessions**, verify commands still work over Wi-Fi
after removing USB later, and keep the Wi-Fi route stable. Use the operator's existing host configuration without publishing its
addresses or credentials. Do not depend on USB Ethernet under test. Keep sleep inhibited for this RAM boot (for example,
`caffeinate -dimsu` in a dedicated session) and retain the tablet display/operator input. If sleep occurs, fail the run.

Capture on the operator's computer, in access-controlled untracked evidence, image/KC hashes, effective arguments, orientation,
fixture model/class/speed, timestamps and the following root reads before unplugging and after each return:

```sh
sysctl kern.boottime kern.bootargs
sysctl machdep.piano_data_io
sysctl machdep.piano_sdam_survey machdep.piano_bootflag
ioreg -r -c PianoUsbHost -l -w 0
ioreg -r -c PianoPower -l -w 0
ioreg -r -c AppleSmartBattery -l -w 0
pmset -g batt
```

Require effective `data-persist=0` and the 900-second bootloader reset; `machdep.piano_data_io` must report `mode=volatile`,
`persist=0`, `reset_after=900`, `reset_fastboot=1`, no store writes and no new errors
(`package/NativeRamBlock/NativeRamBlock.cpp:directStatsSysctl`). If the expected OID/evidence is unavailable, resolve the image
offline and do not disconnect the recovery cable. Require two fresh power samples (at least 5 seconds apart):
`HealthEnabled=1`, `HealthStop=80`, `HealthResume=75`, `HandoverFault=0`, valid CP readback with CHG_EN clear, and no safety alarm.
At the debug computer expect fresh owned UCSI `connected 1 provider 0 partner 1` (DFP), role `device`, VBUS `sink`,
`OwnBusGuard=1 OwnBusInput=0 OwnBus=0`, CDC and NCM SSH working, and **zero** loan BEGIN lines or host-nub instances.
The subclass's module-load line alone is allowed and is not an attach.

If a health stop is already active, require `HealthHold=1` and fresh `EnableReadback=0` across the role change until actual SOC
permits resume at 75%; sourcing must not lift that inhibit. If the sampled higher-pack SOC reaches 80%, require the stop and
acknowledged disable; if it later reaches 75% with a real external source, require normal policy resume. A short run without
those crossings records the threshold behaviour as unexercised. Configuration/hold preservation can pass independently.

Stream the kernel log over Wi-Fi before removing USB; capture retained CDC replay after recovery as a second source:

```sh
log stream --style syslog --level debug --predicate 'process == "kernel"'
```

Require console `domain READY owner PianoDwc3`, `fault_bits 00000000`, fresh shared observations tagged
`PianoUsbUcsiSharedA2c`, successful board configuration and port-enable ACK. If the system's log service does not expose kernel
IOLog, use the image's qualified console capture/replay mechanism; missing trace is not a pass. Do not write
`kern.piano_usbhost_loan`: with `piano-usbhost-test-loan=0` the diagnostic OID should not exist. There is no real-path sysctl
that cancels a loan or safely forces device reclaim.

### Ordered trials and exact acceptance

Finish preflight by boot elapsed 180 s; otherwise stop and use the backstop. Start no new loan after elapsed 600 s, leaving at
least 300 s for recovery before the global reset. The per-loan 60 s includes release, No-Op and Apple startup. Return cleanup adds
its bounded drain/termination/reclaim waits; never extend `back` in place. Allow at most 15 s after loan deadline for the device
return line, and require USB CDC plus NCM SSH within 60 s of reconnecting the debug computer. Any missed bound fails the trial.

1. **Empty port, 20 seconds.** Remove the debug cable, keep the receptacle empty and run commands over both Wi-Fi sessions.
   Require fresh disconnected status, `piano-usbhost-port="nothing attached"`, role `device`, no loan BEGIN, no
   `PianoUsbHostNub`, no host peripherals and no AC from our bus. Stale/unknown status is not evidence of a clean disconnect.
   This checks device readiness while empty; it does not force an empty-port host loan.
2. **USB mouse.** Attach the adapter with only the mouse. Require fresh connected provider/UFP (partner 2, or cable+UFP 4),
   a 32-byte port 0 PAN and GPIO orientation agreement, `piano-usbhost-role="host"`, VBUS `source`, then the real loan.
   Reject an unknown direction, analogue audio, DP mux or disagreeing witnesses. Within two normal charger-report cycles require
   `OwnBusGuard=1 OwnBusInput=1 OwnBus=1`, AC disconnected and valid CP OFF readback. Check the meter continuously.
   Require the host and HID evidence below. Move the mouse in both axes, click a harmless visible control and release every
   button; require matching movement/click/release with no stuck input. Let this first loan expire with the device still attached
   to test the deadline. Require one `loan END reason deadline`, one successful return and no second BEGIN on that same cable.
   Disconnect the adapter, reconnect the debug computer and pass the recovery checks before proceeding.
3. **USB keyboard.** Disconnect the debug computer, verify fresh empty status, then attach only the keyboard through the adapter.
   Apply the same gate, power, DMA, enumeration and HID checks. Type `otg-test` into an unsaved text field, use Backspace, hold
   and release Shift, and confirm the next character has no stuck modifier. Do not enter commands or save a file. Unplug the
   adapter **before** the deadline to exercise disconnect return. Require one successful return; reconnect the debug computer
   and pass recovery. Where event ordering permits, the reason is `unplug`; a notification-in-flight can invalidate the snapshot
   first and produce `relay`, or changed eligibility can produce `role`. Keep the timestamped status/END sequence; never relabel
   an unexplained reason as an unplug pass.
4. **Read-only flash.** After another fresh empty state, insert only the hardware-protected fixture through the adapter.
   Apply all common host/power checks. Require MSC interface plus SCSI/block/IOMedia ancestry, correct small fixture capacity,
   USB2 speed and OS-reported write protection. Assign the observed external partition to `QUAL_PART` only after checking its
   ancestry; never guess a disk number or select an internal/RAM volume. Run the storage reads below, verify the known file
   hash, unmount, then unplug and require return/recovery. If the time budget cannot fit the read and unmount, skip the read,
   unmount if possible and record storage incomplete. The boot backstop still bounds a stuck I/O command.

For each loan, these read-only inspection commands supply the actual registry evidence. Inspect the **full provider chain** under
`PianoUsbHostNub`; global class hits can belong to the gadget, cover keyboard, internal media or another controller:

```sh
ioreg -p IOService -r -c PianoUsbHostNub -l -w 0
ioreg -r -c PianoXhciHost -l -w 0
ioreg -r -c IOUSBHostDevice -l -w 0
ioreg -r -c IOUSBHostInterface -l -w 0
ioreg -r -c IOUSBHostHIDDevice -l -w 0
ioreg -r -c IOHIDInterface -l -w 0
ioreg -r -c IOHIDEventDriver -l -w 0
ioreg -r -c IOSCSIPeripheralDeviceType00 -l -w 0
ioreg -r -c IOBlockStorageDriver -l -w 0
ioreg -r -c IOMedia -l -w 0
```

Common console pass: exactly one `loan BEGIN budget-s 60 diagnostic 0` per insertion, fresh provider/UFP witness, confirmed
gadget release, host switch `PRTCAPDIR 1`/`CURMOD 1`, No-Op PASS (code 1/correct submitted pointer), attach client
`PianoXhciHost`, `PianoUsbDmaReadbackA2d` with `readable-covered 1`, and `loan HOST ... root-port-up` before the deadline.
Root-port evidence must show powered when PPC, connected and enabled (`PP`, `CCS`, `PED`), with Supported Protocol=USB2 and the
observed mouse/keyboard speed (low/full/high as appropriate), or high-speed 480 Mbit/s for the chosen flash fixture. Require
USBCMD.RUN=1, USBSTS.HCHalted=0, HSE/HCE clear, increasing polled callback activity, and zero valid SMMU fault bits at every
available domain sample. Record actual speed; no port number or USB3 capability is a negotiated-speed proof.

HID pass requires the external device/interface, `IOUSBHostHIDDevice`, `IOHIDInterface` and `IOHIDEventDriver` beneath this nub,
plus the deliberate physical input and release behaviour. If an approved input-event observer is already available, retain its
matching press/release/relative-motion events too; registry presence or a gadget-injected event alone cannot pass input delivery.

After binding `QUAL_PART` to the verified fixture partition (for example the observed `diskNs1`, with N replaced locally), use:

```sh
diskutil list external physical
diskutil info "/dev/${QUAL_PART}"
diskutil unmount "/dev/${QUAL_PART}"
diskutil mount readOnly "/dev/${QUAL_PART}"
mount
shasum -a 256 /Volumes/OTGQUAL/fixture.bin
diskutil unmount "/dev/${QUAL_PART}"
```

If not mounted initially, omit the first unmount. Require `diskutil info` to identify USB external physical media with hardware
readonly status, and `mount` to show the fixture mounted read-only. An automatically mounted read-only protected fixture is
acceptable, but check the flags before reading it. Require the 1 MiB file's complete expected hash, no transport/reset/I/O errors,
and successful unmount before deliberate removal. Enumeration without `IOMedia`, an unexpected writable status, hash mismatch,
timeout, or an unsolicited writable mount fails storage. Hardware protection remains required even if the OS reports readonly.

### Recovery, stop conditions and evidence record

After **every** loan require `Apple stop completed`, xHCI stop `ok`, device `PRTCAPDIR 2`, successful PianoDwc3 takeover and
`loan RETURN device domain-pinned 1`; no `HELD`, `FAILED`, negative stage, valid SMMU fault, panic, storm, persistent reset loop
or second loan on the same attachment. Reconnect the debug computer only after removing the OTG fixture. Require actual CDC
console and NCM SSH re-enumeration within the bound, retained log replay, `gadget CDC configured 1 network 1 ... domain-valid 1`,
the **same `kern.boottime`**, and successful root reads over USB NCM as well as Wi-Fi. Device-role intent in ioreg alone cannot
pass recovery. Re-read power state: provider=0/DFP, `OwnBusInput=0 OwnBus=0`, valid CP OFF, health configuration intact, no
`PianoChargeHandoverFault`, and no unexpected SDAM/bootflag change. Require the host nub/peripheral subtree to disappear.

Stop on Wi-Fi command loss, unavailable safety telemetry, unexpected loan, clock/readback failure, stale witness, electrical
anomaly, premature sleep or any recovery failure. Remove the OTG fixture; do not force MMIO/role/charger changes. Leave the
operator present for the prearmed reset to bootloader, then use the prepared recovery/dump procedure and collect pstore.
A reset-assisted return is a **failed in-boot recovery**, even if the next boot's USB works. End the session at its bounded reset;
do not install an image, change UFS/ESP/Data persistence or promote opt-ins to the daily profile.

Record each arm as pass/fail/incomplete with base/reference/image/KC hashes, actual merged arguments, elapsed start/deadline/
END/RETURN/reconnect times, orientation, role/power witnesses, meter range, root-port speed, class ancestry, input/read result,
health/CP/own-bus samples, same-boot proof and recovery result. Raw ioreg, console and network output can contain identifiers;
retain it outside tracked files and publish only sanitized outcomes. Do not commit addresses, network names, device serials,
credentials or operator-machine paths. An omitted test remains unknown.

## Later real-device coverage

These are acceptance gates for a subsequent attended session, **not extra steps in the first protocol**:

| Device/path | Prerequisite | Required observations |
| --- | --- | --- |
| USB3 mass storage | Native QMP USB3 init/orientation/ownership gap closed and reviewed | Repeat bounded, hardware-readonly hash/read and recovery in both orientations; Supported Protocol USB3, PORTSC speed 4 (5 Gbit/s) and matching device speed. USB2 fallback is useful but does not pass USB3. |
| USB-C hub, unpowered | First protocol passed; known total load within a separately established source budget | AppleUSBHub ancestry, each downstream child enumerates and works, one child hot-unplug leaves others working, upstream unplug/deadline removes all children and restores CDC/NCM. Grade USB2 and USB3 separately. |
| PD pass-through hub | Reverse-gate/isolation and source-current uncertainties resolved; reviewed sink-host arm | Separate RAM boot with `sinkhost=1`: fresh UFP/consumer after firmware swap, `OwnBus=0`, external AC and normal charging/80–75 evidence. With `sinkhost=0`, consumer/UFP must stay device. No forced source or parallel-power experiment. |
| Hub Ethernet | Actual KC contains matching ECM/NCM/ASIX or the specific chip's driver | USB interface to network service ancestry, link and bounded traffic, clean detach and debug recovery. Keep Wi-Fi SSH primary and avoid changing its route; exclude addresses from published evidence. |
| Digital USB audio/UAC | Actual AppleUSBAudio dependency/personality/import closure established | UAC device/interfaces to audio engine; bounded playback and capture at a stated format, real sound/input, no isochronous errors/underruns, detach/deadline recovery. Analogue accessory detection and internal audio do not pass UAC. |
| Host sleep/DP coexistence | Shared controller/PHY/clock PM ownership implemented | Separate design/review before any sleep, DP-only mux or Alt Mode trial. The initial awake USB2 protocol cannot qualify these paths. |

## Fix-image gate

Status: HOLD

Analysis-only. Real OTG-device qualification requires an attended session with an OTG adapter/device, working Wi-Fi SSH and
the bounded RAM/recovery protocol above. No real-device, USB3, hub, Ethernet, UAC, VBUS-isolation or source-current result is
claimed. Do not ship a fix image, change the installed profile, or enable daily host opt-ins on this evidence. Revisit only with
reviewed missing-path changes where needed and sanitized attended results proving both peripheral function and device-debug
recovery; static/model/collection success alone cannot clear this gate.
