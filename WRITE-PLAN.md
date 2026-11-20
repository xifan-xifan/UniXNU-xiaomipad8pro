# WRITE-PLAN: HyperOS 3.0.309 and native macOS 27.3

Status: draft, not authorized for execution.

This document defines the gate before any GPT, boot, ESP, root, Data, or boot-triplet write. No step below has been executed.

## Preconditions

1. Obtain the exact native k27 checkout at `11ac316b789d81fc9f734e15645456f536c05141` and prepared XNU tree `de16623d957cccff6dfb78d8eca4a79842af9702`.
2. Build and hash one matched set of 26C5035r kernel, KC, DTB, trust cache, SystemOS, root, Data seed, ESP, and firmware FD inputs.
3. Prove the root and Data layout fits the current device without relying on the author's historical LBAs. Current captures show no free LUN 0 GPT extent and no qualified shrink of encrypted HyperOS userdata.
4. Define an Android recovery set from the verified OS3.0.309 package. The external v1.6.1 switch bundle targets 3.0.303 and is not an approved recovery set.
5. Add a switching wrapper that checks Fastboot product, unlock state, A slot, success bit, retry count, partition sizes, input hashes, and post-write whole-partition readback. Preserve the checked A-slot triplet and GPT boundary captures.
6. Validate a RAM-only diagnostic image and every firmware/configuration path. The final FD must be proven not to fall through to UFS configuration when the RAM payload is absent.

## Candidate layout and execution gates

The offline GPT study uses 4 KiB sectors and proposes these inclusive LUN 0 ranges. See [the stock recovery and storage study](docs/results-2026-11-20-storage-layout-recovery.md) for its CRC and rollback evidence.

| Region | First LBA | Last LBA |
|---|---:|---:|
| Shortened userdata | 3,632,128 | 102,269,695 |
| macOS root | 102,269,696 | 107,512,575 |
| macOS Data | 107,512,576 | 124,289,791 |
| ESP | 124,289,792 | 124,551,935 |

The last three regions overlap the current userdata partition. The current encrypted F2FS filesystem must never be mounted against a shortened block device until a qualified migration or recreation procedure has completed. Restoring the stock GPT restores partition geometry only. If any writes replace the existing F2FS tail, restoring geometry cannot recover those bytes. Under the no-backup instruction, a layout that overwrites those bytes requires a verified stock reinitialization procedure and explicit approval of the resulting data loss.

The live A-slot HyperOS 3.0.309 read-only check on 2026-11-20 found `/data` mounted as F2FS through `dm-56` with `inlinecrypt`. `/vendor/etc/fstab.qcom` marks userdata `formattable` and specifies wrapped-key file and metadata encryption, with the metadata key directory at `/metadata/vold/metadata_encryption`. The stock symlinks resolve userdata to `sda34` and metadata to `sda20`. An unprivileged ADB shell could not read their block geometry from sysfs, so the candidate LBAs still use the verified GPT captures. Free filesystem blocks do not establish a free GPT range. A recreation trial must verify that stock recovery initializes the encrypted mapping and mounts `/data` on the proposed shortened geometry before that recovery route can be relied on. The fstab flag alone does not establish that outcome.

The diagnostic boot gate comes before any device write. Once the temporary firmware path is qualified, test only the diagnostic stages that can run with the stock layout. A desktop test requires the root storage expected by its matched image set. If that storage requires persistent preparation, finalize the affected ranges, pre-write evidence, readback hashes, and failure recovery before requesting approval. The provisional ranges above do not authorize any write.

After authorized storage preparation and successful desktop testing, qualify the paired boot images, ESP and Data binding before installing a persistent selector. Verify the A-slot success and retry state before and after the switch, then record three cold starts, two Android/macOS round trips, Data readback after reboot, normal restart, hard-reset recovery and the v2 daily-use checks.

## Rollback

The official GPT template matches LUN 0 geometry after adjusting the userdata end for disk capacity. Its partition GUIDs and attributes differ from the captured table. Use the verified device captures as the rollback source. [GPT template comparison](docs/results-2026-11-20-stock-gpt-template-comparison.md)

