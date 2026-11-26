English | [简体中文](README.zh-CN.md)

# UniXNU-piano

Native macOS 27 on the Xiaomi Pad 8 Pro (`piano`, Snapdragon 8 Elite SM8750), as a [UniXNU](https://github.com/UniXNU/UniXNU)
device repository: https://github.com/lenzhang/UniXNU-xiaomipad8pro (formerly lenzhang/UniXNU-piano; the local
checkout keeps the name UniXNU-piano). The 27.3 line's k273 XNU build (the k27 source tree with `patches/k27` 0053-0076 and
`patches/k273`) runs as the HLOS at EL1, with
Gunyah staying at EL2. Boot chain: ABL → BootShim → Mu-Silicium UEFI (the FD, piano-uefi's `pianoPkg` with this repository's
deltas) → the UniXNU payload and menu → DarwinBoot → XNU. UniXNU core, Mu-Silicium, piano-uefi, DarwinBoot and the SoC module
are pinned dependencies (`unixnu.lock`, `feeds.toml`), not forks.

Related public repositories: [UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) is a placeholder for the
SM8750 pieces to be shared later (the code still lives here, and the build pins another SM8750 module); [veil](https://github.com/lenzhang/veil)
is the frozen archive of the earlier Linux + Gunyah guest path. The map of repositories, local trees and rules:
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md).

## State (2026-11-26)

Native macOS **27.3 (26C5035r)** boots on the tablet from power-on as well as from a RAM image. The current daily
install is **v13.1y12q** (2026-11-26; install check 75 PASS / 0 FAIL), with Data concurrent mode and USB debug network TX flow control on by default; display fixes stay behind switches that default to off. The machine
identifies as Mac17,11, shown as "Xiaomi Pad 8 Pro" with a
"Snapdragon 8 Elite" chip. Root is on `linux_rootfs`; the owner's Data store is on `linux_data`. High Power is the daily
Energy Mode. The Android-aligned CPU/GPU frequency curve and PD buck charging (9 V) are on by default; the charge pump
stays off.

What works on the installed system today: boot from disk with the owner's data, the display at 60 Hz, the native touch
screen, the keyboard cover and its trackpad, Wi-Fi (join at boot, off/on, rejoin after a link loss), speakers, battery
percentage, 5 V and 9 V PD charging with the 80 % hold, automatic brightness, Metal on the Adreno 830 with paged GPU
memory and 4 GiB GPU windows (Core Image on 4032-pixel photos), Photo Booth and camera recording, hardware video decode in
QuickTime and Safari (H.264 up to 1080p), Safari WebGL 2 and WebGPU, and Chrome with GPU acceleration (WebGL, GPU compositing,
smooth scrolling and video, with its GPU sandbox on) and, since v13.1y12m, Chrome WebGPU without flags (kernel patch 0075, a
47-bit user address space). Also on the daily line since v13.1y12m: the cover and power-key sleep policy (closing the cover on
the charger locks without sleeping; the power key sleeps and wakes), native screenshots from the display driver, and the
Bluetooth LE event-mask fix. The installed system uses a 13 GiB memory window. Not yet: Bluetooth pairing (needs a hands-on
test), disk write-back caching, charge-pump fast charging, 120 Hz in daily use, USB host devices and external displays. In progress:
native panel sleep, Night Shift and refresh switching (built, off by default, in qualification), Bluetooth keyboards and mice,
closing the cover on battery (attended check), disk speed, auto-rotate, and the install guide for other units.

