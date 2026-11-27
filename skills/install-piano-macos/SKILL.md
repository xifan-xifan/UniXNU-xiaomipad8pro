---
name: install-piano-macos
description: Guide a coding agent end to end through putting native macOS 27.3 (UniXNU-piano) on ANOTHER Xiaomi Pad 8 Pro (piano, SM8750) — host requirements, fetching the pinned IPSW from Apple and rebuilding every Apple-derived input locally (root image included) with sha256 checks, building the images from this repository, RAM-boot validation with nothing written, the guarded install, rollback, and post-install checks. Use when someone asks to build these images outside the original setup or to install them on a second tablet. Read "Status" and "Known gaps" first: several inputs are still local to the original setup.
---

English | [简体中文](SKILL.zh-CN.md)

# Install native macOS 27.3 on a Xiaomi Pad 8 Pro (piano)

You are the operator. The tablet owner sits next to the tablet and presses its buttons. You run commands on a Mac. Follow the
sections in order and record the result of each check. **Stop at the first
failure and report it to the owner.** Do not retry a write. Do not work around a refusal.

All paths are relative to this repository's checkout. `tooling/repro/` is the kit this skill calls. Nothing from Apple's IPSW is
ever committed or shipped: every user fetches it from Apple and rebuilds the derived files on their own Mac.

## 0. Status: what a second unit can do today (2026-11-26)

