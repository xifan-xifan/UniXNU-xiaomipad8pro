# 2026-11-26 - LUN 4's GPT head is ours too, and it agrees with the reference; the kext header gap is narrower than recorded

Follows [results-2026-11-26-unit-profile-g6-code.md](results-2026-11-26-unit-profile-g6-code.md). Host: this Windows checkout, the
desk Mac, and the tablet through a RAM-only maintenance session. Upstream moved from `d70c16ce` to `23f6425f` (8 commits) and was
fetched here; the Mac cannot reach GitHub, so upstream arrives through this machine.

## The maintenance session, and what proves it wrote nothing

`fastboot boot out/inputs/v110/release/boot/boot-ram-maintenance.img` (49,180,672 B, sha256 `54001a50a13fe80c...`, equal to the
pin `maint-boot` takes), reached over the USB NCM link at `192.168.88.2:2323` through `tooling/native/maint-shell.py`, exited with
`sync; piano-maint fastboot` back to ABL, then `fastboot reboot`. Evidence that nothing was written:

- `/proc/cmdline` of the running image carries `piano.diag=maintenance piano.maint_idle=600`, and `mount | grep -E "sd|ufs"` printed
  nothing: the only 7 mounts are ramfs/proc/sys/dev. UFS was enumerated (`/sys/block`: `sda` 996,417,536 x 512 = 124,552,192 x
  4 KiB, `sde` 3,031,040 x 512, `sdb`/`sdc` 24,576, `sdd` 16,384, `sdf` 131,072) but no filesystem on it was mounted.
- the session was six `dd` reads of 6 blocks each and one base64 of them; the tablet's own root shell, no writes, no partition
  tools.
- after the reboot HyperOS came back by itself: `sys.boot_completed=1`, `OS3.0.309.0.WPYCNXM`, `ro.boot.slot_suffix=_a`.

The capture pipeline is byte-exact, which is what makes the new head trustworthy: re-reading LUN 0's head the same way hashes to
`12710135635beee2a5de1dd910924a3d2d26e09a6f368c191b879fd6b41a499b`, the value recorded from an earlier session.

## LUN 4: the reference layout, on our unit

Our LUN 4 head (`out/unit-profile/piano-7ccdd855-lun4-head.bin`, 24,576 B): 4 KiB sectors, 378,880 blocks, primary sha256
`dfdc685d3418977d70714f295c96ac75432dc0f4e5d96fcf19d8b894b4565a74`, disk guid `C3A31221-C703-CB3A-1E7F-C0BDC1D8BFBD`, 36 named
partitions before the `_b` half. The five the write tooling cares about:

| Partition | GPT entry index | partition number (the `n-name.img.zst` backups) | first LBA | last LBA |
|---|---|---|---|---|
| boot_a | 12 | 13 | 77,350 | 101,925 |
| vbmeta_a | 15 | 16 | 102,022 | 102,053 |
| dtbo_a | 16 | 17 | 102,054 | 108,197 |
| vendor_boot_a | 22 | 23 | 121,350 | 145,925 |
| init_boot_a | 27 | 28 | 179,782 | 181,829 |

`tooling/persist/common.py` names those partitions in its own comments (`13-boot_a`, `16-vbmeta_a`, `23-vendor_boot_a`,
`28-init_boot_a`) and its `BOOT_A_START_512, BOOT_A_SIZE_512 = 618800, 196608` is exactly what this head yields
(`77350 x 8`, `24576 x 8`). So **gap G6 on this unit is a LUN 0 problem only**: the LBAs of `userdata`/`linux_rootfs`/`linux_data`
are ours, everything on LUN 4 - partition numbers, `boot_a`'s byte offsets, the partition sizes (boot_a and vendor_boot_a 24,576
blocks = 100,663,296 B, the sizes the tooling carries) - already matches. `unit_profile.py --lun0 ... --lun4 ...` now produces one
profile covering both, and `PIANO_UNIT_PROFILE` pointing at it rebinds the places while `REFERENCE` keeps the reference numbers
readable; the verdict stays DIFFERENT (exit 2) because of LUN 0.

## ld-classic (the kernel's link input) is in place

`/Users/zhangqi/bin/ld-classic` is a symlink to the locally built `ld64-957.1` (`PROGRAM:ld-classic PROJECT:ld64-957.1`, built
2026-06-08). Upstream takes it from `$LD_CLASSIC` or `xcrun -f ld-classic` (`tooling/ci/k27-kernel.sh:132`) and
`tooling/repro/check-host.sh:76` looks for it staged as `out/x12377/ldc/ld` **in the k27 tree** - both our trees
(`~/Codex/k27-tree`, `upstream-tip/out/k273/k27-tree`) have `out/x12377/ldc`, so this input needs no further work from us; the
kernel links with `-threaded_starts_section` because of it, which plain `ld` from the current Command Line Tools does not accept.

## The kext header gap, re-checked against everything on this disk

The gap is real but smaller than the previous note said, and one avenue it dismissed is open:

- The 27.2 KDK was expanded properly this time (`pkgutil --expand-full` handles its `pbzx` payload; `tar` does not): 5.2 GB under
  `~/Codex/kdk/full`. It contains **no** `IOBlockStorage*.h` and no `IOKit/storage` directory. The KDK is not the source.
- The k27 tree's `installhdrs` output under `Kernel.framework/.../IOKitUser/` is 22 files of ~44 bytes each: `IOBlockStorageDevice.h`
  is literally `#include <DriverKit/IOBlockStorageDevice.h>`. Those are forwarders, not headers, which is why they fooled the
  earlier check (the file names matched).
- **The macOS SDK does have the tree**: `$(xcrun --show-sdk-path)/System/Library/Frameworks/IOKit.framework/Headers/storage/` holds
  31 headers including `IOBlockStorageDevice.h`, `IOBlockStorageDriver.h` and the partition-scheme headers, and
  `IOKit.framework/Headers/{usb,hid}` likewise. An include directory whose `IOKit/storage` resolves there is exactly the shape
  `-I $K/out/x12377/storage-abi/include` implies, and it is reproducible from a public Apple download.
- What that does not settle is the ABI: those are the user-side IOKit headers, `NativeUfs`/`NativeRamBlock` build with the
  *kernel's* recipe and link into the kernel collection against Apple's IOStorageFamily, and the reference check
  (`build/storage-abi/interface-audit.json`, read by the tree's `scripts/audit-reference.py`) is not published - neither is any
  generator for `storage-abi`, `xnu-m/osfmk` or `inc-gfx` (the k27 tree's `sources/` subtrees, which `audit-reference.py` reads,
  are not present here at all). So an SDK-built object set stays **our unverified reconstruction**, labelled as such; the two
  remaining unknown include trees are `xnu-m/osfmk` and `inc-gfx`, and neither is explained by the SDK.

Upstream's own list still puts these outside the published inputs (`README.md`: "some inputs are still local to the original setup
... the write tooling's single-unit pins"), and `docs/STATUS.md` still lists "the install guide for other units (in progress)" and
the Android/macOS switcher as "RAM only, read-only - a full switch has not been run". Their installed daily build (v13.1y12m) is
an ESP-only update on their own base install, and the repository publishes hashes rather than images, so their system is not
something this unit can download and install.