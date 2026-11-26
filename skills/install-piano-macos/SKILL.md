---
name: install-piano-macos
description: Guide a coding agent end to end through putting native macOS 27.3 (UniXNU-piano) on ANOTHER Xiaomi Pad 8 Pro (piano, SM8750) — host requirements, fetching the pinned IPSW from Apple and rebuilding every Apple-derived input locally (root image included) with sha256 checks, building the images from this repository, RAM-boot validation with nothing written, the guarded install, rollback, and post-install checks. Use when someone asks to build these images outside the original setup or to install them on a second tablet. Read "Status" and "Known gaps" first: several inputs are still local to the original setup.
---

English | [简体中文](SKILL.zh-CN.md)

# Install native macOS 27.3 on a Xiaomi Pad 8 Pro (piano)

You are the operator. The tablet owner sits next to the tablet and presses its buttons. You run commands on a Mac. Follow the
sections in order. Every step names its command, what it writes, and the line that tells you it passed. **Stop at the first
failure and report it to the owner.** Do not retry a write. Do not work around a refusal.

All paths are relative to this repository's checkout. `tooling/repro/` is the kit this skill calls. Nothing from Apple's IPSW is
ever committed or shipped: every user fetches it from Apple and rebuilds the derived files on their own Mac.

## 0. Status: what a second unit can do today (2026-11-25)

| Section | State for a second unit |
|---|---|
| 1 Requirements | ready: `tooling/repro/check-host.sh` |
| 2 Inputs from Apple | ready: `tooling/repro/fetch-ipsw.py` (every member pinned, AEA keys from Apple) and `boot-dtb.py` |
| 2 Per-device data | partly ready: extracted from the unit through the Ubuntu install's tools (G5) |
| 3 Root image rebuild | ready if you have the k27 tree's two boot shims (G1b): `make-systemos.sh`, `verify-root.py`, a pinned file manifest |
| 3 Kernel, KC, RAM images | **blocked without the k27 tree (G1), the base trust bundle (G2), a Data seed (G3), the Turnip tree (G4)** |
| 4 RAM boot | ready once 3 builds (the `fastboot boot` path takes nothing per unit) |
| 5 Install | **blocked: the write tooling is pinned to the reference unit (G6)**. `maint-session.py` refuses another unit at `map`. Never force it. |
| 6 Post-install checks | ready (the same checks as the reference unit) |

If a row says blocked, finish the sections before it, then report the gap to the owner with its options from "Known gaps".

## 1. Requirements

### 1.1 Host: a Mac with Apple silicon

Run `bash tooling/repro/check-host.sh`. It reads only. It prints one line per requirement: `ok`, `WARN` (untested), `FAIL` (cannot
build here) or `GAP` (a local-only input, see Known gaps). Continue only with `summary: 0 FAIL`.

**Verdict: an Apple silicon Mac (M1 or later) is required. Intel Macs are not supported.** Evidence, taken on the reference build
host (macOS 27.2.1 26B131, Xcode 27.2 27B52):