The reference tablet runs **v13.1y12q** with the k27 native patch series **0053–0076**, a **13 GiB** XNU window (13310 MiB),
Data concurrent mode after mount, and USB debug network TX flow control. The installed display runs at 60 Hz with Night Shift
routing and native panel sleep off. [Installed results](../../docs/results-2026-11-26.md#evening)

The reference daily layout puts the root image on `linux_rootfs` and the direct Data store on `linux_data`. The second-unit
qualification in sections 3–4 targets a root image on `linux_data` to preserve Ubuntu's root and uses volatile Data. A complete
daily ESP build and disk install on a second unit still depend on G6–G8. [Reference disk layout (§13.8)](../../docs/persistent-boot.md)

| Section | State for a second unit |
|---|---|
| 1 Requirements | ready: `tooling/repro/check-host.sh` |
| 2 Public build inputs | ready: `scripts/fetch-pins.py` for source, `fetch-ipsw.py` for pinned Apple inputs, and `boot-dtb.py` for the Ubuntu boot DTB |
| 2 Per-device data | partly ready: extracted from the unit through the Ubuntu install's tools (G5) |
| 3 Root image rebuild | needs the two SystemOS boot shims (G1b), the base trust bundle (G2) and a Data seed (G3) |
| 3 Kernel, KC, RAM images | k27 source fetch is available. Needs a built k27 tree (G1), the root inputs above, and Turnip for the A830 build (G4) |
| 4 RAM boot | ready once 3 builds (the `fastboot boot` path takes nothing per unit) |
| 5 Install | **blocked: the write tooling is pinned to the reference unit (G6)**. `maint-session.py` refuses another unit at `map`. Never force it. |
| 6 Post-install checks | ready (the same checks as the reference unit) |

If a row says blocked, finish the sections before it, then report the gap to the owner with its options from "Known gaps".

## 1. Requirements

### 1.1 Host: a Mac with Apple silicon

Run `bash tooling/repro/check-host.sh`. It reads only. It prints one line per requirement: `ok`, `WARN` (untested), `FAIL` (cannot
build here) or `GAP` (an input to supply, see Known gaps). Continue only with `summary: 0 FAIL`.

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
- **Homebrew** at `/opt/homebrew`. Install `python@3.15 llvm gnu-sed bison mtools dtc zstd`, `blacktop/tap/ipsw` (the AEA key
  fetch), and `nasm mtoc acpica` for the FD. The tablet steps also use the cask `android-platform-tools` (`fastboot`).
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
  - creates `linux_rootfs` (60 GiB) and `linux_data` (about 201 GiB), the space used for the macOS root and Data;
  - collects the unit's own files: `boot-profile.json`, your own AVB key `boot-signing.pem`, the touch calibration and the sensor
    registry;
  - leaves Android bootable (`userdata` 200 GiB), with `tools/piano-switch.py` and the slot-A Android set to switch back.
  The macOS install can carve an ESP from `linux_rootfs` and replace `boot_a` after the checks in section 5. Android's data
  and firmware partitions stay intact.
- **Cables:** a USB-C data cable from the tablet to the Mac, directly, not through a hub. The same cable carries fastboot, the
  maintenance image's network and the macOS USB console (`tooling/native/usb-console.py --list`).
- Battery above 50 % (`fastboot getvar battery-voltage`). Android remains the charging fallback until macOS charging is checked
  on the unit.
- **The owner's escape:** hold power + volume-down for 15-20 s. This always reaches ABL fastboot, because ABL is never written.

## 2. Inputs

### 2.1 This repository and its pinned sources

```sh
git clone https://github.com/lenzhang/UniXNU-xiaomipad8pro.git UniXNU-piano && cd UniXNU-piano
export UNIXNU_RESEARCH="$PWD/out/unixnu-research"
python3 scripts/fetch-pins.py --dry-run
python3 scripts/fetch-pins.py
export PIANO_K27_TREE="$UNIXNU_RESEARCH/k27-tree"
```

Run the fetch into a fresh directory. It checks out the commits and submodule pins in `unixnu.lock` under the names the build
expects, including `Mu-Silicium-piano` and `piano-uefi-UniXNU-piano`. For `k27` it calls `fetch-k27-tree.sh`, which applies
`patches/k27-tree` to the pinned macOS-DroidVM source and verifies the resulting Git tree ID. A successful fetch gives you the
source. Build the local k27 inputs in section 3.2. [Fetch script](../../scripts/fetch-pins.py) · [Source lock](../../unixnu.lock) · [k27 provenance](../../patches/k27-tree/README.md)

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

Keep the pinned maintenance image for the RAM verifier in 3.2. The RAM build also needs the patched `mkbootimg.py` from the
Ubuntu host tools. Set `PIANO_MKBOOTIMG` if it is outside the release's `host-tools` directory.

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

Set `LUN0_GPT` and `LUN4_GPT` to the MBR plus primary GPT files from those LUNs in the unit's Ubuntu backup.

```sh
python3 -I tooling/repro/unit-profile.py --lun0 "$LUN0_GPT" --lun4 "$LUN4_GPT"
```

It reads two files and compares `linux_rootfs`, `linux_data`, LUN 0 size and `boot_a` with `tooling/persist/common.py`.
`SAME-LAYOUT` means the partitions sit where the tooling expects. The GPT hash still differs (the GUIDs are per unit), so the
install remains blocked by G6. `DIFFERENT` (exit 2) means the unit cannot use this tooling's layout at all. Report either result.

## 3. Build the images (Mac only; nothing touches the tablet)

Use the entry script to review the full plan and build the root inputs. Prepare the kernel explicitly in section 3.2 so the
relink uses the same baseline tag as the bootstrap.

```sh
bash tooling/repro/fetch-and-build.sh --dry-run
bash tooling/repro/fetch-and-build.sh host inputs systemos root
bash tooling/repro/fetch-and-build.sh --check inputs systemos root
```

### 3.1 The root image, rebuilt locally (never shipped)

The 20.9 GB root image is the SHNGROOT manifest plus two windows (`docs/k273-line.md` §3):

| Piece | Made by | Check | Reproducible? |
|---|---|---|---|
| OS window (14493417472 B) | the IPSW's `141-22233-021.dmg`, decrypted, unchanged | sha256 `40d8282e...` (pinned) | yes, byte for byte |
| SystemOS window (6442450944 B) | `tooling/k273/make-systemos.sh` (invoked by the `systemos` step): the SystemOS and App cryptexes, the Data template's read-only parts, the boot shims (G1b) | The `systemos` step writes `out/inputs/k273/root/systemos.manifest.json` and checks it against `pins.json` with `volume-manifest.py`. The file-level digest must be `070934ff...` (27352 entries) | **bytes: no; tree: yes** |
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

For a freshly fetched tree, export `KDK_DMG` with the path to your Apple KDK download and provide `ld-classic` (G1). The bootstrap
builds the native 0066 baseline and its local build inputs. The k27 source pin already includes 0053–0054. The bootstrap adds
0055–0066, then the 27.3 relink applies `patches/k273` and the remaining checked-in k27 patches through 0076 to its own clone.
The checked-in series skips 0072. [Kernel bootstrap](../../docs/ci.md#buildyml) · [k27 series](../../patches/k27/README.md) · [27.3 series](../../patches/k273/README.md)

```sh
K27_DIR="$PIANO_K27_TREE" K27_TREE_TOP=0066 bash tooling/ci/k27-kernel.sh xnu ld kdk fakeroot platform kernel
K27_TREE_TOP=0066 bash tooling/k273/prepare-k27-tree.sh 0066
K273_KERNEL=out/k273/k27-tree/out/k27/kernel-k273-0076-s0001-native.macho \
  K273_ROOT_PART=linux_data PIANO_GPU=2 PIANO_RESET_AFTER=900 bash tooling/repro/fetch-and-build.sh fd image
python3 tooling/k27/check-piano-bootargs-kc.py --kc out/k27/kc-k27-native.bin
python3 tooling/adt/check-ram-window.py --adt out/adt/piano-k27.adt --kc out/k27/kc-k27-native.bin \
  --fd out/fd/piano-uefi-A-bundled.fd
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json \
  --reference "$PIANO_V110/release/boot/boot-ram-maintenance.img"
```

Use the relink's printed kernel path if `K273_TREE` points elsewhere. The FD builder stamps both `ram-payload-first` and
`ram-window`. The 27.3 ADT asks for `0x33fe00000` bytes (13 GiB less 2 MiB) and needs a DarwinBoot FD with the 13 GiB bound.
Require all three checks above to exit 0, the RAM window check to report 13310 MiB, and the image to fit the 250000000-byte
limit. The entry script sets `PIANO_DATA_PERSIST=0` so the RAM image uses volatile Data, and `PIANO_RESET_AFTER=900` returns
the tablet to ABL fastboot after 900 s. [FD stamps](../../tooling/firmware/build-fd.sh) · [RAM window (§12)](../../docs/persistent-boot.md)

## 4. RAM-boot validation first (`fastboot boot` only; nothing is written)

A RAM boot loads the image into memory and writes no partition. Run the RAM steps below, in order, before any write.

1. ABL, read only: `python3 tooling/persist/maint-session.py abl-check`. Pass: product `piano`, unlocked `yes`, current-slot `a`,
   slot A bootable, battery OK.
2. Start the console capture: `python3 tooling/native/usb-console.py --capture 960`. Only one capture may own the port.
3. `fastboot boot out/ram/boot-piano-uefi-ram-A-bundled.img`, then wait. With no root on the disk yet, the boot must reach:
   - UEFI: `UNIXNU: config: volume VenHw(A9C6827C-...)`: the image's own payload, not an ESP;
   - DarwinBoot -> XNU: `NativeRamBlock: RAM window ... (13310 MiB)`;
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
`verify-root.py` PASS with the manifest, and all three image checks in 3.2), the RAM boot above, the unit's own full backup
(Ubuntu INSTALL §2) present and its hashes checked, the applicable gaps G6–G8 resolved, and the owner present.

The full-desktop qualification needs the root on the disk. It follows the first write (5.2): the same image boots the desktop
from `linux_data` with Data volatile. Check the login window, touch, keyboard cover, USB network/ssh, Wi-Fi scan and battery
readout before writing anything else.

## 5. Install (through `tooling/persist/maint-session.py` after G6–G8)

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

### 5.2 Reference workflow and second-unit prerequisites

These are the stages a second-unit workflow must cover once G6–G8 are resolved. Use `--dry-run` before each session.

1. Build an ESP for the unit's layout, with matching KC, ADT, trust cache and seed. `build-k273-esp.py` updates a verified base ESP
   and its JSON record. `map` checks the root partition and the Data store's partition, LBA and seed against that record (G8).
2. Back up the region and write the root image using `linux-data-root` with the unit's `--root-part` and the local
   `--root-sha256` from `root.json`. For the Ubuntu-preserving `linux_data` qualification, run the full-desktop RAM checks
   from section 4 after the read-back passes.
3. Back up and verify the GPT, diagnostic ESP and Data store region as applicable. The reference workflow uses `gpt` and `esp`
   for provisioning, then checks RAM-payload precedence and tests the final boot image from RAM. For the 13 GiB ESP use a
   v5-capable FD (`build-boot.py --series 5`), a unit-signed `--boot-image` (G7), and the `ram-boot-a` test path.
4. After those gates, the guarded workflow writes `boot_a` and `esp-final` with backups and read-back checks, then verifies a
   cold boot. If `abl-check` shows `slot-successful:a: no`, the reference `slot-success-crc` step updates bit 54 of the `boot_a`
   entry and the CRCs in both LUN 4 GPT copies after backing them up.

Sections 12 and 13.8 document the reference v5 boot path, ESP and Data migration, including the required order for changing
the boot image and ESP together. [Persistent boot](../../docs/persistent-boot.md) · [Data store](../../docs/data-store.md)

### 5.3 Rollback, least invasive first (`docs/persistent-boot.md` §7, `docs/install-esp-v3.md` §5 for the pattern)

1. **A hang or panic:** the boot guard (`piano-boot-guard=180`, `docs/boot-guard.md`) lands in ABL fastboot. Otherwise hold power
   + volume-down 15-20 s. From ABL every RAM image boots as before. A RAM image's own payload wins over the ESP (`ram-precedence`).
2. **Undo a step:** use `maint-session.py` to enter maintenance, map the disk, roll back the step from its verified backup,
   then map, check and exit. To return to an 8 GiB FD from a 13 GiB pair, roll back `esp-final` before `boot-a` (§12.7).
3. **Android (the charging fallback):** from ABL, `piano-switch.py` of the Ubuntu release with the unit's own `android-set` writes
   slot A's Android set back. `userdata`, `super` and `metadata` were never touched. Back to macOS the same way with the macOS set,
   then `slot-success-crc` (`docs/persistent-boot.md` §7, the Android row).
4. **Factory:** the unit's own full backup from Ubuntu INSTALL §2, through the maintenance image. Never `flash_all_lock`.

## 6. Post-install checks

On the first boots from the disk (not a RAM image), record each with its console or shell line:

- `piano-boot-guard: healthy after 180 s`, restart reason normal (`docs/boot-guard.md` §5);
- `NativeRamBlock: RAM window ... (13310 MiB)` and `max_mem 13310 MiB` with the 13 GiB FD/ADT pair;
- the Data store's state line (`NativeRamBlock: Data store state 3` for a deliberately fresh store, `state 2` for a resumed
  store). **Stop and roll back** on an unexpected `state 3`, `state 7`, `kept` or `started over`;
