# UniXNU-piano

Native macOS 27 on the Xiaomi Pad 8 Pro (`piano`, Snapdragon 8 Elite SM8750), as a [UniXNU](https://github.com/UniXNU/UniXNU)
device repository: https://github.com/lenzhang/UniXNU-xiaomipad8pro (formerly lenzhang/UniXNU-piano; the local
checkout keeps the name UniXNU-piano). The 27.3 line's k273 XNU build (the k27 source tree with `patches/k27` 0053-0066 and
`patches/k273`) runs as the HLOS at EL1, with
Gunyah staying at EL2. Boot chain: ABL → BootShim → Mu-Silicium UEFI (the FD, piano-uefi's `pianoPkg` with this repository's
deltas) → the UniXNU payload and menu → DarwinBoot → XNU. UniXNU core, Mu-Silicium, piano-uefi, DarwinBoot and the SoC module
are pinned dependencies (`unixnu.lock`, `feeds.toml`), not forks.

Related public repositories: [UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) is a placeholder for the
SM8750 pieces to be shared later (the code still lives here, and the build pins another SM8750 module); [veil](https://github.com/lenzhang/veil)
is the frozen archive of the earlier Linux + Gunyah guest path. The map of repositories, local trees and rules:
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md).

## State (2026-11-24)

Native macOS **27.3 (26C5035r)** boots on the tablet from power-on as well as from a RAM image. The current daily
install is **v13.1y12d** (2026-11-24). The machine identifies as Mac17,11, shown as "Xiaomi Pad 8 Pro" with a
"Snapdragon 8 Elite" chip and a 13 GiB RAM window. Root is on `linux_rootfs`; the owner's Data store is on
`linux_data`. High Performance mode is the daily setting. The Android-aligned CPU/GPU frequency curve and the PD
buck charging path are on by default; the charge pump stays off.

A USB-C debug console (`package/PianoDwc3`, `tooling/native/usb-console.py`) and ssh over the cable are available;
the monitor's `reset fastboot` returns the tablet to ABL without buttons. Everyday development uses `fastboot boot`
RAM images, which take priority over the installed copy.

### Hardware support

Statuses are from tablet runs, not from host-only analysis or "built but not run" work. **Works** means verified on
the installed daily image (v13.1y12d) unless a note says otherwise. **Partial** means it works with documented gaps.
**RAM-qualified** means verified on a RAM boot but not present, or not yet qualified, on that installed image.
**Not supported**, **deferred** and **untested** are what they say.

