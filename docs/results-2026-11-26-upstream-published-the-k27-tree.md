# Upstream moved the missing k27 tree into the repository; the local probe line is retired (2026-11-26)

Fetch: `480c661a` was the adopted upstream tip; `upstream/main` is now `b117da9e`, 79 commits ahead
(`git rev-list --count 480c661a..upstream/main`). None of them touch our tablet, which stayed in HyperOS
`OS3.0.309.0.WPYCNXM` throughout this round.

## 1. The k27 tree gap is closed, and reproducible here

`d83e0912` publishes the k27 delta as `patches/k27-tree` (two patches) on `Droid-VM/macOS-DroidVM`
`abcbc18f4791b9cd4a3b2a52c92ef8382435376d`; `unixnu.lock` `[pin."k27"]` adds `commit`, `patches` and
`tree = 544f9468b660aabbc73ee24638adabd14c274671`, and keeps `local = 11ac316b789d81fc9f734e15645456f536c05141`.
That resolves the search from `docs/results-2026-11-19-...` and later: `11ac316` is the maintainer's local commit
(`el1/native-variant`) meaning `abcbc18` + these two patches, not a commit that exists on GitHub.

Reproduced read-only on this host, WSL `UbuntuPiano`: `git archive upstream/main` into `/tmp/upstream-main`,
then `tooling/k27/fetch-k27-tree.sh /tmp/k27tree` printed

    k27 tree: https://github.com/Droid-VM/macOS-DroidVM.git abcbc18f4791 + 2 patches = tree 544f9468b660

so the pinned tree (build scripts, KC builder, `patches/xnu-12377` 0001-0054) is now buildable by us.

## 2. The interrupt conclusion was about a probe branch, not the mainline kernel

`docs/power-irq.md` 2.1 states what the native kernel offers: UniXNU's `ArmCpu` (a `unixnu.lock` pin) is
force-loaded into the k27 kernel and drives the GICv3 (dist `0x16000000`), exporting `armcpu_spi_attach`
and `armcpu_spi_count`; no SPI-level `IOInterruptController` exists, so native kexts take SPIs through
`ArmCpu`, not through `registerInterrupt`. Grepping the binaries we hold (counts are occurrences of the name):

| file | size | `armcpu_spi_attach` | `armcpu_spi_unhandled` | `piano_gicv3_init` |
| --- | --- | --- | --- | --- |
| `UniXNU/out/piano/macos-27.3-26C5035r/piano_macos/kernel/kernel.macho` in WSL (built here 2026-11-16) | 142,393,344 | 1 | 1 | 0 |
| `out/k27-smprobe/piano-ufs266.kc` | 147,095,552 | 4 | 2 | 1 |
| `out/upstream-pub-20261125/piano-ufs269.kc` | 135,806,976 | 3 | 1 | 0 |

`piano_gicv3_init`, `piano_gic_irq_handler`, `piano_delay_timer_service` and `piano-boot-guard` appear in no
source on this host and in none of the public sources: `grep -rl` over the fetched k27 tree and over
`upstream/main` (`.patch`, `.h`, `.c`, `.cpp`, `.sh`, `.py`, `.md`) returns zero for `piano_gicv3_init`,
`gicv3` and `ICR0ADDR`. They belong to the probe collection only. Consequence: "the base kernel has no timer or
interrupt path" (`docs/results-2026-11-25-ufs269-base-kernel.md`) is correct about that MachO but was the wrong
frame for the mainline, which gets its interrupts from `ArmCpu`. The memory-probe line of work
(`docs/results-2026-11-26-smprobe-run-and-fd-check.md` and the `oem dump` channel) is retired: it can only ever
tell us about a build we no longer want to make.

## 3. What upstream now reports about the tablet

`docs/STATUS.md` (2026-11-25) is a per-feature table where every row comes from a run on the maintainer's
tablet with the installed daily build **v13.1y12k**: boot from disk with a persistent install, display at 60 Hz,
native touch, keyboard cover, Wi-Fi, speakers, cameras, 5 V charging, Metal, Safari WebGL2/WebGPU, H.264
hardware decode, QuickTime playback, app launches. Still open upstream: Bluetooth pairing, 120 Hz in daily use,
all 16 GB of RAM, hardware encode, and the OS switcher, which is "RAM only, read-only" with no full switch run.
That is the same tablet model as ours, and it is far ahead of anything this workspace reached: our line stopped
before the root volume was published. So the mainline for this repository is now upstream, and our own kernel
probing is history we keep as evidence only.

## 4. What a second unit (ours) still lacks, per `skills/install-piano-macos`

The skill is explicit that a second tablet is gated, and instructs never to force around the refusals.