- A failed preflight or readback aborts before reboot.
- The temporary boot gate requires evidence for the distributed ABL ramdisk selection and all persistent write paths. Reviewed Qualcomm reference source is pinned to CodeLinaro commit `aef4e86045bd151a54d33bab8ae5e8e62462a8f4`; `ClearUnbootable()` writes primary and backup GPT only if it changes the active-slot flag. Previously captured `unbootable=0` makes that operation a no-op only if current and if the distributed binary follows this source. Xiaomi binary build provenance/configuration and remaining non-GPT persistence paths are not established. See [stock ABL inspection](docs/results-2026-11-20-stock-abl.md). The diagnostic BDS deliberately halts when the menu fails or returns. Before a device attempt, establish the hardware reset and return path to ABL Fastboot.
- A failed persistent step restores the immediately preceding verified region in reverse dependency order, then reads it back before reboot.
- Boot-triplet rollback uses only the three verified device-captured A-slot images (`boot_a`, `init_boot_a`, `vendor_boot_a`); the offline verifier checks size and SHA-256. The official package has a different `init_boot` ramdisk and is not interchangeable.
- The official package `flash_all_except_storage.sh` (SHA-256 `180b79a08b7ffcf66df678c07e4afb3b66cd2c01a3a6bb5b7b855f5d6204de28`) erases `boot_ab`, flashes firmware and system partitions including `init_boot_ab`, `vendor_boot_ab`, `super`, `misc`, recovery and `boot_ab`, sets slot A, then reboots. Static review found no GPT, `userdata` or `metadata` write in this script. It is a broad stock-system recovery operation, not a boot-triplet rollback or GPT restoration procedure.
- The package `flash_all.sh` (SHA-256 `8610cf859ab4fdda40acaa81c1c25ef27b233d8622df716587e7c518360ccb10`) additionally erases and flashes `metadata` and flashes `userdata.img`. It destroys existing userdata and is excluded from the preservation plan. Neither vendor script has been run.
- Full stock-layout recovery remains a separate procedure: after altered partition boundaries or overwritten sectors, it may require restoring both GPT copies and reinitializing `/data` and `/metadata`; neither vendor script restores GPT. Qualify the exact commands, recovery entry/return path and readback before approval.
- The plan cannot be executed until the exact image set, free-space/layout proof, recovery set, and write-specific hashes are recorded in the manifest.

Before approving boot-triplet rollback, run tooling/persist/verify-android-rollback.py against the selected capture directory. Both retained capture directories passed. Substituting the official init_boot under the captured filename failed the hash check.

The post-write readback transport remains unqualified. Inspection of the pinned Qualcomm reference command source and the decompressed stock ABL strings found no `fetch` command marker. This search cannot establish the device command set. Qualify a read-only path for all three complete partitions before relying on this check. The host-only ABL slot fixture and offline verification of both retained Android triplets passed on 2026-11-20.

Offline inspection of the retained Ubuntu v1.1.0 maintenance image matched its SHA-256 and the embedded gzip initramfs digest against the release manifest. The default maintenance init mounts proc, sysfs, devtmpfs, devpts and tmpfs, then starts the USB console and idle watchdog. The watchdog and explicit Fastboot exit use a reboot-mode driver targeting PMIC SDAM, with a normal reboot fallback for the watchdog. This supplies a candidate transport for whole-partition hashes. Qualification on 3.0.309 still depends on the stock ABL write-path audit and confirming the maintenance kernel and reset behavior on the tablet.

The retained maintenance image embeds a Linux kernel whose SHA-256 matches the release manifest (`7051dc0410c4a1a8004d082f969435a4805aecc6dd21ab7b7ab2aebd5caedf1f`). Its embedded config disables block-backed pstore and enables RAM-backed pstore, EFI variable pstore, and hibernation. Decompiling the packaged DTB confirms a 4 MiB reserved `ramoops` region split between console and pmsg, with no EFI handoff properties or DT boot arguments. The packaged boot arguments specify neither a hibernation resume device nor a pstore block device. The runtime DTB can differ after ABL handoff. EFI runtime availability and firmware-side logging remain open, so the host inspection does not qualify a no-write boot.

The v1.1.0 release manifest identifies kernel commit `d2710cd13b337ef5172bd2394839e9d9673ff88a` and integration commit `b0a3b219a9de7ad4739f19feb3f3bc5306c891ec`. The matching packaged kernel contains the `efi_pstore.pstore_disable` parameter name. The pinned kernel source (`drivers/firmware/efi/efi-pstore.c`, retrieved through the GitHub contents API) confirms that enabling this parameter skips EFI pstore registration. The released image leaves the parameter unset. An audited maintenance candidate should set `efi_pstore.pstore_disable=1` and verify the effective command line. Stock ABL runs before the kernel and requires a separate write-path audit.

