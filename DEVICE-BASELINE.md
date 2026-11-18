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

Root-readable Linux block metadata reports six UFS LUNs, all with 4096-byte logical blocks. A read-only parser checked the primary and backup GPT header CRCs and entry-array CRCs on all six LUNs; all were valid and primary/backup entry arrays matched. LUN 0 (`sda`) contains 124552192 logical blocks and 34 entries; LUNs 1-5 (`sdb`-`sdf`) contain 3072, 3072, 2048, 378880, and 16384 logical blocks, with 6, 6, 4, 79, and 8 entries respectively. No full GPT dump or device GUIDs are included in this repository.

On LUN 0, `super` ends at LBA 3632127 and `userdata` occupies LBAs 3632128-124552186, through the last usable GPT LBA. Thus no unallocated range exists after userdata for a native root/Data/ESP layout. The table contains Android `boot_a`, `boot_b`, `vendor_boot_a/b`, and `init_boot_a/b`, but no `linux_rootfs`, `linux_data`, or named ESP partition.

The boot triplet resides on LUN 4 (`sde`): `boot_a` is `sde13` (start 77350, last LBA 101925), `vendor_boot_a` is `sde23` (start 121350, last LBA 145925), and `init_boot_a` is `sde28` (start 179782, last LBA 181829). These are native 4096-byte GPT LBAs. This mapping is inventory only; it is not a write recipe.

## Verified read-only captures

On 2026-11-19, the A-slot `boot_a`, `vendor_boot_a`, `init_boot_a`, and 4 MiB `misc` partition were captured with root-readable `toybox dd` to independent E: NVMe and F: USB destinations. Exact partition lengths were checked; both copies match by SHA-256, and a fresh device reread of each partition matched the stored E: copy. The file hashes and paths are recorded in `outputs/manifests/native-macos-27.3.json`. These are boot-control/rollback inputs only, not a full device backup or verified HyperOS recovery package. No GPT images or userdata are included.

The standard AOSP slot-control structure read from `misc` did not have the expected magic or valid CRC at the checked location. A temporary Fastboot query on 2026-11-19 provided authoritative bootloader values instead: current slot A, both slots `successful=yes`, both retry counts 6. The tablet was returned with `fastboot reboot`; ADB then confirmed `piano`, build `BP2A.250605.031.A3`, slot `_a`. These values are a point-in-time observation, not proof of switching or recovery behavior.

GPT entries and primary/backup CRCs are now verified. A/B bootable/successful flags and retry counters have not been decoded. Android's `bootctl` command is absent/inaccessible and the read-only property query only established the active suffix. These remain required before a switching plan can be approved. A verified recovery package is also outstanding.

## Recovery asset inventory

An external local backup volume contains a large historical tablet backup (about 210 GB across the inventoried categories). It is preserved and was not modified. It is not yet accepted as a 3.0.309 recovery set: its source build is not established, and the existing verification log reports `Unrecognized archive format` for several large archives, including the aggregate media archive. The separate DCIM and Pictures tar files parse successfully and contain 5519 and 571 entries; they have not yet been compared file-by-file with the tablet or validated against a content-hash manifest. Do not delete or overwrite this copy; do not rely on it as the sole recovery path.

The older research workspace has an unrelated 67 MB `kernel` and 3.6 MB `ramdisk`, but no `out/k27`, `out/k273`, or `out/inputs` tree. Their provenance does not match the pinned native 27.3 input set, so they are retained as historical evidence and excluded from candidate builds.

## Remaining read-only work

- Determine A/B bootable/successful flags and retry counts using an authoritative read-only method.
- Establish whether an official, region-matched 3.0.309 package and a second verified copy of the required recovery assets exist.
- Compare requested photo/screenshot files against the tablet without extracting over or changing the existing archive.