| Area | Status | Notes |
| --- | --- | --- |
| Display / GPU (Adreno 830) | Works | The login window and the desktop (Dock, menu bar, Finder, Control Center) composite on the GPU. The daily image runs at 60 Hz. |
| 120 Hz | Partial (RAM) | A 120 Hz mode starts and is published (3200x2136). Measured presentation is not a solid 120 fps yet (idle cadence around 90 fps; a 3D bench around 50 fps). The installed daily profile stays at 60 Hz. |
| Touch | Works | Native multitouch has landed: tap, drag, pinch and rotate were verified end-to-end on RAM; daily images publish the native touch screen. |
| Keyboard cover | Works | Keys and the touchpad work on the installed system. |
| Buttons and backlight | Works | Power and volume keys, and backlight steps, run on the daily image. ALS-driven auto-brightness is present. |
| Charging and battery | Partial | Battery percentage and 5 V charging work on the installed image, with the 80 % health hold (resume at 75 %) unchanged. On a computer USB-C port the firmware may request 3 A and then settle lower when VBUS sags (around 4.72 V, roughly 1.8 A). The PD buck path is on by default in v13.1y12d. The charge pump and MiPPS stay off until an attended official-charger test. A newer ladder / MiPPS state machine (mipps2) passed a 5 V smoke test on RAM image y12f — ICL never dropped to 100 mA, no 9 V step on that cable, MiPPS idle — but is not installed yet. |
| Wi-Fi | Partial | The installed system joins at boot (WPA2, DHCP, DNS) and, on recent daily qualifications, comes back after radio off/on, a link drop, and simulated involuntary losses. An office-network (many access points) fix is in the tree; a real office-network test is still pending. |
| Speakers | Works | Playback, system volume and mute work on the installed system. |
| Microphone | Partial | A microphone path exists and a quiet-room noise-suppression check passed on RAM; daily microphone quality and speaker-protection work are still in progress. |
| Bluetooth | Partial | Discovery has been seen on an installed system. Pairing a keyboard or mouse, and Bluetooth audio, are not reliably working yet (connect timeouts and radio issues remain open). |
| Sleep and wake | Partial | Timer-wake suspend-to-idle passed on the v13.1y12d RAM qualification (Wi-Fi and the USB gadget recovered; the lock screen appeared after wake). Lid-close and power-button sleep are not fully signed off. |
| Cameras | Partial | Front and rear cameras enumerate, and colour frames through AVFoundation have been demonstrated. Hardware encode is not built (recordings use the software encoder). Some stock-app and FaceTime gaps remain. |
| Hardware video decode | Partial | Stock QuickTime has played AVC and HEVC fixtures, including a session on an installed image. Concurrent sessions, aerial wallpaper, and some 4K / HEVC cases are only partly qualified. |
| Safari GPU / WebGPU | RAM-qualified, not installed | With the GPU addon view, Safari's GPU process can open the accelerator and WebGPU works (RAM runs safari1b and y12f). This is not on the installed y12d image. After y12f, the next install candidate is y12f2 (the addon view without the family1 Mac2 change). |
| Chrome WebGL / Mac2 | Not supported | On y12f, Chrome started on the family1 (Mac2) plugin and the tablet reset to the bootloader about 11 seconds later, with no panic or GPU error on the console. y12f will not be installed as-is. Chrome stays on software rendering until a later RAM test captures reset evidence. |
| OS switcher | RAM-qualified (read-only) | On y12f, and on an earlier y12e RAM run, the boot-flag sysctls are registered with writes off: the flag byte read `0x00` three times, a write was refused (`EPERM`), and the byte stayed `0x00`. A full switch to Android has not been tested. This is not in the installed y12d image. |
| USB device console | Works | The USB-C debug console, a root shell and ssh over the cable work. |
| USB host / OTG | Not yet | Host-mode code exists but has not been run with a real device on the tablet. |
| DisplayPort | Not yet | A fake second display was exercised on RAM. A real DisplayPort partner has never been plugged on a native boot. |
| Sensors and auto-rotate | Partial | ALS (auto-brightness) runs on the installed system. Auto-rotate is off in the current daily profile; parts of the chain have been demonstrated on RAM but not signed off end-to-end. |
| IR blaster | Not supported | No native driver yet. |
| Stylus | Untested | |
| NPU | Deferred | No native driver. Core ML / Apple Neural Engine is not possible on this SoC. |
| Fingerprint | Deferred | There is no Secure Enclave, so Touch ID is not possible. A non-SEP unlock path has not been started. |
| Software restart | Known issue | Earlier 27.3 boots ended in a 30 s `Halt/Restart Timed Out` panic. This has not been re-qualified as fixed on the current daily image. |
| Login notifications | Known issue | Background Items notifications for `piano-*` helpers still appear at login. |

Per-driver pages, boot arguments and test levels: [docs/drivers/README.md](docs/drivers/README.md) and
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md). Using, updating and recovering the installed system:
[docs/USING.md](docs/USING.md). Earlier public write-ups include
[docs/results-2026-11-19.md](docs/results-2026-11-19.md) and
[docs/results-2026-11-17.md](docs/results-2026-11-17.md).

## Layout

