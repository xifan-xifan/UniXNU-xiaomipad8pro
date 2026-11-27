# Native Darwin drivers for piano

This index follows the login milestones in the [native bring-up plan](../el1-native-plan.md). See the [support matrix](../STATUS.md)
for verified tablet status and the [operator's guide](../USING.md) for the daily profile. The installed v13.1y12q stays at 60 Hz
with Night Shift routing and native panel sleep off. Display, EFI memory, battery and mixed HEVC qualification ran from RAM.
[November 26 evening results](../results-2026-11-26.md#evening)

## Driver designs

| # | Page | Driver(s) | Gate | UniXNU base |
|---|---|---|---|---|
| 1 | [display.md](display.md) | IOMFB from the VM-era display kext, GOP framebuffer adoption, DPU address-only flips and backlight | M2: first frame and login window | `GopFramebuffer`, UniXNU-sheng backlight |
| 1a | [native-gop-display.md](native-gop-display.md) | `NativeGopDisplay`: k27 native framebuffer on `/arm-io/gop-display` | M2a: WindowServer frames on the panel | VM-era display kext (`package/NativeGopDisplay/UPSTREAM`) |
| 1b | [soft-metal.md](soft-metal.md) | `NativeSoftGpu` and `NativeSoftMetal`: software composition path for SkyLight and Core Animation | M2a without a GPU driver | VM-era GPU kext and Metal plugin |
| 1c | [backlight.md](backlight.md) | `PianoBacklight`: two KTZ8866 backlight controllers over software I2C, brightness keys and panel blanking | Brightness and display power | UniXNU-sheng `ShengBacklight`, PianoKeyboard `BitI2c.h` |
| 1d | [dpu.md](dpu.md) | `PianoDpu`: zero-copy flips, refresh modes, Night Shift (PCC) and panel sleep on the bonded DPU/DSI path | Native scanout and display power | GopFramebuffer direct flip, ShengUfs display SMMU window |
| 2 | [ufs.md](ufs.md) | `NativeUfs` for `qcom,sm8750-ufshc` | M3: root and Data on disk | `ShengUfs` |
| 3 | [touch.md](touch.md) | `PianoTouch`: NT36532E over GENI SPI, contact tracking, gestures and palm rejection | Login by touch | Project-SunUEFI touch producer and NativeGeniSpi |
| 3a | [keyboard.md](keyboard.md) | `PianoKeyboard`: keyboard and trackpad via Nanosic WN8030 over software I2C, plus the folio hall sensor | Login with the keyboard cover | UniXNU-sheng `Wn8030` and `KeyboardCover` |
| 3b | [stylus.md](stylus.md) | Xiaomi Focus Pen / M80p: static analysis of PianoTouch, PianoBluetooth and PianoPower paths | HOLD: daily items, Goal 11 BT pairing and owner-attended pen hardware | Ubuntu kernel `7f8d6bedde479a01f4a05d6d0581b43c7fd20a01` |
| 4 | [charging.md](charging.md) | `PianoPower`: fuel gauges, the charger PMIC and DSP charger firmware over PAS, GLINK and MCA | Battery reporting and charging | UniXNU-sheng ADSP/GLINK/QRTR, Adreno830 `Sm8750Scm`, Project-SunUEFI PMIH010x |
| 5 | [gpu.md](gpu.md) | `PianoA830`: k27 native Adreno830 KMD, piano board glue and AGXAccelerator for the Metal plugin | M5: GPU-composited desktop | `Adreno830`, `Sm8750Platform` and `AdrenoMetal` |
| 5a | [gpu-userspace.md](gpu-userspace.md) | `NativeAdrenoMetal.bundle`: the Metal plugin with Turnip on Adreno830 ABI v2 | M5: GPU userspace | VM-era plugin and Mesa pin (`tooling/metal/UPSTREAM`) |
| 5b | [gpu-dvfs-thermal.md](gpu-dvfs-thermal.md) | Adreno 830 DVFS ladder, thermal guard and Energy Mode budget analysis | Fix-image HOLD; battery/AC and high-refresh thermal evidence pending | PianoA830, PianoThermal, PianoPolicy and pinned Ubuntu kernel |
| 6 | [wifi.md](wifi.md) | `PianoWifi`: WCN7860 over PCIe, firmware over MHI, QMI, scan, association and the data path | M5: networking | UniXNU-sheng `WifiProbe`, PaganiWifi and Ath12kWifi |
| 7 | [audio.md](audio.md) | `PianoAudio`: four FS19xx amplifiers on TDM, digital microphones and AudioReach over GPR | Speaker and microphone devices | UniXNU-sheng GPR/AudioReach, PianoPower DSP and VM-era audio frontend |
| 8 | [sensors.md](sensors.md) | `PianoSensors`: ambient light and accelerometer through the ADSP hub, including FastRPC and QMI | Automatic brightness and rotation | PaganiSensors and PianoPower DSP/router |
| 8a | [sensors-calibration.md](sensors-calibration.md) | B3/B13 static analysis: factory calibration, real-lux ALS, desktop autorotate and touch | Fix-image HOLD; read-only-first and attended RAM qualification | PianoSensors, PianoAuto, PianoBacklight; Ubuntu kernel `7f8d6bedde479a01f4a05d6d0581b43c7fd20a01` |
| 4b | [thermal.md](thermal.md) | `PianoThermal`: TSENS, thermal protection and CPU frequency over SCMI and the CPUCP doorbell | Thermal and Energy Mode policy | Adreno830 `Sm8750Tsens`, K90 SCMI and UniXNU-sheng `CpuFreqProbe` |
| 4a | [rtc.md](rtc.md) | PMK8550 RTC and calendar offset, plus PianoPower readout | Wall clock across reboots | k27 SPMI reads and PianoPower `SpmiObserver.h` |
| 4c | [charging-policy.md](charging-policy.md) | `ChargePolicy`: source-dependent limits, PD, thermal steps, 80/75 hold and sleep handover | Battery health and charging policy | Ubuntu xiaomi_mca policy |
| 4d | [sleep-wake-audit.md](sleep-wake-audit.md) | Native s2idle ordering, path/check matrix and read-only RAM observation plan | Analysis only; fix-image gate HOLD | Pinned Ubuntu kernel and native IOKit power contract |
| – | [usb-host.md](usb-host.md) | `PianoUsbHost`: Type-C roles, orientation, VBUS and the Apple xHCI integration | USB host and OTG | QcomDwc3, PianoDwc3 and PianoPower DSP |
| – | [usb-otg-qual.md](usb-otg-qual.md) | B7 USB-C OTG path/check matrix and bounded attended RAM protocol | Fix-image HOLD; real-device qualification pending | PianoUsbHost, PianoXhciHost, PianoDwc3 and PianoPower |
| – | [video.md](video.md) | `PianoVideo`: Iris video core, power, firmware and HFI, with a VideoToolbox frontend and native Iris backend | Hardware video decode and encode | VM-era VideoToolbox frontend, PianoPower and PianoA830 |
| – | [displayport.md](displayport.md) | `PianoDp`: USB-C DP Alt Mode, QMP PHY, hotplug events, second DPU path and second framebuffer | External displays | PianoDpu, PianoUsbHost and PianoPower |
| – | [camera.md](camera.md) | `PianoCamera`: front and rear camera control, RAW capture, software ISP and CMIOExtension | Camera apps | Ubuntu camera and CCI reference, PianoTorch and CMIOExtension |
| – | [camera-raw-isp.md](camera-raw-isp.md) | B11 full-resolution RAW10, DMA/SMMU, CPU/Metal and hardware ISP, AE/AWB and app verification analysis | Fix-image gate HOLD; daily path unchanged | Pinned Ubuntu camera/CAMSS reference and native camera sources |
| – | [torch.md](torch.md) | `PianoTorch`: PM8550 flash LED control, read-back, time limit and thermal guard | Rear flashlight | PianoPower `SpmiObserver.h` and PianoBacklight |
| – | [ir.md](ir.md) | `PianoIR`: opt-in SE15 IR remote transmitter | Built and host-tested | PianoTouch GENI SPI and PianoDwc3 SMMU helpers |
| – | [bluetooth.md](bluetooth.md) | `PianoBluetooth`: WCN7860 over UART, shared rails with PianoWifi and HCI transport to macOS | Keyboards, mice and audio | UniXNU-sheng `BluetoothProbe` / `WcnPower` and PianoWifi |
| – | [usb-device-debug.md](usb-device-debug.md) | `PianoDwc3`: USB device mode with console, monitor and user pipe | Debugging and maintenance | Native gadget core and Project-SunUEFI takeover order |

## Implementation and qualification notes

- **Display pipeline and refresh.** [M2 cache clean](dpu-m2-analysis.md), [M3 modes](dpu-m3.md), [144 Hz qualification](dpu-m3-144.md),
  [144 Hz bandwidth and MM0 vote](dpu-m3-bandwidth.md), [DPU clock and DRV-2 readback](dpu-clkread.md), [120 Hz composition throughput](m120-throughput.md) and
  [drawable presentation feedback](presented-time.md); [27.3 120 Hz swap / frame-info static analysis](m120-swap-frameinfo-analysis.md) (both fix-image gates HOLD).
- **Display colour and power.** [M4 Night Shift PCC](dpu-m4-pcc-colour.md) and [M5 panel sleep and wake](dpu-m5-analysis.md).
- **Storage and memory.** [Data concurrency and durability](data-store-m4-m5.md), [APFS write-back eligibility](m5-apfs.md),
  [RAM16G GPU memory](ram-extra.md), [GPU tier workload and crash analysis](ram16g-tier-crash.md) and [RAM16G ownership proof](ram16g-owners.md).
- **Audio and video.** [Audio duplex and input wake](audio-duplex.md), [VideoToolbox cancellation analysis](video-hevc-cancel.md),
  [Iris hardware encode static analysis](video-encode.md) (historical B10 fix-image gate),
  [Iris E1 encode self-test integration](video-encode-e1.md) and
  [E1 720p/1080p scaling and Apple 27.3 E2 contract analysis](video-encode-e2-analysis.md)
  (E1 30 fps RAM observe ready; E2 implementation image HOLD).
- **Networking and USB.** [Wi-Fi menu](wifi-menu.md), [Wi-Fi toggle and rejoin](wifi-toggle-rejoin.md),
  [WCN7860 Wi-Fi / Bluetooth coexistence](wifi-bt-coex.md), [USB debug network TX flow control](usb-net-reset.md)
  and [USB host2 integration and Phase U qualification](usb-host2-qualification.md).
- **Sensors and power.** [Auto-rotate](auto-rotate.md), [charging over the buck path](charging-buck-session.md),
  [Android-reference brightness and CPU ceiling](android-reference-controls.md), [PMK8550 power-on history](pon-history.md) and
  [27.3 BH-2 powerd storage analysis](power-bh2-powerd-analysis.md) (storage fix-image gate HOLD).

## Common template

Use these headings for a driver design page.

1. **Purpose and gate.** What the driver does and which login milestone (M1..M5 of plan §5) it unblocks.
2. **Hardware facts.** Register blocks, IRQs, SMMU SIDs, clocks/regulators/GDSCs, firmware files. Every fact has a
   source (DT node, dossier section, kernel file). Mark each fact *verified* (read from source/live DT), *inferred*, or
   *needs-boot*.
3. **Ubuntu as the specification.** Pin the reference revision and cite the exact files and functions for the sequence,
   registers and timings. The Ubuntu release's `kernel/source.json` identifies its Linux revision.
4. **UniXNU template to build on.** Identify the existing component, its pinned commit and the delta for piano.
   [Upstream macOS 27.3 board reference](../upstream/2026-11-10-unixnu-macos-27.3.md)
5. **IOKit family / ABI.** The class the driver publishes and the family it binds to (IOMFB 27.x, IOBlockStorageDevice,
   IOHIDFamily, IOPMPowerSource, IOGPU/AGX).
6. **Boot args.** UniXNU 27.3 naming (no `sheng-` prefix), default value, what each value does.
7. **Kernel archive / component.** Name in the piano board's `[kernel] archives` / `components` (shape of UniXNU-sheng
   `os_overrides."macos-27.3-26C5035r"`), started by `Stage7Probe`.
8. **Hardware test plan.** Identify the milestone, observations, pass criteria and fallback. A RAM boot takes priority over
   the installed profile for that boot.
9. **Fallback.** What runs if the driver is disabled or fails.
10. **Open questions.** Numbered, each with how it gets answered (source read, VM test, needs-boot).
11. **Licence note.**

## Rules that apply to every page

- **Licence.** Linux drivers (GPL-2.0) are read as the specification only. Pages describe behaviour and cite
  `file:function`; no Linux code is pasted into pages or into Darwin sources. Darwin code is written fresh, or derived
  from UniXNU components under their own licences (check each repo's `LICENSE` before copying). The user's own
  userspace (for example `device/piano-thp`) is checked for its SPDX header before reuse.
- **Privacy.** Keep credentials, keys, device identifiers, network details and per-unit calibration out of published pages.
- **Qualification.** Start with read-only or output-only checks on a RAM image. Document the fallback and the changes to the
  profile required for each level.
- **Boot-arg gates.** Driver pages document the arguments and test levels. The daily profile enables qualified paths and
  keeps experimental display switches off. [Installed profile](../USING.md#2-the-installed-profile)
