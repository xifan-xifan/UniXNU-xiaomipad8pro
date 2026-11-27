# USB host2 integration and Phase U qualification

The eight audited commits `9a01affe` through `d23ff3d9` extend the bounded host loan to a sink-powered UFP and add the opt-in own-bus charging guard, A2 block storage and keyboard, mouse and consumer HID fallbacks.

On `main`, the own-bus flag uses bit 21. Non-PD, unmapped PD and virtual skin temperature retain bits 18, 19 and 20. The guard discards unsent charge-policy writes, including any queued PPS refresh, when sourced VBUS first becomes known. It preserves active charging stops through the role change and sleep, and excludes sourced VBUS from AC reporting. The A2 HID fallbacks require a `PianoUsbHostNub` ancestor, leaving gadget HID services to their existing matches.

Real loans verify the shared S1 domain after a cable swap has dropped the gadget link. Diagnostic loans also require CDC and NCM to be active before the release, so the unattended run can verify their recovery.

## Default-off gates

| Switch | Default and opt-in |
| --- | --- |
| `PIANO_KEXT_PROFILE=usbhost-a2` | Unset for daily builds. Selects the full native list plus the A2 stack and subclass. `PIANO_KC_ONLY=1` runs the collection build and checks without the RAM image stages. |
| `piano-usbhost=4 piano-usbhost-handoff=6 piano-usb-handoff=1` | Each defaults to off. Together they permit the live controller handoff from PianoDwc3 to Apple's xHCI. Select the pinned subclass with `piano-usbhost-a2=1`. A live loan requires shared S1 and a nonzero `piano-usbhost-back` budget. |
| `piano-usbhost-sinkhost=1` | Defaults to 0. Enables the sink-powered case on handoff 6, with a fresh UFP witness. A DFP partner, stale status and unknown power direction close the gate. |
| `piano-power-own-bus=1` | Defaults to 0. Requires `piano-usbhost=3`, `4`, `5` or `6` and `piano-power-policy=1`. Enables the own-bus decision and its diagnostics for fresh UCSI provider or PMIC Rd evidence. An observed Rp takes precedence. |
| `piano-usbhost-test-loan=1` | Defaults to 0. With handoff 6, it selects the existing one-shot diagnostic loan and requires the root sysctl request. Use a separate boot for the real gate. |