| Path | What |
| --- | --- |
| `patches/k27/` | the k27 kernel patches 0053-0066 on top of the k27 source tree (xnu-12377 + the baseline series 0001-0052) |
| `patches/k273/` | the 27.3 line's kernel series on top of `patches/k27` |
| `patches/k27-kc/` | the KC builder's native placement (best fit, relocation-free constants in `__TEXT`), applied by hand to the k27 tree ([docs/kc-space.md](docs/kc-space.md)) |
| `patches/xnu/` | the device series of the UniXNU macOS 27.3 line (empty: none yet) |
| `package/` | the native kexts; each is inert without its boot argument |
| `configs/` | the native kext list (`k27-native-kexts.txt`), the ADT identity and display node (`k27-adt.toml`, `k27-gop-display.toml`), the 27.3 line's kernel config |
| `dts/piano.dts` | piano's hardware for the ADT |
| `tooling/` | `k273/build-k273-image.sh` (the 27.3 kexts → kernel collection → ADT → RAM images, `K273_KERNEL`), `k273/build-k273-esp.py` (the installed 27.3 ESP), `kc/` (the KC build), `adt/`, `bootimg/` (RAM images and their verification), `native/` (the test loop, the USB console, input), `persist/` (the installed copy), `firmware/` (the FD) |
| `firmware/` | deltas to piano-uefi's `pianoPkg`, Mu-Silicium and UniXNU |
| `docs/` | bring-up notes, driver pages (`drivers/`), plans and evidence |
| `logs/`, `artifacts/ARTIFACTS.md` | committed verification logs; SHA-256 of built artifacts and inputs |
| `private/`, `out/` | ignored: keys, per-unit calibration; build outputs |

## Build

Inputs: the k27 tree, a local kernel tree the build only reads (its place is `PIANO_K27_TREE`, in the environment or in
`~/.config/piano/env`), the pinned UEFI checkouts of `unixnu.lock`, and the OS image inputs of `out/inputs/` (recorded in
`artifacts/ARTIFACTS.md`, never committed).

```sh
tooling/k273/prepare-k27-tree.sh                             # the 27.3 tree and its relinked kernel; prints K273_KERNEL's path
tooling/unixnu.sh build firmware --skip-preflight           # the FD
K273_KERNEL=<path printed by prepare-k27-tree.sh> tooling/k273/build-k273-image.sh
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

Only the 27.3 line is built. The retired `tooling/k27/build-native-image.sh` 27.0.1 path is not used. **Layout note
(2026-11-14):** the root is now on `linux_rootfs` and Data on `linux_data`; `build-k273-image.sh`'s `K273_ROOT=k273` default
still emits the superseded `native-ufs-root-part=linux_data`, so do not use that default for the current layout. The installed
ESP recipe uses `build-k273-esp.py --root-part linux_rootfs` ([docs/persistent-boot.md](docs/persistent-boot.md) §13.8).

Details, the boot arguments and what is read from the k27 tree: [docs/bringup-debugging.md](docs/bringup-debugging.md) §4.

## Boot and test

Everyday development uses RAM boots: `fastboot boot` of a RAM image, which takes priority over the installed copy, with an
unattended loop that boots a test, resets and reads the test's log back (`tooling/native/native-loop.sh`,
[docs/bringup-debugging.md](docs/bringup-debugging.md) §3). The installed copy ([docs/persistent-boot.md](docs/persistent-boot.md))
writes only the regions that document lists, after verified backups of each, with a read-back check after every write and a
rollback for each step; it never writes firmware partitions. Do not flash boot or firmware partitions by hand.

## The native kext rule

The native kernel collection links only this repository's kexts (`configs/k27-native-kexts.txt`). `tooling/k27/check-native-kexts.py`
enforces it on every build, before and after the link: no descriptor from elsewhere, none of the VM-era paravirtual kexts.

## Where to read next

- [docs/USING.md](docs/USING.md): using, updating and recovering the installed system.
- [docs/drivers/README.md](docs/drivers/README.md): every driver, its levels and its state.
- [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md): repositories, local layout, hard rules.
- [docs/bringup-debugging.md](docs/bringup-debugging.md): the boot chain, debugging without a screen, the test loop, rebuilding.
- [docs/BACKLOG-hardware.md](docs/BACKLOG-hardware.md): the hardware still to bring up.
- [docs/kc-space.md](docs/kc-space.md): the kernel collection's room for native kexts, the run-time firmware loader.

## Licence and contents

Code here is BSD-2-Clause-Patent, Copyright (c) 2026 Leo Zhang, unless a package's `LICENSE`, a file's SPDX header or an
`UPSTREAM` file says otherwise (third-party parts keep their own holders and licences).
Vendor firmware, partition images and Apple payloads (kernel collections, trust caches, IPSW-derived files) are never in this
repository; neither are secrets, keys, per-unit calibration, local network or storage details, or machine names.
