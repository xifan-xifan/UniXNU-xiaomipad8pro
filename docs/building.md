# Building the macOS 27.3 line

The local image build targets macOS 27.3 (`26C5035r`) using the `mac17j` kernel collection base. Run the commands from the
repository root on an Apple silicon Mac in a native arm64 shell. The build produces local files under `out/`. For the full
input recovery procedure and installation prerequisites, see the [27.3 line](k273-line.md) and the [install guide](../skills/install-piano-macos/SKILL.md).

## Host tools and inputs

- Run `bash tooling/repro/check-host.sh`. It checks the host, Xcode SDKs, tools and local input gaps. Use the full Xcode 27
  toolchain with the macOS and iPhoneOS SDKs. Check the `ld-classic` entry for the kernel linker. [Host preflight](../tooling/repro/check-host.sh)
- Put Python **3.11 or newer** on `PATH` for the build. The Turnip builder has a separate virtual environment interpreter
  selected by `PYTHON315`. It defaults to Homebrew's `python@3.15`. The build preflight lists Homebrew's `llvm`, `gnu-sed`,
  `bison`, `mtools`, `dtc` and `zstd`, and the `ipsw` utility for AEA key lookup.
  [Turnip build](../tooling/metal/build-turnip-a830.sh) · [Host preflight](../tooling/repro/check-host.sh)
- The source pins are in `unixnu.lock`. `python3 scripts/fetch-pins.py` fetches the checkouts, including the reconstructed
  k27 tree. Set `UNIXNU_RESEARCH` to the checkout parent and `PIANO_K27_TREE` to the prepared baseline tree. The 27.3
  preparation step needs that tree's build inputs and the decompressed 27.3 base. [Lockfile](../unixnu.lock) ·
  [Source fetcher](../scripts/fetch-pins.py) · [k27 tree patches](../patches/k27-tree/README.md)
- The IPSW fetcher verifies Apple inputs against `tooling/repro/pins.json`. The SystemOS and root steps assemble the local
  root image and verify its contents. The base trust bundle and Data seed are local inputs. The install guide records
  input gaps and their prerequisites. [IPSW fetcher](../tooling/repro/fetch-ipsw.py) · [Install guide](../skills/install-piano-macos/SKILL.md)

## Build and verify

```sh
bash tooling/repro/fetch-and-build.sh --dry-run
bash tooling/repro/fetch-and-build.sh
bash tooling/repro/fetch-and-build.sh --check
```

The wrapper runs `host`, `inputs`, `systemos`, `root`, `kernel`, `fd` and `image` in order. Pass step names to run only those
steps. `--check` verifies existing outputs offline. The SystemOS step needs `sudo` in a terminal.
[Build wrapper](../tooling/repro/fetch-and-build.sh)

With the root, seed, trust cache and IPSW-derived inputs in place, the main build steps are:

1. Run `bash tooling/k273/prepare-k27-tree.sh`. It prepares the 27.3 clone, applies the relevant patches and relinks the
   native kernel at the segment addresses of the 27.3 base. Set `K273_KERNEL` to the kernel path it prints if you need to
   select that kernel explicitly. [Kernel preparation](../tooling/k273/prepare-k27-tree.sh)
2. Run `bash tooling/firmware/build-fd.sh` to build the bundled UEFI FD from the pinned firmware sources.
   [FD builder](../tooling/firmware/build-fd.sh)
3. Run `bash tooling/k273/build-k273-image.sh`. It selects the 27.3 tree, kernel, collection base and ADT identity, then drives
   the native kext, kernel collection, ADT and RAM image recipes. On the reference disk layout, set
   `K273_ROOT_PART=linux_rootfs`. The builder's default is `linux_data` for the initial second-unit layout. Data is volatile
   unless persistence is explicitly selected. [27.3 image builder](../tooling/k273/build-k273-image.sh) · [Root layouts](k273-line.md#4-ram-images-and-the-historical-first-boots)
4. Run `python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json`. The verifier reads the images beside
   the build record and uses `mtools` to inspect the payload FAT. It compares the Android boot v2 header fields with the
   reference, recomputes the image ID, checks the FD and boot DTB, and verifies the recorded payload hashes and the macOS
   boot arguments, store settings and addon consistency. Use `--reference`, `--fd` and `--dtb` to select the matching
   inputs for an archived build. For signed images, supply `--avbtool` and `--pubkey` to verify the AVB signature. Success
   is exit 0 and `PASS: 0 failure(s)`. Keep the default **250,000,000-byte** image cap used by both builder and verifier.
   [RAM image verifier](../tooling/bootimg/verify-ram-images.py)

## Kernel collection and staged userland

The default collection lists **27 native kexts**, including `PianoIR`. Wrapper scripts compile the sources under `package/`
into `out/native-kexts/`. The image recipe resolves `@OUT@` in the descriptors and passes them as `NATIVE` to
`tooling/kc/build-kc-k27.sh KERNEL OUTPUT.bin`. The 27.3 wrapper selects the matching `KC_BASE` and
`KC_BUILDER=k27/kc-build273-piano.py` and enables `__LINKEDIT` compaction by default.
[Native kext manifest](../configs/k27-native-kexts.txt) · [Descriptor templates](../tooling/k27/kext/) · [Kernel collection builder](../tooling/kc/build-kc-k27.sh)

The collection builder checks the segment layout, validates native descriptors before linking and checks the link log for
VM-era bundles afterwards. It uses best-fit placement by default with the patched builder and reports remaining space.
It produces the collection, a link log, a native-placement record, a symbol listing and SHA-256 checksums. The Bluetooth
codeless metadata is normalized in a separate copy of the link input. [Native kext rule](bringup-debugging.md#5-the-native-kext-rule-2026-11-11) · [Collection layout](kc-space.md)

The userland manifest builds **14 arm64 Mach-Os** across its non-planned units. Thirteen use `build = "cc"` and
`piano-rotated` comes from a build script. Planned entries such as `piano-wifi` and optional bundled apps are outside that
count. The staging tool installs into a copy of the Data seed, records the files, and adds trusted Mach-Os' CDHashes to the
trust cache. [Userland manifest](../configs/userland.toml) · [Staging tool](../tooling/userland/stage.py) · [Userland reference](userland.md)

## Camera and DisplayPort tables

Both sets of checked-in data come from the same pinned Ubuntu piano kernel revision. Their provenance files identify the
source files and hashes. Generation validates the repository manifests before emitting headers for the builds.

| Inputs | Build-time check and output |
| --- | --- |
| S5KJN1 rear sensor, OV32D40 front sensor and CSIPHY lane tuning | `python3 tooling/camera/sensor-tables.py check data/camera`. The tool's `header` step emits `CameraTables.h` for `PianoCamera`. [Camera provenance](../data/camera/README.md) |
| SM8750 QMP DisplayPort PHY tuning and piano's per-lane rate cap | `python3 tooling/dp/dp-phy-tables.py check data/dp`. The tool's `header` step emits `DpPhyTables.h` for `PianoDp` and its host tests. [DisplayPort provenance](../data/dp/README.md) |

For host validation of an existing checkout or build, use the [host checklist](host-checklist.md). The [CI reference](ci.md)
describes the GitHub-hosted checks and release artifacts.