Host candidate: boot-ram-maintenance-efi-pstore-disabled.img, stored beside the verified v1.1.0 input in D:/Temp/piano-v110-ranges-bc75e4c7-38d0-4bd0-8eb7-f04886abad71. SHA-256 64164d4c221386c64b2e92c542efb8474b4f397ca997a2db7f2a9ffe4ed79e3a. The build appended the parameter in the Android v2 primary command line field and verified the file length and all bytes outside that field. Pending hardware qualification and stock ABL audit.

A host fixture exercised the existing maintenance restore method with matching and mismatching whole-region digests. The mismatch raised Stop before any later session step. The host only shuts down its HTTP service on this error. The maintenance image has an independent idle watchdog with a normal reboot fallback, so the on-device failure procedure must account for that timeout before any boot triplet write is approved.

The host built and checked a manual-exit readback candidate. The [dated maintenance result](docs/results-2026-11-20-maintenance-manual-exit.md) records its restart changes. Before any persistent write, qualify the readback path and verify that loss of the USB link or the host does not restart the tablet.

## Recovery branches (2026-11-20 audit)

There are two different recovery cases. If the GPT remains stock, the exact captured A-slot `boot_a`, `init_boot_a`, `vendor_boot_a` and verified stock GPT are the direct rollback inputs; the captured `init_boot_a` differs from the factory package and must not be interchanged. If a native layout has shortened userdata and overwritten its tail, returning to the original GPT does not restore the lost filesystem. The verified stock package's 45 GiB `userdata.img` and 64 MiB `metadata.img` fit the original partition capacities, but the package script `flash_all.sh` has not been run or qualified and applies other broad writes. It is not an approved shortened-layout recovery procedure.

The recovery ramdisk's fstab marks `/data` and `/metadata` formattable and binds encryption metadata to `/metadata/vold/metadata_encryption`. The wipe flow could therefore destroy the existing F2FS and encryption metadata while initializing a shortened GPT, but it has not been exercised and does not establish that HyperOS can mount or boot it. Proving that requires a dedicated disposable test layout or a reviewed recovery build/run that captures the actual recovery log, confirms the `make_f2fs` target block length and metadata initialization, then cold boots 3.0.309 and verifies `/data` is healthy. None is currently available without using this tablet's userdata tail.

The live system also reports `ro.virtual_ab.enabled=true` and exposes `miui.snapshot.service`; an attempted `update_engine_client --status` is unsupported by this vendor client, so merge status remains unknown. Before any GPT or userdata operation, require a fresh `snapshot-update-status=none` from stock ABL Fastboot or Android userspace Fastboot. Both transports returned `none` on this unit. Use ABL, identified by `is-userspace=no`, for the slot success, bootability and retry values. If snapshotted or merging, stop and resolve that state through the vendor-supported path first. AOSP Virtual A/B rules explicitly guard userdata and metadata erase/wipe during `MERGING` or `SNAPSHOTTED` states. The state query is an additional preflight, not permission to wipe or resize.

The historical 4 GiB seed footprint fits the provisional 64 GiB Data region. Its sibling trust bundle is byte-identical to the Apple-only bundle assembled from 26C5035r. The 28 matching historical manifests all disable persistence, so the pair still requires native boot and Data qualification. The maintainer's documented 200 GiB profile needs 215,828,664,320 bytes including its bitmap, header and 1 GiB reserve. That exceeds the provisional region's 68,719,476,736 bytes. Finalize the Data region against the actual selected seed and profile before generating writable GPT artifacts.

For the preserved 24 GiB first-install seed, the provisional Data extent above places the 6,291,649-block store at LBA 117,735,999 through 124,027,647, inclusive. This leaves 262,144 blocks at the tail for the 1 GiB reserve. The stock ESP builder's `--seed` path still uses the author's partition end and calculates LBA 117,998,394 for this seed. NativeUfs rejects a mismatched `data-store-lba`. Do not use that builder output for this provisional layout. Derive the binding from the approved partition boundaries and verify it against the final ESP boot arguments before any write.