| gap | status after this round |
| --- | --- |
| G1 k27 tree | **closed** (section 1) |
| G1b SystemOS shims (two shell scripts, 17 and 358 bytes) | open, small: rewrite here, add a `SYSTEMOS_SHIMS` switch, re-pin the manifest group |
| G2 base trust cache | derived from the k27 tree (`make-trust.py`), so open only until a build runs |
| G3 Data seed | buildable from the IPSW Data template (`make-macos-data-seed.sh`, `seed-data-skip-setup.py`) |
| G4 Turnip/Mesa and the Metal plugin | largely public: `tooling/metal/UPSTREAM` names Mesa `Droid-VM/mesa cecc0e9` + UniXNU-turnip's 21 `patches/mesa` + macOS-DroidVM `patches/mesa-macos`, our own delta as `tooling/metal/turnip-a830/{mesa,frontend}/*.patch`; only `libnative_metal_bridge.a` is a prebuilt archive (public sources, public script, Rust nightly arm64e) |
| G6 the writer is pinned to the reference device | **still open**: `tooling/persist/common.py` has no unit-profile hook (`grep -n "unit_profile\|UNIT_PROFILE"` is empty) and `tooling/repro/unit-profile.py` is a read-only comparison. This is the gate for any write on our tablet |
| G7 `boot_a` signing with our own AVB key | open, ours to do locally with `avbtool` + the stock props of `pack-selector.py` |
| G10 another disk layout than the reference | ours to express through the G6 profile: our candidate GPT has userdata 376.272 GiB (factory 461.273 GiB), the `linux_data` store is 64 GiB bound at `first_lba=117735999` over `window_blocks=6291649`, and `esp` starts at `first_lba=124289792` (`docs/results-2026-11-25-esp-record-audit.md`, `docs/results-2026-11-25-layout-recheck.md`), none of which the constants in `tooling/persist/common.py` can describe |

## 5. The build host we do not have

`skills/install-piano-macos` 1.1 requires Apple silicon plus macOS 26.6 or newer and Xcode 27 (the kernel builds
against the macOS 27 SDK with `SDKROOT=iphoneos`, and the reference host was macOS 27.2.1 `26B131` with Xcode 27.2
`27B52`). `ssh mac-lan` is an `M1` `MacBookPro17,1` running **macOS 15.3 `24D60`** with only
`/Library/Developer/CommandLineTools` and no `Xcode.app`: it cannot compile `xnu-12377.121.6`, and it is not the
host the pinned toolchain assumptions were written against. GitHub Actions can build the kernel from public
sources (`docs/ci.md`, `tooling/ci/k27-kernel.sh`, `macos-26` runners) but stops at the Kernel Debug Kit, which
needs an Apple Account (`vars.PIANO_KDK_URL` + `PIANO_KDK_AUTH`, an open item upstream leaves to the owner).
Neither the kernel, the kernel collection, the RAM images nor the 20.9 GB root image can be produced on Windows
or in WSL; the FD and the firmware-side pieces can, and did, build there.

## 6. Decisions taken

- Mainline = upstream. Our commits stay as evidence; the probe tooling stays but gets no further runs.
- Work that needs no Apple build host goes first: the unit profile for G6 (from our own backups, with
  `test-persist.py` simulated sessions), the combined `boot_a` selector image (`tooling/switch/pack-selector.py`
  + `firmware/selector` + our own AVB key, G7), and the FD at the new pins.
- The build-host question goes to the owner as a single decision, because it is either an operating-system
  upgrade on their laptop or money for an Apple-silicon runner; nothing we can build here substitutes for it.

## 7. Our unit measured against the pinned layout (G6 is real, and now quantified)

The unit's own block mapping, read from HyperOS over adb (`readlink /dev/block/by-name/...`, read only):
`userdata`, `linux_rootfs`, `linux_data` and `esp` are `sda34/35/36/37` (UFS LUN 0) while `boot_a` and `vbmeta_a` are
`sde13` and `sde16` (UFS LUN 4). The only GPT heads we hold as backups are LUN 0 (`stock-gpt-head.bin` 34 entries,
`candidate-gpt-head.bin` 37 entries, both `24576` bytes, disk `510165778432`); **no LUN 4 table was ever saved**, and
scanning every `.bin` under `out/` for an `EFI PART` at byte 4096 finds only those five LUN 0 files.

`tooling/repro/unit-profile.py` (upstream, run from a snapshot of `upstream/main` in WSL) against our candidate
LUN 0 head gives the authoritative verdict, kept at `out/unit-profile/piano-7ccdd855-lun0.json` (git-ignored):

    same  LUN 0 blocks: unit 124552192, tooling 124552192
    DIFF  linux_rootfs first/last LBA: unit 102269696..107512575, tooling 56060928..71789567
    DIFF  linux_data first/last LBA: unit 107512576..124289791, tooling 71789568..124552186
    DIFF  ESP (partition 37) already present: unit True, tooling False
    DIFF  LUN 0 primary GPT sha256: unit 12710135..., tooling 90a9d3bb...
    verdict: DIFFERENT ... no write session on this unit until it has its own device profile (G6)

So the reference unit's `linux_data` runs to the end of LUN 0 (about 201 GiB) and has no ESP at install time, while
ours is a 64 GiB store with the ESP carved after it. Two consequences for the plan, both cheap and host-side:

- the G6 profile must carry LUN 0 (ours, from the backups we hold) **and** LUN 4 (not captured yet); the boot
  partitions live on LUN 4, so the profile cannot be written from what we have, and the LUN 4 head has to be read
  once, read-only, from a RAM maintenance boot or from the Ubuntu install tooling's backup step.
- `unixnu.lock` and `tooling/repro/pins.json` are pinned to factory `OS3.0.303.0.WPYCNXM` while this unit runs
  `OS3.0.309.0.WPYCNXM` (upstream gap G11). The per-unit firmware inputs (DSP, GPU zap, Wi-Fi, Bluetooth, touch,
  amplifier presets) have to be taken from our own stock partitions and hashed, not borrowed from the 3.0.303 set.