The DSP controls sourced VBUS through its existing board configuration. The separate SC8541 charge-pump and MiPPS charging controls each default to off. See [VBUS witnesses and prerequisites](usb-host.md#64-vbus-h3).

## Prior host qualification on `lead/v131y12t`

- All PianoDwc3, PianoUsbHost, UCSI relay, HID, xHCI and USB console/network helper host tests passed. PianoDwc3's three arm64e objects matched the base byte for byte, and its SDAM/bootflag suite passed all 1,558 checks. A 300-cycle default-off charge replay also matched the base byte for byte.
- `run-charge-host.sh` passed the 80/75, PD buck, battery-field and charge-pump checks. The sanitized simulator passed 45 reference cases and 440 integrated runs, caught all 17 mutants and found zero unsafe episodes in either 1,000-episode fuzz run. The added own-bus queue cancellation check passed in the final contract-link suite.
- PianoPower, PianoUsbHost, PianoDwc3 and the USB stand-in compiled for arm64e with zero unresolved imports. PianoXhciHost passed the pinned 427-slot ABI and resolved all 448 imports against the local 27.3 collection. PianoPower used a keyless build, with synthetic MiPPS cases in the host tests.
- The 27.3 daily and A2 collections linked against the local `0070-s0001` kernel and passed `check-native-kexts.py`. Both contain all 26 daily descriptors. A2 adds its codeless stack and xHCI subclass. The linked-metadata check found the SCSI direct-access personality, the three host-nub-scoped HID fallbacks and exactly one nub client. The daily manifest has no A2 stack or nub client. The collection run skipped the separate A830 host suite and compiled the A830 kext in both profiles.
- The privacy scan against `3fcf74f4` reported zero hits and refused files. The range passed `git diff --check`.

## Host qualification on `main`

- The PianoUsbHost, PianoXhciHost and PianoDwc3 host suites passed. The charge suite passed all 440 integrated runs, and its reference and integrated fuzz runs each finished 1,000 episodes with zero unsafe outcomes. The contract-link test also verifies queued PPS cancellation and intact skin telemetry when the own-bus guard takes over.
- The four arm64e kext builds reported zero warnings and unresolved imports. PianoXhciHost passed the pinned 427-slot ABI and resolved all 448 imports. The three PianoDwc3 objects matched `main` byte for byte after the warning cleanup. The PianoPower build used no MiPPS keys.
- Both the daily and A2 27.3 collections linked. The daily collection carried no opt-in host stack. The A2 check found a single host-nub client, the SCSI direct-access personality and all three host-nub-scoped HID fallbacks in the linked metadata.

## Phase U unattended tablet checklist

Hardware qualification is pending.

Use matched 27.3 RAM images. Keep the Mac cable connected for U0–U3 and use the same daily boot arguments and charger policy in every arm except for the explicit overrides below. Use volatile Data (`data-persist=0`), a 600-second backstop (`piano-reset-after=600 piano-reset-to=bootloader`), and the pstore dump companion. Keep the fixed-rate and battery publication settings from the daily profile. Avoid sleep during the loan. Record the image build record, KC hash, full merged boot arguments, `kern.boottime`, the host USB enumeration and the raw console through the return to bootloader.

1. **U0 daily control.** Use the daily KC and omit the host, handoff, own-bus and OTG opt-ins. Verify CDC console and NCM SSH. Capture `ioreg -rc AppleSmartBattery -l`, `pmset -g batt`, and root reads of `machdep.piano_sdam_survey` and `machdep.piano_bootflag`. Require the daily TX-flow log (`piano-usb-net-txflow=1`), the existing PD and battery publication evidence, and zero host loans or A2 nub matches. Read the charge hold state at the actual SOC. Grade the 80% stop and 75% resume only when the sample crosses those thresholds.
2. **U1 profile with boot gates closed.** Repeat U0 with the `usbhost-a2` KC and the same merged argument bytes. Require the same gadget, network and charging evidence, no `PianoUsbHostNub`, no host USB devices and no own-bus diagnostics. The A2 subclass and its libraries may load as dependencies. Their presence is recorded separately from driver matching.
3. **U2 real gate with the Mac as DFP.** Keep `piano-usb>=2 piano-usb-net=1 piano-power=2 piano-power-policy=1 piano-power-ucsi=1`. Apply the overlay below. After CDC and SSH are ready, require a fresh owned snapshot (`PianoUsbUcsiSharedA2c`) with `connected 1 provider 0 partner 1` (DFP). Check `piano-usbhost-role=device` and `piano-usbhost-vbus=sink` on `ioreg -rc PianoUsbHost -l`. Wait at least 60 seconds. Require zero `loan BEGIN` lines and continuous CDC and SSH. In `ioreg -rc PianoPower -l`, require `OwnBusGuard=1`, `OwnBusInput=0` and `OwnBus=0` in the `PianoCharge` dictionary.
4. **U3 bounded diagnostic and recovery.** Reboot U2 with `piano-usbhost-test-loan=1` added. Wait for fresh UCSI, CDC and NCM SSH. From the existing root connection issue `sysctl -w kern.piano_usbhost_loan=1` once. Expect the link to drop. In the retained console require exactly one diagnostic `loan BEGIN`, No-Op PASS, `PianoUsbDmaReadbackA2d` with `readable-covered 1`, `loan HOST`, `loan END reason deadline`, and `loan RETURN device`. Check that the Mac re-enumerates CDC and that SSH reconnects within 60 seconds of reclaim. Re-read `kern.boottime`, the battery state and the SDAM/bootflag OIDs. Require the same boot with no DMA fault, held-controller failure, panic or second loan before the backstop.
5. **U4 real sourced-VBUS loan.** In a separate attended run with the U2 overlay, replace the Mac cable with an OTG adapter and a bus-powered device. Require a fresh provider/UFP witness, a real loan after the gadget link drops, `OwnBus=1` and battery reporting without AC. Repeat for block storage and HID. Require an `IOMedia` service and a mount for storage, and kernel HID events for keyboard and mouse. Unplug or let the budget expire, then verify CDC and NCM recovery after reconnecting the Mac.
6. **U5 real sink-powered loan.** Repeat with a powered PD pass-through dock after its data-role swap. Require a fresh consumer/UFP witness before the loan, working device enumeration, `OwnBus=0`, and the existing charging and battery-health evidence. Repeat with `piano-usbhost-sinkhost=0` and require that the controller stays in device mode.

U2 overlay (apply once per key to the daily profile):

```text
piano-usb-handoff=1 piano-usb-smmu=2
piano-usbhost=4 piano-usbhost-handoff=6 piano-usbhost-a2=1
piano-usbhost-smmu=2 piano-usbhost-probe=1 piano-usbhost-attach=1
piano-usbhost-debounce=1000 piano-usbhost-back=30 piano-usbhost-sinkhost=1
piano-power-own-bus=1
```

A timeout, unexpected loan, failed reclaim or charging protection regression fails the arm. Keep the pstore and host traces for diagnosis. See the [USB host test ladder](usb-host.md#8-hardware-test-plan-and-ladder) for the peripheral runs.
