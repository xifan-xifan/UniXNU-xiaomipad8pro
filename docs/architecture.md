# Native macOS architecture

The Xiaomi Pad 8 Pro (`piano`, SM8750) runs macOS 27.3 (`26C5035r`) as Mac17,11. ABL starts the bundled UEFI firmware.
DarwinBoot loads the kernel collection, ADT, trust cache and Data seed from the UniXNU payload. XNU runs at EL1 with Gunyah
at EL2. The installed payload is on the ESP. A RAM boot supplies the payload in the boot image and uses volatile Data by
default. [27.3 line](k273-line.md) · [Installed layout and boot chain](USING.md#1-what-runs-where)

The kernel collection combines the relinked native kernel, selected Apple entries from the `mac17j` base, and **27 native
kexts** from the default manifest. The native kext rule covers the repository additions. The build validates the descriptors
and checks the link log for VM-era bundles. Optional profiles have their own collection wiring, including the extra USB host
stack entries. [Native kext manifest](../configs/k27-native-kexts.txt) · [Native kext rule](bringup-debugging.md#5-the-native-kext-rule-2026-11-11) · [Build reference](building.md)

## Native kexts

| Kext | Role |
| --- | --- |
| `ARMPlatformStandIn` | Supplies AppleARMPlatform classes for graphics dependencies and a diagnostic-data stand-in on IOResources. [Identity design](identity.md) |
| `NativeUfs` | Exposes the UFS root windows and backs the persistent Data store. [UFS driver](drivers/ufs.md) |
| `NativeRamBlock` | Presents the Data disk from a RAM seed, with changed pages backed by NativeUfs when persistence is enabled. [Data store](data-store.md) |
| `NativeGopDisplay` | Publishes the framebuffer and graphics nub. It can CPU-copy into the GOP framebuffer or hand a surface to PianoDpu for a flip. [Framebuffer driver](drivers/native-gop-display.md) |
| `PianoA830` | Adreno 830 kernel driver and AGX accelerator used by the Metal plug-in. [GPU driver](drivers/gpu.md) · [GPU userspace](drivers/gpu-userspace.md) |
| `PianoBacklight` | Controls the panel's two KTZ8866 backlight controllers over software I2C. [Backlight](drivers/backlight.md) |
| `PianoDpu` | Surveys the Display Processing Unit and drives bonded DPU/DSI scanout, flips, refresh modes, PCC colour and panel power. [DPU](drivers/dpu.md) |
| `PianoDwc3` | USB device-mode controller for the debug console and optional user pipe, HID and network functions. [USB device mode](drivers/usb-device-debug.md) |
| `PianoInput` | Publishes the power and volume buttons as HID consumer keys. [Driver source](../package/PianoInput/) |
| `PianoKeyboard` | Drives the WN8030 cover keyboard, trackpad and hall sensor. [Keyboard cover](drivers/keyboard.md) |
| `PianoNullEthernet` | Provides a built-in Ethernet stand-in for profiles that need an interface for TimeSync and WindowServer. [Identity design](identity.md) |
| `PianoPower` | Reports battery and charger state, manages the DSP charger path, and relays Type-C port events. [Charging](drivers/charging.md) · [Charging policy](drivers/charging-policy.md) |
| `PianoTorch` | Controls the PM8550 rear flash LED with read-back, a time limit and a thermal guard. [Torch](drivers/torch.md) |
| `PianoIR` | Opt-in SE15 infrared transmitter. Level 1 is a read-only survey. Level 2 adds GENI DMA and an emission-free loopback before enabling administrator sends. [IR transmitter](drivers/ir.md) |
| `PianoCamera` | Drives the front and rear sensors and RAW capture. The userspace camera provider adds software ISP and a CMIOExtension. [Camera](drivers/camera.md) |
| `PianoThermal` | Supplies TSENS temperatures, thermal protection and SCMI CPU-frequency control. [Thermal](drivers/thermal.md) |
| `PianoTouch` | Drives the NT36532E touch panel over GENI SPI, including contact tracking and palm rejection. [Touch](drivers/touch.md) |
| `PianoWifi` | Drives WCN7860 over PCIe, with firmware loading, scanning, association and the native IO80211 path. [Wi-Fi](drivers/wifi.md) |
| `PianoWifiEthernet` | Companion from the same source package that exposes the Ethernet-style interface used in Wi-Fi bring-up. [Wi-Fi bring-up](drivers/wifi.md#historical-wi-fi-bring-up) |
| `PianoAudio` | Presents speaker and microphone devices over AudioReach/GPR on the ADSP, with TDM amplifiers, DMIC capture and a userspace HAL plug-in. [Audio](drivers/audio.md) |
| `PianoUsbHost` | Handles Type-C host-role observation and handoff, with an opt-in Apple xHCI path. [USB host](drivers/usb-host.md) · [OTG qualification](drivers/usb-otg-qual.md) |
| `PianoVideo` | Powers and boots the Iris video core and manages native HFI sessions behind the VideoToolbox frontend. [Video](drivers/video.md) |
| `PianoSensors` | Uses FastRPC and QMI to expose ambient light and accelerometer streams from the ADSP. [Sensors](drivers/sensors.md) |
| `PianoDp` | The linked level 1 path surveys USB-C DisplayPort hardware and decodes port events read-only. D0 host models cover EDID/CTA, AUX/DPCD link training and MSA/TU. External link bring-up and scanout remain open. [DisplayPort](drivers/displayport.md) |
| `PianoBluetooth` | Drives WCN7860 Bluetooth over UART and shares rail ownership with PianoWifi. [Bluetooth](drivers/bluetooth.md) |
| `PianoPolicy` | Routes cover, keyboard wake, power key and Low Power Mode policy to the platform drivers, with support from a userspace agent. [Policy](policy.md) |
| `SoftwareAES` | Supplies the software `IOAESAccelerator` required for AppleImage4 startup and IOKit settling. [Source provenance](../package/SoftwareAES/UPSTREAM) · [Identity design](identity.md) |

## Display, GPU and media paths

- **PianoDpu.** M2 adds an opt-in scanout cache-clean skip when the write-combined backing is verified, with a full clean as
  fallback. M3 defines 60, 90, 120 and 144 Hz timings and verifies the selected fixed mode. M4 routes Night Shift through
  PCC coefficients. The reference daily build uses 60 Hz flips with refresh switching, Night Shift routing and native panel
  sleep off. Fixed 120/144 Hz scanout and the unlocked Night Shift sweep have RAM qualification.
  [M2 cache policy](drivers/dpu-m2-analysis.md) · [M3 refresh modes](drivers/dpu-m3.md) · [M4 colour](drivers/dpu-m4-pcc-colour.md) · [Support status](STATUS.md)
- **PianoA830.** `A830GpuTier` can admit eligible buffer objects to write-combined Region1 slabs after reservation handoff
  and checks. The tier is **off by default** (`piano-gpu-tier=0`). Mode 1 tries the tier for buffers of at least 2 MiB
  and mode 2 extends eligibility to smaller non-contiguous buffers. Private-tier RAM qualification is recorded separately
  from installed GPU status. [GPU arguments](drivers/gpu.md) · [Region1 design and qualification](drivers/ram-extra.md)
- **PianoAudio.** MIPLAY adds the optional Dolby/MiSound post-processing subgraph inside speaker protection. If that subgraph
  fails, the protection graph can run without it. The opt-in `piano-audio-spk-probe=1` runs a silent protection probe with
  live per-amplifier V/I feedback. The November 27 RAM run exercised the probe and MIPLAY together.
  [Audio design and probe results](drivers/audio.md) · [Duplex and input wake](drivers/audio-duplex.md)
- **PianoVideo.** On VideoToolbox AVC cancellation, accepted asynchronous frames receive terminal `Demt` dispositions before
  the `dinv` reply clears the guest callback target. The fix has a host regression test and passed November 27 RAM
  qualification on the standard XNU arena. The reference daily image predates that fix.
  [VideoToolbox cancellation](drivers/video-hevc-cancel.md)

The ESP carries the kernel collection and boot payload. The installed root lives on `linux_rootfs`, and the persistent Data
store uses `linux_data`. Staging adds the repository's userland and launchd jobs to the seed and records the trusted Mach-Os
in the trust cache. [Installed profile](USING.md#2-the-installed-profile) · [Userland](userland.md)

Use the [driver index](drivers/README.md) for implementation details and the [support matrix](STATUS.md) for current
qualification. [Hardware backlog](BACKLOG-hardware.md)