- for the y12q Data profile on a resumed direct store (`data-persist=2`), look for `data-store-concurrent=2` in the boot arguments.
  Data mounts in single-gate mode. With `PIANO_DATA_CONCURRENT_AT_BOOT=1` in its staged plist, the `piano-data-concurrent`
  daemon switches only after mount and live `store_state=2`. Confirm `sysctl -n machdep.piano_data_concurrent` returns `1`
  and `sysctl -n machdep.piano_data_io` reports `mode=concurrent`. Write-back and queue modes stay off. [Data concurrent mode](../../docs/data-store.md#concurrent-mode-at-boot-piano-data-concurrent-2026-11-26)
- the root from the expected partition (`NativeUfs ... root-part=...`, the windows published, the root mounted);
- over the USB shell or ssh: `sw_vers` gives 27.3 26C5035r, `sysctl hw.model` gives Mac17,11;
- with 0076, `sysctl -n kern.piano_bootargs` shows the full merged boot line. Check the expected profile arguments, including
  any past the 1024-byte limit of legacy `kern.bootargs`. If the profile enables the USB debug shell, verify that it starts.
- the y12q USB debug network profile sets `piano-usb-net=1` and `piano-usb-net-txflow=1`. Confirm
  `PianoDwc3: net: piano-usb-net-txflow=1` in the console and check exact data and zero TX drops in the transfer matrix.
  The reference test passed 66 of 66 transfers. The driver defaults to `0` for TX flow control when the profile omits it.
  [USB transfer matrix](../../tooling/native/usb-net-transfer-test.md)
- the login window, then the desktop: touch, keyboard cover, Wi-Fi join, sound, brightness;
- battery percentage, and charging on a USB-PD charger (Android stays the fallback until this is seen);
- `fastboot getvar battery-voltage` from ABL before and after;
- a cold power cycle (power off, unplug, power on) boots macOS again; Android still switches back and forth (5.3 step 3).

## Known gaps and options for the owner

Keep unit-specific inputs, keys and seeds private. Resolve each applicable gap before continuing.

| # | Gap | Why it matters | Options |
|---|---|---|---|
| G1 | **Built k27 inputs** in `PIANO_K27_TREE` | The public source fetch leaves the native kernel, generated headers, fakeroot and platform objects to build locally. The bootstrap needs Apple KDK headers and `ld-classic` (the reference uses ld64-957). | Fetch the pinned source in 2.1 and use the bootstrap in 3.2 with your own Apple KDK and `ld-classic` via `LD_CLASSIC` if needed. See the open items in the [CI notes](../../docs/ci.md#open-items). |
| G1b | The SystemOS boot shims (`overlay/systemos-shims/sbin/fsck` and `mount` under `PIANO_K27_TREE`) | `make-systemos.sh` requires these files and the resulting tree must match the existing manifest pin. | Check that both are present before running `systemos`. If either shim is missing or the manifest differs, stop and resolve the input with the owner. |
| G2 | The base trust bundle `out/inputs/kernel.macho.trustcache` (`2b87b224...`), whose own modules `make-trust.py` keeps | made by the k27 tree's `scripts/make-trust-bundle.py` from staged code | regenerate it from the k27 tree (with G1); or let `make-trust.py` start from no own modules and let the staging tools add theirs |
| G3 | A clean Data seed (`K273_SEED`) | The reference seeds contain account data. | Build a fresh seed from the IPSW's Data template (the k27 tree's `make-macos-data-seed.sh`, UniXNU's `seed-data-skip-setup.py`) with the user's own account made at first boot. The fresh recipe still needs a build and a RAM test. |
| G4 | The Turnip Mesa tree (`TURNIP_TREE`, UniXNU-turnip) and its staged Metal plugin (`out/turnip-a830`) | These inputs are required for A830 acceleration. The Mesa delta and metal2vulkan bridge lack public pins. | Publish the Mesa delta on a pinned upstream commit and pin the bridge, subject to the owner's decision. |
| G5 | Per-device data (2.4) and MiPPS keys | per unit, or third-party protocol material | Extract each unit's data with the tools in 2.4. `PIANO_POWER_MIPPS=0` builds without MiPPS keys. |
| G6 | **The write tooling is pinned to the reference unit**: `tooling/persist/common.py` (GPT sha256s, LBAs, boot-partition hashes, the `CRITICAL-BACKUP` paths), `build-gpt.py` (from that unit's tables), `make-root-image.sh`'s write plan | it was written for one tablet | a device profile JSON made from the unit's own backups (`tooling/repro/unit-profile.py` is the read-only start), read by `common.py` instead of its constants, with `test-persist.py`'s fake-tablet session run per profile. Until then, no write on a second unit |
| G7 | `boot_a` signing and verification | `tooling/persist/sign.sh` delegates to the reference signing host and `build-boot.py --verify` expects the reference key. | Use a local `avbtool` workflow with the unit's own `boot-signing.pem` and `boot-profile.json` (Ubuntu INSTALL §3), with verification against that unit's public key. |
| G8 | Complete y12q ESP recipe | The public settings, userland staging and Data daemon cover parts of y12q. The updater needs a verified base ESP and its JSON record, plus the matching seed, layer trust caches and firmware inputs. | Assemble a second-unit base ESP and a daily profile with values for that unit. Stage the userland layer from [the public manifest](../../configs/userland.toml) using [the staging guide](../../docs/userland.md). |
| G9 | `out/fd` from `build-fd.sh` is not bit-reproducible (two clean builds differ) | EDK II build timestamps | identify it by its hash only (as now); optional: pin the EDK II reproducible-build flags |
| G10 | 256 GB units: `linux_data` about 61 GiB, other LBAs | the tooling knows the 512 GB layout | covered by G6's device profile, plus a smaller Data store |
| G11 | Other stock builds and regions (Wi-Fi `board.bin` differs per region) | locks pin OS3.0.303.0.WPYCNXM, China | new locks per build/region, as the Ubuntu project does |

## Files of this kit

- `tooling/repro/pins.json`: every pin (IPSW, members, plain images, KC, root layout, SystemOS manifest, maintenance image, DTB).
- `scripts/fetch-pins.py`: the pinned source checkouts and k27 source tree (2.1).
- `tooling/repro/check-host.sh`: the requirements (1).
- `tooling/repro/fetch-ipsw.py`: the Apple inputs (2.2).
- `tooling/repro/boot-dtb.py`: the boot DTB (2.3).
- `tooling/repro/unit-profile.py`: the unit's layout against the tooling's (2.5).
- `tooling/repro/volume-manifest.py`, `verify-root.py`: the root checks (3.1).
- `tooling/repro/fetch-and-build.sh`: the one entry (3).
- `tooling/repro/test-repro.py`: host tests, no network, no tablet.
