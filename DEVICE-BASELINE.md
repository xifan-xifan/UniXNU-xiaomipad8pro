# Xiaomi Pad 8 Pro Device Baseline

Audit date: 2026-11-19. Collection was read-only. No fastboot transition, partition write, reboot, or device-side file creation was performed.

## Confirmed on the tablet

| Item | Observed value |
|---|---|
| Product / device | `25091RP04C` / `piano` |
| SoC | `SM8750P` |
| Android | Android 16, API 36 |
| HyperOS build | `OS3.0.309.0.WPYCNXM` |
| Android kernel | `6.6.118-android15-8-gc44b714366cc-abogki519650608-4k` |
| Memory | `MemTotal=15705116 kB` (about 15 GiB visible to Linux) |
| ADB | Authorized; one device listed as `device` |
| Root | `su` returns uid 0 in Magisk context |
| Bootloader | Unlocked; `ro.boot.verifiedbootstate=orange` |
| Active slot | `_a` (`ro.boot.slot_suffix`) |
| User data | `/data` is 461 GiB total, 22 GiB used, 439 GiB available at audit time |

No device serial, MAC address, calibration value, or private backup location is recorded here.

## Storage observations

Root-readable Linux block metadata reports `sda` as the user-area disk, with a 4096-byte logical block size and 996417536 512-byte sysfs sectors. `/dev/block/by-name/userdata` resolves to `sda34`; its sysfs start and size are 29057024 and 967360472 512-byte sectors. These are Linux sysfs sector units, not confirmed GPT LBAs. The current table has `boot_a`, `boot_b`, `vendor_boot_a/b`, `init_boot_a/b`, `super`, `userdata`, and the normal Android partitions. No `linux_rootfs`, `linux_data`, or named ESP partition is present.

The boot triplet resides on the block device exposed as `sde` (sysfs path ends in UFS target LUN 4); `boot_a` is `sde13` (start 618800, size 196608 512-byte sectors), `vendor_boot_a` is `sde23` (start 970800, size 196608), and `init_boot_a` is `sde28` (start 1438256, size 16384). The three device logical block sizes checked were 4096 bytes. This mapping is inventory only; it is not a write recipe.

The complete GPT entries, primary/backup GPT CRCs, block-control success flags, and retry counters have not been decoded. Android's `bootctl` command is absent/inaccessible and the read-only property query only established the active suffix. These remain required before a storage or switching plan can be approved. The full disk/LUN inventory and verified recovery package are also outstanding.

## Recovery asset inventory

An external local backup volume contains a large historical tablet backup (about 210 GB across the inventoried categories). It is preserved and was not modified. It is not yet accepted as a 3.0.309 recovery set: its source build is not established, and the existing verification log reports `Unrecognized archive format` for several large archives, including the aggregate media archive. Individual DCIM and Pictures markers report entry/byte totals, but archive parsing and content-level comparison were not confirmed in this audit. Do not delete or overwrite this copy; do not rely on it as the sole recovery path.

The older research workspace has an unrelated 67 MB `kernel` and 3.6 MB `ramdisk`, but no `out/k27`, `out/k273`, or `out/inputs` tree. Their provenance does not match the pinned native 27.3 input set, so they are retained as historical evidence and excluded from candidate builds.

## Remaining read-only work

- Decode every relevant LUN's GPT with a verified parser and record partition boundaries in native LBA units.
- Determine A/B bootable/successful flags and retry counts using an authoritative read-only method.
- Establish whether an official, region-matched 3.0.309 package and a second verified copy of the required recovery assets exist.
- Re-verify the requested photo/screenshot backup without extracting over or changing its only known copy.