Kernel patch 0076 lets the USB debug shell and other updated helpers read the full boot line, including arguments past
the legacy 1024-byte cut. Pending tablet checks: the Night Shift colour fix (the blue tint on qualification boots) and the native panel sleep DSI fix, both in v13.1y12s.
CI now runs the Adreno830 GPU host suite with pinned public kits. [Afternoon results](docs/results-2026-11-26.md#afternoon-storage-power-cut-tests-120-hz-ci)

### Hardware support

Every status comes from a run on the tablet. "Works (installed)" means verified on the system installed on the
tablet's disk (the build named in the row); "RAM only" means verified on a RAM boot image only.

✅ fully supported · 🟡 partial / in progress · ❌ not supported

| Feature | Status | Verified (build, date) | Note |
| --- | --- | --- | --- |
| Boot from disk, persistent install | ✅ Works (installed) | v13.1y12q, 2026-11-26 (install check 75/0) | Boots from disk. Installs write only changed parts after backups, verify them by reading them back, and support rollback to the previous build. |
| Display | ✅ Works (installed), 60 Hz | v13.1y12q, 2026-11-26 | PianoDpu flips on the boot framebuffer's pipes and native screenshots are on; colours on the installed desktop are correct (webcam check after the install). Native panel sleep, Night Shift and refresh switching are off by default. Night Shift routing turned the desktop blue on qualification boots: the colour-matrix (PCC) coefficients were written with the cross terms transposed and negative values in sign-magnitude form, which the hardware reads as two's complement. The fix (v13.1y12s) passes the host tests; the tablet check is pending. The native panel sleep fix (DSI controller state after wake) is also pending its tablet check. |
| 120 Hz | 🟡 Partial | v13.1y12p and v13.1y12q test profiles, 2026-11-26 | macOS offers 120 Hz on the fixed-120 profile and both panel links scan at 120.0 Hz. The profile is off by default. Apps reach roughly 58–60 fps. Swaps take about 13 ms against an 8.33 ms period due to GPU-fence wait and vblank acceptance. A fix is under analysis. |
| Touch screen (native) | ✅ Works (installed) | v13.1y12l, 2026-11-26; gestures on RAM touch-native3, 2026-11-23 | A native built-in Touch Screen bound to the display: tap, long press (right click), pan and scroll in scroll views, pinch, rotate, smart magnify, palm rejection, 90 degree axes (44/0 on RAM). Installed: published from boot and acquired by the system. Long use by hand on the native path is pending. |
| Keyboard cover and trackpad | ✅ Works (installed) | owner hands-on, v12, 2026-11-19; v13.1y12l, 2026-11-26 | Typing and the trackpad work; the cover is known to the login window. |
| Wi-Fi | 🟡 Works (installed), partial | v13.1y12l, 2026-11-26; v13.1x, 2026-11-21 | Joins at boot (WPA2, DHCP, DNS); radio off/on from the switch rejoins; a forced link loss rejoins within seconds; country code set. Pending: joining by clicking a network row by hand, auto-rejoin after a hotspot goes away and comes back, and a multi-access-point office network. |
| Bluetooth | ❌ Not yet (in progress) | owner session on an installed v13.1 build, 2026-11-20; v13.1y12m, 2026-11-26 | Discovery works; a Bluetooth LE connection timed out (HCI 0x08) before the link was ready, so keyboards and mice did not pair. The LE event-mask fix is now on the daily line (v13.1y12m: the controller is ready and the mask is set at the Bluetooth daemon's start); pairing a keyboard and a mouse still needs a hands-on test. The radio coexistence with Wi-Fi (the shared antenna) is under analysis. Bluetooth audio not started. |
| Speakers | ✅ Works (installed) | v13.1u, 2026-11-21 (audible tone) | Playback, volume and mute; speaker protection at gain 243 and the stock Dolby "Movie" processing run on the DSP. Since 2026-11-23, installs are silent by request, so later installs check the output device without a tone. |
| Microphone | 🟡 Partial | v13.1y12l, 2026-11-26; noise reduction on RAM mic-ns4b, 2026-11-23 | The raw "Mic" device (the default input) and a "Mic (noise reduced)" device (RNNoise) are in the audio system on the installed boot. The recording-during-playback stutter was fixed on the installed system (2026-11-21). An app recording (QuickTime) played back by hand is pending. |
| Cameras: Photo Booth | ✅ Works (installed) | v13.1y12l, 2026-11-26 | Live front preview with no prompt; the effects grid opens. |
| Cameras: QuickTime | 🟡 Partial | v13.1y12l, 2026-11-26 | 10 s H.264 movies from the front and rear cameras record (AVFoundation, the QuickTime High preset). A New Movie Recording saved and played back by hand is pending. Encoding is in software. |
| Cameras: FaceTime | 🟡 Not yet verified | permissions pre-granted, v13.1y12l, 2026-11-26 | Both cameras are listed and FaceTime has Camera and Microphone access; a FaceTime self-view has not been checked. |
| Charging: 5 V | ✅ Works (installed) | v13.1y12d, 2026-11-24 | Up to 3 A from a 3 A source (input limit 3000 mA; on a computer port the input settles near 1.8 A when the bus sags to about 4.72 V); net battery charge current positive. |
| Charging: PD 9 V (buck) | ✅ Works (installed) | v13.1y12d, 2026-11-24 | On by default. Fixed 9 V PD on the stock charger: 9 V / 1.6 A (about 14 W, the current the charger firmware requests, as on Ubuntu). PPS 9 V (up to 2.5 A) is built but not run. |
| Charging: 80 % hold | ✅ Works | RAM chg-buck6, 2026-11-17; in every installed profile since | Stops at 80 %, resumes at 75 %. |
| Charging: charge pump / MiPPS | ❌ Not yet | first attended try, 2026-11-24 | Off by default. The first attempt stayed at a 100 mA input limit (an unmapped vendor power object); fixed in the build, the next attended test is pending. |
| Battery percentage | ✅ Works (installed) | v13.1y12k, 2026-11-25 | Percentage, charging state and the gauge read correctly. Cycle count, health and time-to-full fields are not filled yet. |
| Sleep and wake | 🟡 Partial | v13.1y12m, 2026-11-26; RAM v13.1y12m qualification, 2026-11-26 | Suspend-to-idle with a timer wake works; Wi-Fi and the USB link recover; the lock screen appears. The cover and power-key policy is armed on the installed system: closing the cover on the charger locks without sleeping (charging goes on), the power key sleeps and wakes (both shown on the RAM qualification). Closing the cover on battery is still an attended check. |
| Brightness: manual | 🟡 Partial | keys on installed v12, 2026-11-19; slider path on v13.1i | Brightness keys work. The slider path through the system's brightness control works; a hand drag in Control Center is pending. |
| Brightness: automatic | ✅ Works (installed) | v13.1y12l, 2026-11-26 | The light sensor drives automatic brightness; manual moves do not end automatic mode. A real light-change span is pending. |
| GPU: Metal (Adreno 830) | ✅ Works (installed) | v13.1y12l, 2026-11-26 | Our Metal plug-in runs WindowServer, the desktop and apps; ImageIO, Core Image on Metal, MetalKit texture loading and Quick Look thumbnails pass: 30 of 30 graded paths, including the 4032-pixel Core Image and HEIC texture paths that ran out of memory before; Preview opens a 4032-pixel photo. |
| GPU: paged memory | ✅ Works (installed) | v13.1y12l, 2026-11-26 | 4 KiB paged GPU memory (cap 7168 MiB, no fixed pool), no refusals, no page faults; hang recovery by replay on by default. |
| GPU: Android-level frequency curve | ✅ Works (installed) | v13.1y12l, 2026-11-26 | The Android 14-level table up to 1100 MHz with its skin and thermal steps and a KGSL-style governor; the Android CPU table. Sustained 3D bench about 50-54 fps at 734 MHz (RAM, 2026-11-23). |
| GPU: 4 GiB windows | ✅ Works (installed) | v13.1y12l, 2026-11-26 | 64 client windows of 4 GiB in a 256 GiB aperture, 1 GiB per object, no GPU open refused. Fixes the Core Image 4032-pixel out-of-memory paths (Metal image check 30/30). |
| Safari: WebGL 2 and WebGPU | ✅ Works (installed) | v13.1y12o, 2026-11-26 | WebGL 2 and WebGPU passed again. On v13.1y12m, the WebGPU adapter was "adreno" and the test page ran at about 52 fps with correct readback. |
| Safari: hardware video decode | ✅ Works (installed) | v13.1y12o, 2026-11-26; v13.1y12i, 2026-11-25 | H.264 up to 1920x1088 decodes on the Iris video core. The 1080p test again reported zero dropped frames. |
| Safari: HEVC and 4K | ✅ Works (installed), software | v13.1y12j, 2026-11-25; HEVC replayed on v13.1y12o | HEVC 2160p 600/0 at 29.9 fps and H.264 2160p 591/5 on v13.1y12j, in Apple's software decoders. VP9 and AV1 are not supported. |
| Chrome: GPU acceleration | ✅ Works (installed) | v13.1y12o, 2026-11-26; earlier tests on v13.1y12m and v13.1y12l | WebGL measured 37.7 fps through ANGLE on Metal (Adreno 830) with the GPU sandbox on and no flags. Earlier checks covered GPU compositing, canvas 2D at 56 fps, scrolling at 60 fps with no frame over 34 ms, and video playback with 0-1 dropped frames: H.264 and HEVC at 1080p and 2160p, VP9 and AV1 at 1080p (HEVC through VideoToolbox, the others in Chrome's software decoders). Those checks produced no crash report. Chrome can exhaust the RAM-backed Data disk on RAM boots. |
| Chrome: WebGPU | ✅ Works (installed) | v13.1y12o, 2026-11-26 | Without flags, 6 of 6 WebGPU contexts opened on Qualcomm "adreno-8xx", with correct compute results and frame readback. Kernel patch 0075 gives processes a 47-bit user address space. All 17 Chrome processes reported secure V8 sandbox mode and a 1152 GB reservation, resolving the transient v13.1y12m finding. |
| Video playback (QuickTime) | ✅ Works (installed) | v13.1x, 2026-11-21 | AVC High and HEVC Main10 decoded in hardware; about 100 s each with 0 transient or dark frames on the webcam. |
| Energy modes | 🟡 Partial | v13.1y12l, 2026-11-26 | Low Power, Automatic and High Power (the daily setting) set with `pmset` reach the power and thermal policy. The picker in System Settings is not verified by hand. |
| Login window idle CPU | ✅ Works (installed) | v13.1y12l, 2026-11-26 | Fixed: an audio daemon looped at the login window (about 1.3 cores); it is now held until the login. |
| App launches | ✅ Works (installed) | v13.1y12l, 2026-11-26 | Calculator, TextEdit and Notes open their first window in under 2.5 s. |
| USB debug (console, root shell, ssh) | ✅ Works (installed) | v13.1y12q (installed), 2026-11-26 | USB-C debug console, root shell and SSH over the cable work. Patch 0076 lets the shell read boot arguments past the legacy 1024-byte cut. A full TX queue used to drop frames during large single transfers; TX flow control (66/66 exact transfers in its test) is on by default since v13.1y12q. |
| OS switcher (Android / macOS) | 🟡 RAM only, read-only | RAM v13.1y12e, 2026-11-24 | The boot-flag sysctls read the flag; writes are refused by design. A full switch has not been run. |
| USB host / OTG | ❌ Not yet | RAM usb-a2d, 2026-11-18 (empty port) | Host mode starts on an empty port; VBUS supply and a real device are not done. |
| External display (USB-C DP) | ❌ Not yet | none | Design only. |
| Auto-rotate and other sensors | ❌ Not yet | RAM, 2026-11-19 (accelerometer sample) | The light sensor works; auto-rotate is off in the daily profile and in analysis. |
| Hardware video encode | ❌ Not yet | camera recordings, 2026-11-25 | Recordings use the software encoder. |
| IR blaster, stylus, torch | ❌ Not yet | none | Drivers built or designed, not run. |
| Memory (16 GB) | 🟡 Partial | v13.1y12l, 2026-11-26 | The installed system uses a 13 GiB window (13310 MiB; was 8 GiB). At idle: 47 % free, no compression, no swap, no memory pressure. In progress: the GPU memory tier and the driver pools. |
| Internal storage (UFS) | 🟡 Partial | v13.1y12q (installed: concurrent mode live); v13.1y12o (Data A/B) and scratch-window tests, 2026-11-26 | Compared with serial operation on Data, concurrent mode delivered 4.9× the 4 KiB random-read IOPS at QD8 and 5.6× at QD32. At QD8, 1 MiB sequential reads reached 4.8× throughput, 1 MiB sequential writes reached 1.5×, and 4 KiB random writes reached 0.9× IOPS. Mixed-reader p99 fell from 2528 to 269 µs. The reboot canary survived. Concurrent, write-back and queue modes each kept every acknowledged `F_FULLFSYNC` record after a direct reset in scratch-window tests. Concurrent mode is on in the installed v13.1y12q (switched on after the Data mount). Write-back and queue modes stay off. |
| Fingerprint, NPU | ❌ Deferred | none | No Secure Enclave (no Touch ID); no Core ML backend on this SoC. |

Details (status definitions, known gaps, showcase): [docs/STATUS.md](docs/STATUS.md).
Today's tablet results: [docs/results-2026-11-26.md](docs/results-2026-11-26.md) (yesterday's:
[docs/results-2026-11-25.md](docs/results-2026-11-25.md)).

**Showcase:** screenshots and short clips of each feature, taken on the tablet itself, are collected in
[docs/STATUS.md#showcase](docs/STATUS.md#showcase) (first native captures from the installed v13.1y12m: About This Mac, Displays, Battery, Finder, Activity Monitor, Metal 3D, Safari video).

A USB-C debug console (`package/PianoDwc3`, `tooling/native/usb-console.py`) and ssh over the cable are available;
the monitor's `reset fastboot` returns the tablet to ABL without buttons. Everyday development uses `fastboot boot`
RAM images, which take priority over the installed copy.

Per-driver pages, boot arguments and test levels: [docs/drivers/README.md](docs/drivers/README.md) and
[docs/PROJECT-MAP.md](docs/PROJECT-MAP.md). Using, updating and recovering the installed system:
[docs/USING.md](docs/USING.md). Earlier public write-ups include
[docs/results-2026-11-19.md](docs/results-2026-11-19.md) and
[docs/results-2026-11-17.md](docs/results-2026-11-17.md).

## Layout

| Path | What |
| --- | --- |
| `patches/k27-tree/` | our delta of the k27 tree on its pinned upstream (macOS-DroidVM): build scripts, KC builder, kernel series 0050-0054 |
| `patches/k27/` | the k27 kernel patches 0053-0076 on top of the k27 source tree (xnu-12377 + the baseline series 0001-0052) |
| `patches/k273/` | the 27.3 line's kernel series on top of `patches/k27` |
| `patches/k27-kc/` | the KC builder's native placement (best fit, relocation-free constants in `__TEXT`), applied by hand to the k27 tree ([docs/kc-space.md](docs/kc-space.md)) |
| `patches/xnu/` | the device series of the UniXNU macOS 27.3 line (empty: none yet) |
| `package/` | the native kexts; each is inert without its boot argument |
| `configs/` | the native kext list (`k27-native-kexts.txt`), the ADT identity and display node (`k27-adt.toml`, `k27-gop-display.toml`), the 27.3 line's kernel config |
| `dts/piano.dts` | piano's hardware for the ADT |
| `.github/`, `scripts/` | CI (checks, monthly dependency check, builds, hand-made releases: [docs/ci.md](docs/ci.md)), `fetch-pins.py` |
| `tooling/` | `k273/build-k273-image.sh` (the 27.3 kexts → kernel collection → ADT → RAM images, `K273_KERNEL`), `k273/build-k273-esp.py` (the installed 27.3 ESP), `kc/` (the KC build), `adt/`, `bootimg/` (RAM images and their verification), `native/` (the test loop, the USB console, input), `persist/` (the installed copy), `firmware/` (the FD) |
| `firmware/` | deltas to piano-uefi's `pianoPkg`, Mu-Silicium and UniXNU |
| `docs/` | bring-up notes, driver pages (`drivers/`), plans and evidence |
| `skills/`, `tooling/repro/` | the agent guide for another unit and its kit: pins of Apple's inputs, self-fetch, root rebuild checks, host check |
| `logs/`, `artifacts/ARTIFACTS.md` | committed verification logs; SHA-256 of built artifacts and inputs |
| `private/`, `out/` | ignored: keys, per-unit calibration; build outputs |

## Build

The whole path for a new unit (requirements, inputs, build, RAM-boot validation, guarded install, rollback, checks) is the
install guide [skills/install-piano-macos/SKILL.md](skills/install-piano-macos/SKILL.md), written for a coding agent
(Chinese: [SKILL.zh-CN.md](skills/install-piano-macos/SKILL.zh-CN.md)). This section is the
summary; the skill is the reference.

### Host requirements

- **A Mac with Apple silicon (M1 or later). Intel Macs are not supported.** Xcode 27's compiler and linker and macOS 27's own
  tools are arm64-only, and two build steps run arm64e programs they compile. The evidence is in the skill (§1.1).
- Tested: macOS 27.2.1 with **Xcode 27.2** (the app, not only the Command Line Tools: the kernel needs the iphoneos SDK and
  `Kernel.framework`; the kernel links with Xcode's `ld-classic`). Homebrew at `/opt/homebrew` with the packages the skill lists
  (§1.2), `fastboot`, Python 3.11 or later, about 90 GiB free disk, `sudo` in an attached Terminal for the root image steps.
- `bash tooling/repro/check-host.sh` reads the host and prints `ok` / `WARN` / `FAIL` / `GAP` per requirement; continue only with
  `0 FAIL`. Run from a native arm64 shell, not under Rosetta.

### Sources and pins

- This repository plus the pinned dependencies of `unixnu.lock` / `feeds.toml` (UniXNU core, Mu-Silicium, piano-uefi,
  DarwinBoot, the SoC module) and the k27 kernel tree (`PIANO_K27_TREE`, in the environment or in `~/.config/piano/env`; pinned
  in `unixnu.lock`).
- **Fetching the pins:** `python3 scripts/fetch-pins.py` fetches every `unixnu.lock` checkout by commit into `$UNIXNU_RESEARCH`
  (`--verify` only checks them; contributed by SorasakiHinaHusband). The k27 kernel tree is upstream
  [macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) at its pin plus our [patches/k27-tree](patches/k27-tree/README.md):
  `tooling/k27/fetch-k27-tree.sh DEST`, then `PIANO_K27_TREE=DEST`.
- Apple's files are never distributed. `tooling/repro/fetch-ipsw.py` fetches the pinned 27.3 IPSW's members from Apple's CDN and
  the AEA keys from Apple, and checks every file against `tooling/repro/pins.json`.
- Some inputs are still local to the original setup (the k27 kernel tree, the base trust bundle, the Data seed, the Turnip tree,
  the write tooling's single-unit pins): the skill's "Known gaps" lists each with its options.

### Build steps

```sh
bash tooling/repro/fetch-and-build.sh --dry-run   # every command it would run; nothing fetched, built or written
bash tooling/repro/fetch-and-build.sh             # host, inputs, systemos, root, kernel, fd, image (or one step by name)
bash tooling/repro/fetch-and-build.sh --check     # each step's outputs against the pins (no network)
```

The steps it runs, also usable one by one:

```sh
bash tooling/k273/prepare-k27-tree.sh          # the 27.3 kernel tree and its relinked kernel; prints K273_KERNEL's path
bash tooling/firmware/build-fd.sh               # the UEFI FD from the unixnu.lock checkouts
K273_KERNEL=<path printed above> bash tooling/k273/build-k273-image.sh   # kexts -> kernel collection -> ADT -> RAM images
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

- **Kexts and the kernel collection:** the KC links only this repository's kexts (`configs/k27-native-kexts.txt`);
  `tooling/k27/check-native-kexts.py` enforces it before and after the link. Pass line in the KC log:
  `native kext rule: ... all this repository's`.
- **The root image is generated locally, never shipped.** The 20.9 GB root is the IPSW's OS volume (byte for byte, pinned) plus a
  SystemOS window rebuilt by `tooling/k273/make-systemos.sh` and `make-root-image.sh`, checked by `tooling/repro/verify-root.py`
  against the pinned file-level manifest (skill §3.1).
- Only the 27.3 line is built; the retired 27.0.1 path (`tooling/k27/build-native-image.sh`) is not used. Root placement: a new
  unit starts with the root on `linux_data` (`K273_ROOT_PART=linux_data`, skill §3.2); the reference unit's installed ESP uses
  `build-k273-esp.py --root-part linux_rootfs` ([docs/persistent-boot.md](docs/persistent-boot.md) §13.8).

### CI and releases

The FD, the k27 native kernel and our kexts also build on GitHub's hosted runners ([docs/ci.md](docs/ci.md)): pull requests run the
pin fetch, the self-tests and a privacy scan; releases (tagged by hand after a tablet qualification) carry only our binaries, the
recipes, SHA-256 manifests and the install skill. The root image and the kernel collection are always built on the user's side.

### Size limit of a RAM image

A RAM boot image must be **at most 250,000,000 bytes**. ABL loads the whole image into the UEFI's 256 MiB kernel region; a
294,952,960-byte image stayed in fastboot after the download, while 247,767,040-byte images boot
([docs/k273-line.md](docs/k273-line.md) §4). The cap is `IMAGE_MAX_BYTES` in `tooling/bootimg/build-ram-images.py` (which
shrinks the payload's free space when an image would pass it) and is checked again by `tooling/bootimg/verify-ram-images.py`
(both read `PIANO_IMAGE_MAX_BYTES`; do not raise it).

### RAM boot before any install

Every image is first booted with `fastboot boot` (a RAM boot writes no partition) and must pass there before anything is written
to the disk: the console reaches XNU, the USB console answers, the boot returns to ABL by its reset guard, and for a desktop image
the login window, touch, keyboard cover, ssh, Wi-Fi and battery read correctly (skill §4). Installs then go only through
`tooling/persist/maint-session.py`, with backups, read-back checks and a rollback for each step
([docs/persistent-boot.md](docs/persistent-boot.md), [docs/install-esp-v3.md](docs/install-esp-v3.md)).

More: boot arguments and debugging without a screen in [docs/bringup-debugging.md](docs/bringup-debugging.md) (§4 for what is read
from the k27 tree); what works today in [docs/STATUS.md](docs/STATUS.md).

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

- [docs/STATUS.md](docs/STATUS.md): what works, per feature, with the build and date it was verified.
- [docs/USING.md](docs/USING.md): using, updating and recovering the installed system.
- [skills/install-piano-macos/SKILL.md](skills/install-piano-macos/SKILL.md): building and installing on another unit (agent guide).
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

`scripts/fetch-pins.py` was first written by SorasakiHinaHusband (pull request #2).