| Piece | Evidence | Consequence |
|---|---|---|
| Xcode 27 | `file` of `Xcode.app/Contents/MacOS/Xcode`, the toolchain's `clang` and `ld`: `Mach-O 64-bit executable arm64` (no x86_64 slice) | the compiler and linker the build calls (`xcrun clang`, `xcrun ld`) do not run on Intel |
| macOS 27 | `/usr/bin/kmutil`, `/usr/bin/compression_tool`, `/usr/bin/hdiutil`: `arm64e` and `arm64e.x1` slices only | macOS 27 itself runs only on Apple silicon. Only a few tools (`aea`, `codesign`, `newfs_apfs`) still carry x86_64 slices |
| Build steps that execute what they compile | `tooling/k27/build-piano-micns.sh` compiles `plugin_host` with `-arch arm64e` and runs it to load and grade the CoreAudio plug-in. `tooling/metal/check-turnip-a830-host.py` runs arm64e Turnip test processes | an Intel CPU cannot execute arm64e code (Rosetta translates x86_64 to arm64, never the other way) |
| Homebrew prefix | `/opt/homebrew` (Apple silicon's prefix) is hard-coded in `tooling/metal/build-turnip-a830.sh`, `tooling/kc/build-kc-k27.sh`, `tooling/k27/build-native-gop-display.sh`, `build-native-piano-touch.sh`, `tooling/persist/maint-session.py` and the k27 kernel script | Intel Homebrew (`/usr/local`) misses these paths. Some have overrides (`PYTHON315`, `LLVM_NM`, `LLVM_BIN`), not all |
| Not blockers | `ld-classic` (ld64-957, the kernel's linker) is universal (x86_64 + arm64). `fastboot` is universal. `ipsw` has x86_64 releases. `kmutil` is not used: the KC is linked by the k27 tree's Python builder. Ad hoc `codesign` works on either | - |

Caveats: only macOS 27.2.1 with Xcode 27.2 has been tested. Xcode 27 declares macOS 26.6 as its minimum
(`LSMinimumSystemVersion`), so an Apple silicon Mac on macOS 26.6 or later may work, untested (`check-host.sh` says WARN). An
Intel path would need Xcode 26 on macOS 26, the Intel Homebrew prefix in every script, and the two arm64e self-tests removed. It
has never been tried and is not supported. Run every command from a native arm64 shell, not under Rosetta (`check-host.sh` FAILs a
translated shell).

### 1.2 Host software

- **Xcode 27** (the app, not only the Command Line Tools): the macOS SDK with `Kernel.framework` and the iphoneos SDK (the k27
  kernel builds with `SDKROOT=iphoneos`). Run `sudo xcode-select -s /Applications/Xcode.app`.
- **Homebrew** at `/opt/homebrew`. Install: `python@3.15 llvm gnu-sed bison mtools dtc zstd`, `blacktop/tap/ipsw` (the AEA key
  fetch) and the cask `android-platform-tools` (`fastboot`).
- **python3 3.11 or later**. Only the standard library is needed: the Turnip build makes its own venv with pinned meson,
  mako, PyYAML and packaging.
- **Disk:** about 90 GiB free. The IPSW inputs take up to 32 GB, the root image 21 GB, the SystemOS window 6 GB, and the
  builds and images the rest.
- **sudo** in an attached Terminal. Only `make-systemos.sh` (newfs_apfs, ditto with ownership) and `volume-manifest.py` (reading
  0600 root files) need it.
- Run the tablet steps from an attached Terminal session (`open -a Terminal`). A detached process can lose the USB network to
  macOS's Local Network privacy.

### 1.3 The tablet

- A Xiaomi Pad 8 Pro **25091RP04C, 512 GB**. The tooling's layout is the 512 GB model's (G10).
- Stock build **OS3.0.303.0.WPYCNXM** on **slot A**, the same requirement as the Ubuntu project.
- **Bootloader unlocked** through Xiaomi's official process. `fastboot getvar unlocked` must say `yes`.
- **Ubuntu installed first**, with the Ubuntu project's own installer:
  [xiaomipad-8pro-mainline](https://github.com/yzddmr6/xiaomipad-8pro-mainline), `docs/INSTALL.en.md`. That installer:
  - backs up every partition and GPT of the unit (keep this backup; it is your factory way back);
  - creates `linux_rootfs` (60 GiB) and `linux_data` (about 201 GiB), the only space macOS uses;
  - collects the unit's own files: `boot-profile.json`, your own AVB key `boot-signing.pem`, the touch calibration and the sensor
    registry;
  - leaves Android bootable (`userdata` 200 GiB), with `tools/piano-switch.py` and the slot-A Android set to switch back.
  This repository never repartitions and never touches Android's partitions.
- **Cables:** a USB-C data cable from the tablet to the Mac, directly, not through a hub. The same cable carries fastboot, the
  maintenance image's network and the macOS USB console (`tooling/native/usb-console.py --list`).
- Battery above 50 % (`fastboot getvar battery-voltage`). Android remains the charging fallback until macOS charging is checked
  on the unit.
- **The owner's escape:** hold power + volume-down for 15-20 s. This always reaches ABL fastboot, because ABL is never written.

## 2. Inputs

### 2.1 This repository and its pinned sources

```sh
git clone https://github.com/lenzhang/UniXNU-xiaomipad8pro.git UniXNU-piano && cd UniXNU-piano
```

`unixnu.lock` pins every other checkout (UniXNU, Mu-Silicium and its submodules, piano-uefi, DarwinBoot, the SoC layer) by URL
and commit. Clone each `[pin."NAME"]` at its `commit` into one directory and set `UNIXNU_RESEARCH` to it. `tooling/unixnu.sh` and
`tooling/firmware/build-fd.sh` read them there. They are public; the k27 tree is not (G1).

### 2.2 The pinned IPSW from Apple, with AEA keys from Apple

```sh
python3 -I tooling/repro/fetch-ipsw.py --dry-run          # the plan, the tools, the disk space; no network, no writes
python3 -I tooling/repro/fetch-ipsw.py --remove-aea       # fetch, decrypt, decompress; checks every file against tooling/repro/pins.json
python3 -I tooling/repro/fetch-ipsw.py --check            # later: no network, everything still equal to its pin
```

What it does, failing closed at each step:

1. A HEAD request to the pinned URL (`UniversalMac_27.3_26C5035r_Restore.ipsw`, 27206093542 bytes). Apple's
   `x-amz-meta-digest-sha256` must be the pin `1f372fa1...`. Otherwise the CDN serves another file: stop.
2. `tooling/k273/ipsw-members.py fetch` downloads only the K114c identity's 11 members (14 GB of 27 GB) with resumable ranges,
   checking each member's zip CRC-32. Each file is then hashed again and compared with the pin. `BuildManifest.plist` too (`58f18ab4...`).
3. `tooling/k273/decrypt-aea.py` handles the two AEA disk images. The key comes from Apple's key service through `ipsw fw aea --key`
   (the archive header names the key URL). macOS `/usr/bin/aea` decrypts. The key is never written or printed. Plain sha256:
   OS `40d8282e...`, SystemOS cryptex `14330ed9...`.
4. `tooling/k273/decompress-kernelcache.py` makes the KC base `kc/kc273-mac17j.bin`, sha256 `d1003b51...` (the UniXNU
   macos-27.3-26C5035r profile's pin).

Pass: `PASS: every pinned Apple input is present and equal to its pin`. The files land in `out/inputs/k273/26C5035r/` and
`out/inputs/k273/kc/` (ignored by git; never copy them anywhere public).

### 2.3 The boot DTB and the maintenance image (public, from the Ubuntu project)

Download the Ubuntu v1.1.0 release (`piano-ubuntu-v1.1.0-boot.zip`, checked with its `SHA256SUMS.assets`). Unpack it into a
directory and set `PIANO_V110` to it. The file `release/boot/boot-ram-maintenance.img` must hash to `54001a50...`
(`pins.json` `maintenance_image`). Every write session runs in that RAM-only Linux (`maint-session.py maint-boot`). Its DTB
section is the boot DTB every image carries:

```sh
python3 -I tooling/repro/boot-dtb.py "$PIANO_V110/release/boot/boot-ram-maintenance.img"    # -> out/inputs/piano-boot.dtb, 50636eff...
```

### 2.4 Per-device data: only from THIS unit, never from another

None of these files ever goes into a repository, an issue or a chat. Keep them under `private/` (ignored by git) or
`~/.config/piano/`.

| Data | From | How | Used by |
|---|---|---|---|
| Full partition backup + GPTs of every LUN | the unit | Ubuntu INSTALL §2 (before any write) | rollback to factory; `unit-profile.py` |
| AVB key `boot-signing.pem`, `boot-profile.json` | made / read at Ubuntu INSTALL §3 | Ubuntu's tools | signing `boot_a` (G7) |
| Slot-A Android set (stock `boot`, `init_boot`, `vendor_boot`) | the unit's backup | Ubuntu INSTALL §10 (`android-set`) | the Android fallback (`piano-switch.py`) |
| Touch calibration `thp-calibration.json` | vendor `piano_nova_<panel>_thp_config.ini` | Ubuntu `tools/extract-thp-calibration.py`; `PIANO_TOUCH_CALIBRATION` (`docs/drivers/touch.md`) | PianoTouch |
| Sensor registry -> sensor pack | `persist`/vendor sensor files, read only | Ubuntu INSTALL §3 "prepare your sensor registry", then `package/PianoSensors/tools/make-sensor-pack.py` (`docs/drivers/sensors.md` §5b) | PianoSensors (`PIANO_SENSOR_PACK`) |
| Speaker Re25 + protection parameters | `persist` `fsalgo_rdc.bin` + the stock ACDB | `tooling/audio/spk-prot-extract.py re25 / params` (`docs/drivers/audio.md` §12) | PianoAudio (`piano-audio-spk`, optional) |
| Bluetooth and Wi-Fi addresses | `persist` | read only, from the backup | Bluetooth, Wi-Fi |
| Stock firmware (DSP, GPU zap, Wi-Fi, Bluetooth, touch, amplifier preset) | the same on every unit of OS3.0.303; Wi-Fi `board.bin` is per region | the Ubuntu project's `firmware/` tree (locked by name, size, sha256) or the unit's own stock partitions | PianoAudio, PianoVideo, Wi-Fi, GPU, Bluetooth addons |

Never mount `persist` writable. Never use another unit's calibration.

### 2.5 Is this unit's layout the one the tooling knows?

```sh
python3 -I tooling/repro/unit-profile.py --lun0 <LUN 0 MBR+primary GPT from the Ubuntu backup> --lun4 <LUN 4 likewise>
```

It reads two files and compares `linux_rootfs`, `linux_data`, LUN 0 size and `boot_a` with `tooling/persist/common.py`.
`SAME-LAYOUT` means the partitions sit where the tooling expects. The GPT hash still differs (the GUIDs are per unit), so the
install remains blocked by G6. `DIFFERENT` (exit 2) means the unit cannot use this tooling's layout at all. Report either result.

## 3. Build the images (Mac only; nothing touches the tablet)

One entry runs the steps in order. Use `--dry-run` first, then the real run, then `--check`:

```sh
bash tooling/repro/fetch-and-build.sh --dry-run                 # every command it would run; nothing fetched, built or written
bash tooling/repro/fetch-and-build.sh                           # host inputs systemos root kernel fd image
bash tooling/repro/fetch-and-build.sh --check                   # each step's outputs against the pins (no network)
bash tooling/repro/fetch-and-build.sh root                      # or any single step
```

### 3.1 The root image, rebuilt locally (never shipped)

The 20.9 GB root image is the SHNGROOT manifest plus two windows (`docs/k273-line.md` §3):

| Piece | Made by | Check | Reproducible? |
|---|---|---|---|
| OS window (14493417472 B) | the IPSW's `141-22233-021.dmg`, decrypted, unchanged | sha256 `40d8282e...` (pinned) | yes, byte for byte |
| SystemOS window (6442450944 B) | `tooling/k273/make-systemos.sh OS.dmg 141-22145-021.dmg 141-22106-021.dmg out/inputs/k273/root/systemos.img` (sudo): the SystemOS and App cryptexes, the Data template's read-only parts, the boot shims (G1b) | `sudo python3 -I tooling/repro/volume-manifest.py --image out/inputs/k273/root/systemos.img --compare tooling/repro/pins.json`. The file-level manifest digest must be `070934ff...` (27352 entries) | **bytes: no; tree: yes** |
| Manifest (block 0) | `tooling/native/make-root-manifest.py`, inside `make-root-image.sh` | `verify-root.py` recomputes it from the two windows | yes, given the windows |
| `root.img`, `root.json` | `tooling/k273/make-root-image.sh out/inputs/k273/root` | `python3 -I tooling/repro/verify-root.py out/inputs/k273/root --manifest-json out/inputs/k273/root/systemos.manifest.json` | follows the SystemOS window |

Why the SystemOS bytes differ: `newfs_apfs` gives every new container a random volume UUID, and APFS records creation times and
allocation order. Two runs never give the same image sha256. On 2026-11-25 the reference window (`a5346509...`, UUID 07AD53B3...)
and a fresh rebuild from the same pinned members (`30818f46...`, another UUID) gave the same file manifest `070934ff...`. That
manifest records path, type, mode, owner, flags (without UF_COMPRESSED), size, content sha256 and link target, and leaves out
times and the root's `.fseventsd`. Your SystemOS UUID is your own. `make-root-image.sh` writes it into `root.json`, and
`build-k273-image.sh` passes it to the ADT (`PIANO_SYSTEMOS_UUID`), so it never has to equal the reference. Your `root.img`
sha256 is your own too. `verify-root.py` checks the parts that can be checked: the pinned OS window inside the image, your
SystemOS window inside the image, the recomputed manifest, the layout, the UUIDs. Write down your `root.json` `image_sha256`: the
install steps take it as `--root-sha256`.

`make-root-image.sh` also makes the trust bundle (`make-trust.py`: the base bundle's own modules (G2) + the IPSW's three trust
caches, pinned) and copies the Data seed (`K273_SEED`, G3). The entry script refuses to start this step without both. Note:
`root.json`'s `write` block names the reference unit's `linux_data` LBAs (common.py), one more reason for G6.

### 3.2 Kernel, firmware, kernel collection and RAM images

```sh
bash tooling/k273/prepare-k27-tree.sh                     # needs PIANO_K27_TREE (G1); prints the relinked kernel's path
bash tooling/firmware/build-fd.sh                          # the UEFI FD from the unixnu.lock checkouts; stamps ram-payload-first
K273_ROOT=k273 K273_ROOT_PART=linux_data PIANO_GPU=2 PIANO_DATA_PERSIST=0 PIANO_RESET_AFTER=900 \
  bash tooling/k273/build-k273-image.sh                    # KC (only this repository's kexts), ADT, out/ram and out/ram-dump
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

Pass lines: the KC log's `native kext rule: ... all this repository's`; `verify-ram-images.py` exits 0; the image is at most
250000000 bytes (ABL's 256 MiB kernel region). `PIANO_DATA_PERSIST=0` makes Data volatile, so a test image never opens a Data
store. `PIANO_RESET_AFTER=900` returns the tablet to ABL fastboot after 900 s. `K273_ROOT_PART=linux_data` is the first root
place on a second unit (Ubuntu's root on `linux_rootfs` stays).

## 4. RAM-boot validation first (`fastboot boot` only; nothing is written)

A RAM boot loads the image into memory and writes no partition. Run the RAM steps below, in order, before any write.

1. ABL, read only: `python3 tooling/persist/maint-session.py abl-check`. Pass: product `piano`, unlocked `yes`, current-slot `a`,
   slot A bootable, battery OK.
2. Start the console capture: `python3 tooling/native/usb-console.py --capture 960`. Only one capture may own the port.
3. `fastboot boot out/ram/boot-piano-uefi-ram-A-bundled.img`, then wait. With no root on the disk yet, the boot must reach:
   - UEFI: `UNIXNU: config: volume VenHw(A9C6827C-...)`: the image's own payload, not an ESP;
   - DarwinBoot -> XNU: the `NativeRamBlock: RAM window ...` line;
   - `NativeUfs: start ... root-part=linux_data` and the partition scan, then the wait for the root (no SHNGROOT on
     `linux_data` yet: expected);
   - the USB console answering (`usb-console.py` shows the kernel log);
   - back in ABL fastboot by `piano-reset-after` (or send `reset fastboot` on the console).
   A panic, a hang without the reset, or `Load Error` is a FAIL. Read `docs/bringup-debugging.md` §1 for each symptom, and
   `tooling/native/native-loop.sh`'s dump image for the mailbox log.
4. The maintenance image, read only: `maint-session.py --dry-run maint-boot map maint-exit` (read every line), then without
   `--dry-run`. On a second unit `map` stops at the GPT and boot-partition checks (G6). That stop is the expected result; it
   proves the guard works. Record its output.

**Before any write, all of these must pass:** sections 1-3 (`check-host.sh` 0 FAIL, `fetch-ipsw.py --check` PASS,
`verify-root.py` PASS with the manifest, `verify-ram-images.py` 0), the RAM boot above, the unit's own full backup (Ubuntu INSTALL
§2) present and its hashes checked, G6 resolved, and the owner present.

The full-desktop qualification needs the root on the disk. It follows the first write (5.2): the same image boots the desktop
from `linux_data` with Data volatile. Check the login window, touch, keyboard cover, USB network/ssh, Wi-Fi scan and battery
readout before writing anything else.

## 5. Install (only through `tooling/persist/maint-session.py`, only after G6)

### 5.1 Rules: never broken, no exceptions

- **Never write** `xbl`, `abl`, `tz`, `hyp`, `vbmeta`, `dtbo`, `persist`, any firmware/modem/efs/calibration partition, or **any
  `_b` partition**. Never write `userdata`, `super`, `metadata`, `vendor_boot_a`, `init_boot_a`. They are only read and backed up.
- **Never** `fastboot flash`, `fastboot erase`, `fastboot --set-active` / `set_active`, any `fastboot oem`, `flash_all_lock`.
  Writes are `dd` from the maintenance RAM image, by `maint-session.py`, and nothing else.
- **Backup first:** before each write, the step `backup` exports every region it writes, over one connection, into
  `PIANO_BACKUP_DIR` (set in `~/.config/piano/env`). Three sha256 checks: the tablet's own hash of the region, the received
  bytes, and the stored copy read back without the page cache. A missing or unequal backup stops the step.
- **Read back after each write:** the region is read from the medium with the cache dropped, and its sha256 must be the image's.
  Any mismatch stops the session.
- `--dry-run` first, every time. Read every command it prints. One tablet session at a time. Never interrupt a running write.
- Only the regions `docs/persistent-boot.md` §1 and §13.8 list: the LUN 0 GPT (one new partition, 37 `esp`, cut from the end of
  `linux_rootfs`), the ESP, `boot_a`, the root image, the Data store region. On a second unit that also means the Ubuntu-era
  `linux_rootfs`/`linux_data` contents; back up anything you want to keep from them yourself first (`linux-data-root` keeps no
  copy of `linux_data`'s old contents).

### 5.2 The sessions (the reference unit's order, `docs/persistent-boot.md` §5, §12, §13.8)

`P=tooling/persist/maint-session.py`. Add `--dry-run` first each time:

0. Build the ESP first, offline: `tooling/k273/build-k273-esp.py --root-part linux_data ...` (`docs/persistent-boot.md` §13.8;
   its record names the root partition, the Data store's partition and LBA, and the seed). Every session below passes it as
   `--esp`, because `map` checks the disk against that record. `--dry-run` lists anything else a step needs.
1. Root image onto `linux_data` (Ubuntu's root stays): `python3 $P maint-boot map linux-data-root check maint-exit --esp <esp>.img
   --root-part linux_data --root-image out/inputs/k273/root/root.img --root-sha256 <root.json image_sha256>`. The step checks the
   image's hash on the host, writes it, and reads `linux_data` back over the image's length. Then run the full-desktop RAM
   qualification (section 4, last paragraph).
2. ESP and GPT: `python3 $P maint-boot map backup gpt esp check maint-exit --esp <esp>-diag.img`, then the RAM tests of the flashed
   path with nothing written: `ram-esp-test`, `ram-v4-signed`, `ram-precedence` (§5). A test-phase ESP's default entry resets into
   ABL fastboot by itself (`-diag`); the daily one comes with `esp-final`.
3. `boot_a`: sign the boot image with your own AVB key (G7; `tooling/persist/build-boot.py`, `sign.sh`), then `python3 $P
   maint-boot map backup boot-a esp-final check maint-exit --esp <esp>-diag.img --esp-final <esp>.img`, then `abl-final reboot`. Slot A then boots macOS. Last test: power
   off, unplug, power on.
4. If `abl-check` shows `slot-successful:a: no`, run `slot-success-crc`. It writes only bit 54 of `boot_a`'s LUN 4 entry and the
   two CRCs, after backups of both copies. Never `set_active`.

The ESP and the root image must agree: the ESP's arguments name the root partition (`native-ufs-root-part`) and the Data store's
partition and LBA, and `map`/`esp-final` refuse an ESP whose store is not the one on the disk (`docs/data-store.md`).

### 5.3 Rollback, least invasive first (`docs/persistent-boot.md` §7, `docs/install-esp-v3.md` §5 for the pattern)

1. **A hang or panic:** the boot guard (`piano-boot-guard=180`, `docs/boot-guard.md`) lands in ABL fastboot. Otherwise hold power
   + volume-down 15-20 s. From ABL every RAM image boots as before. A RAM image's own payload wins over the ESP (`ram-precedence`).
2. **Undo a step:** `python3 $P maint-boot`, then `python3 $P rollback STEP`, then `maint-exit`. Each rollback streams that step's
   verified backup back and reads it back. Full undo order: `boot-a`, `esp`, `store`, `gpt`.
3. **Android (the charging fallback):** from ABL, `piano-switch.py` of the Ubuntu release with the unit's own `android-set` writes
   slot A's Android set back. `userdata`, `super` and `metadata` were never touched. Back to macOS the same way with the macOS set,
   then `slot-success-crc` (`docs/persistent-boot.md` §7, the Android row).
4. **Factory:** the unit's own full backup from Ubuntu INSTALL §2, through the maintenance image. Never `flash_all_lock`.

## 6. Post-install checks

On the first boots from the disk (not a RAM image), record each with its console or shell line:

- `piano-boot-guard: healthy after 180 s`, restart reason normal (`docs/boot-guard.md` §5);
- the Data store's state line (`NativeRamBlock: Data store state 3` for a fresh store, later `state 2`). **Stop and roll back**
  on `state 7`, `kept` or `started over` (`docs/data-store.md` §3);
- the root from the expected partition (`NativeUfs ... root-part=...`, the windows published, the root mounted);
- over the USB shell or ssh: `sw_vers` gives 27.3 26C5035r, `sysctl hw.model` gives Mac17,11;
- the login window, then the desktop: touch, keyboard cover, Wi-Fi join, sound, brightness;
- battery percentage, and charging on a USB-PD charger (Android stays the fallback until this is seen);
- `fastboot getvar battery-voltage` from ABL before and after;
- a cold power cycle (power off, unplug, power on) boots macOS again; Android still switches back and forth (5.3 step 3).

## Known gaps (local-only inputs) and the options for the owner

Nothing below may be published. Each item is a decision for the owner of the original setup. The skill never works around one.

| # | Gap | Why it is local | Options |
|---|---|---|---|
| G1 | **The k27 tree** (`PIANO_K27_TREE`, `unixnu.lock` pin `k27` 11ac316b): xnu-12377.121.6 + the baseline series 0001-0052, the DroidVM bootstrap (fakeroot headers, firehose, ArmDTPlatform/ArmCpu objects), the KC builder `k27/kc-build273-piano.py`, the 27.0.1 KC base, `ld-classic` (ld64-957 of an older Command Line Tools) and its libraries | DroidVM content is not public (project rule), and parts come from Apple's KDK and toolchain downloads | (a) a script that rebuilds the tree from public sources: Apple's xnu-12377.121.6 tag + the series as patches + the user's own KDK and Command Line Tools downloads from Apple's developer site, verified by file hashes; needs the owner's decision on publishing 0001-0052 and the bootstrap pieces; (b) a hash manifest of the tree so a holder can verify a copy; (c) the owner gives the tree privately to named builders |
| G1b | The SystemOS boot shims (`overlay/systemos-shims/sbin/fsck`, `mount`: two shell scripts of 17 and 358 bytes) | part of the k27 tree | own rewritten shims in this repository with a `SYSTEMOS_SHIMS` switch in `make-systemos.sh`; that changes the pinned manifest's `sbin` group, so it needs a new pin and a tablet check (the installed line keeps its window) |
| G2 | The base trust bundle `out/inputs/kernel.macho.trustcache` (`2b87b224...`), whose own modules `make-trust.py` keeps | made by the k27 tree's `scripts/make-trust-bundle.py` from staged code | regenerate it from the k27 tree (with G1); or let `make-trust.py` start from no own modules and let the staging tools add theirs |
| G3 | The Data seed (`out/inputs/scratch-data.img` `cb50ceac...`; the daily seeds) | contains the owner's account and Data | a fresh seed from the IPSW's Data template (the k27 tree's `make-macos-data-seed.sh`, UniXNU's `seed-data-skip-setup.py`) with the user's own account made at first boot. Needs building and a RAM test |
| G4 | The Turnip Mesa tree (`TURNIP_TREE`, UniXNU-turnip) and its staged Metal plugin (`out/turnip-a830`) | not public | publish the delta as patches on a pinned upstream Mesa commit (MIT), owner decision. Without it there is no A830 desktop |
| G5 | Per-device data (2.4) and the MiPPS key file `private/charging/mipps-keys.bin` | per unit, or third-party protocol material | per-unit: extracted on each unit with the Ubuntu tools and ours (2.4). MiPPS: never shipped; `PIANO_POWER_MIPPS=0` builds without it (the charge pump is off by default anyway) |
| G6 | **The write tooling is pinned to the reference unit**: `tooling/persist/common.py` (GPT sha256s, LBAs, boot-partition hashes, the `CRITICAL-BACKUP` paths), `build-gpt.py` (from that unit's tables), `make-root-image.sh`'s write plan | it was written for one tablet | a device profile JSON made from the unit's own backups (`tooling/repro/unit-profile.py` is the read-only start), read by `common.py` instead of its constants, with `test-persist.py`'s fake-tablet session run per profile. Until then, no write on a second unit |
| G7 | `boot_a` signing: `tooling/bootimg/sign-on-remote.sh` signs on a remote host with the reference unit's key | the private key never leaves that host | a local `avbtool` path with the unit's own `boot-signing.pem` (Ubuntu INSTALL §3), the same footer parameters (`boot-profile.json`) |
| G8 | The daily recipe: the exact-args profile, the layer trust caches, the userland layer and the firmware addons the owner's daily images take | recorded in private notes | a public daily profile in `configs/` (arguments without private values) and the layer build from `tooling/userland/` |
| G9 | `out/fd` from `build-fd.sh` is not bit-reproducible (two clean builds differ) | EDK II build timestamps | identify it by its hash only (as now); optional: pin the EDK II reproducible-build flags |
| G10 | 256 GB units: `linux_data` about 61 GiB, other LBAs | the tooling knows the 512 GB layout | covered by G6's device profile, plus a smaller Data store |
| G11 | Other stock builds and regions (Wi-Fi `board.bin` differs per region) | locks pin OS3.0.303.0.WPYCNXM, China | new locks per build/region, as the Ubuntu project does |

## Files of this kit

- `tooling/repro/pins.json`: every pin (IPSW, members, plain images, KC, root layout, SystemOS manifest, maintenance image, DTB).
- `tooling/repro/check-host.sh`: the requirements (1).
- `tooling/repro/fetch-ipsw.py`: the Apple inputs (2.2).
- `tooling/repro/boot-dtb.py`: the boot DTB (2.3).
- `tooling/repro/unit-profile.py`: the unit's layout against the tooling's (2.5).
- `tooling/repro/volume-manifest.py`, `verify-root.py`: the root checks (3.1).
- `tooling/repro/fetch-and-build.sh`: the one entry (3).
- `tooling/repro/test-repro.py`: host tests, no network, no tablet.
