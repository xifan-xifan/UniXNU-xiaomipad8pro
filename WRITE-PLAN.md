# WRITE-PLAN: HyperOS 3.0.309 and native macOS 27.3

The owner authorized writes within the deployment scope and declined a new tablet backup. The candidate GPT was written and independently read back. HyperOS 3.0.309 completed boot on slot A and a synced Android marker survived a subsequent boot. See [the GPT result](docs/results-2026-11-21-candidate-gpt-tablet.md) and [the HyperOS result](docs/results-2026-11-21-candidate-hyperos.md).

This document records the candidate GPT trial and defines the remaining gates before boot, ESP, root, Data or boot-triplet writes. The candidate GPT is installed; stock GPT rollback has not been qualified. Root, macOS Data, ESP and boot-triplet deployment remain gated below.

## Preconditions

1. Reproduce the upstream native k27 checkout and prepared XNU tree, or freeze and document the independently prepared source, patch series, build inputs and hashes. Qualify the resulting kernel against the 26C5035r collection and its startup, storage and driver contracts.
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

The last three regions overlap the current userdata partition. The current encrypted F2FS filesystem must never be mounted against a shortened block device until a qualified migration or recreation procedure has completed. Restoring the stock GPT restores partition geometry only. If any writes replace the existing F2FS tail, restoring geometry cannot recover those bytes. Under the no-backup instruction, a layout that overwrites those bytes requires a verified stock reinitialization procedure. The owner reports the tablet was factory-reset and declined a tablet backup. The owner authorized the candidate layout trial.

The live A-slot HyperOS 3.0.309 read-only check on 2026-11-20 found `/data` mounted as F2FS through `dm-56` with `inlinecrypt`. `/vendor/etc/fstab.qcom` marks userdata `formattable` and specifies wrapped-key file and metadata encryption, with the metadata key directory at `/metadata/vold/metadata_encryption`. The stock symlinks resolve userdata to `sda34` and metadata to `sda20`. An unprivileged ADB shell could not read their block geometry from sysfs, so the candidate LBAs still use the verified GPT captures. Free filesystem blocks do not establish a free GPT range. A recreation trial must verify that stock recovery initializes the encrypted mapping and mounts `/data` on the proposed shortened geometry before that recovery route can be relied on. The fstab flag alone does not establish that outcome.

The first hardware experiment is the frozen diagnostic RAM container `40482bc38d1566cddc1c4b74dbf9004a616355581f99e399feaf496004aa6dd5` (`UbuntuPiano:/tmp/piano-ram-build.0YIaUj/menu-bds-final.ac77mrn7/boot-piano-uefi-ram-menu-isolated.img`). Before loading it, record ABL `is-userspace`, `snapshot-update-status`, current slot, successful/bootable flags, retry counts. After the diagnostic path returns, collect the same ABL values and confirm Android `sys.boot_completed=1` and build `OS3.0.309.0.WPYCNXM`. Abort on any slot flag, retry, or snapshot change. This experiment does not authorize GPT, userdata, metadata, ESP, or boot-partition writes. These observations do not cover firmware variables or rollback state. The diagnostic RAM boot was attempted and returned to HyperOS after a hardware reset. The menu was not observed. A separate maintenance RAM boot verified USB access, stock boot partition hashes and a return to HyperOS. See [diagnostic recovery](docs/results-2026-11-21-diagnostic-boot-recovery.md) and [maintenance RAM boot](docs/results-2026-11-21-maintenance-ram-boot.md). The existing authorization covers a temporary boot once the review establishes no persistent disk writes. Whole-partition readback is a separate qualification before persistent switching.

The diagnostic boot gate comes before any device write. Once the temporary firmware path is qualified, test only the diagnostic stages that can run with the stock layout. A desktop test requires the root storage expected by its matched image set. If that storage requires persistent preparation, finalize the affected ranges, pre-write evidence, readback hashes, and failure recovery before applying the provisional ranges.

After authorized storage preparation and successful desktop testing, qualify the paired boot images, ESP and Data binding before installing a persistent selector. Verify the A-slot success and retry state before and after the switch, then record three cold starts, two Android/macOS round trips, Data readback after reboot, normal restart, hard-reset recovery and the v2 daily-use checks.

## Rollback

The official GPT template matches LUN 0 geometry after adjusting the userdata end for disk capacity. Its partition GUIDs and attributes differ from the captured table. Use the verified device captures as the rollback source. [GPT template comparison](docs/results-2026-11-20-stock-gpt-template-comparison.md)

- A failed preflight or readback aborts before reboot.
- The temporary boot gate requires evidence for the distributed ABL ramdisk selection and all persistent write paths. Reviewed Qualcomm reference source is pinned to CodeLinaro commit `aef4e86045bd151a54d33bab8ae5e8e62462a8f4`; `ClearUnbootable()` writes primary and backup GPT only if it changes the active-slot flag. Previously captured `unbootable=0` makes that operation a no-op only if current and if the distributed binary follows this source. Xiaomi binary build provenance/configuration and remaining non-GPT persistence paths are not established. See [stock ABL inspection](docs/results-2026-11-20-stock-abl.md). The diagnostic BDS deliberately halts when the menu fails or returns. Before a device attempt, establish the hardware reset and return path to ABL Fastboot.
- A failed persistent step restores the immediately preceding verified region in reverse dependency order, then reads it back before reboot.
- Boot-triplet rollback uses only the three verified device-captured A-slot images (`boot_a`, `init_boot_a`, `vendor_boot_a`); the offline verifier checks size and SHA-256. The official package has a different `init_boot` ramdisk and is not interchangeable.
- The official package `flash_all_except_storage.sh` (SHA-256 `180b79a08b7ffcf66df678c07e4afb3b66cd2c01a3a6bb5b7b855f5d6204de28`) erases `boot_ab`, flashes firmware and system partitions including `init_boot_ab`, `vendor_boot_ab`, `super`, `misc`, recovery and `boot_ab`, sets slot A, then reboots. Static review found no GPT, `userdata` or `metadata` write in this script. It is a broad stock-system recovery operation, not a boot-triplet rollback or GPT restoration procedure.
- The package `flash_all.sh` (SHA-256 `8610cf859ab4fdda40acaa81c1c25ef27b233d8622df716587e7c518360ccb10`) additionally erases and flashes `metadata` and flashes `userdata.img`. It destroys existing userdata and is excluded from the preservation plan. Neither vendor script has been run.
- The candidate ESP write (`esp-candidate`) records the whole 1 GiB window's SHA-256 before writing and makes no byte copy, per the owner's no-backup instruction. LBA 124289792-124551935 lies past the candidate `userdata` (ends 102269695) and past `linux_data` (ends 124289791), so the old bytes were the tail of the factory-reset userdata and are not needed. If the write is wrong, the recovery path is to rebuild the ESP from the recorded inputs and rewrite the same window; the original bytes cannot be restored.
- Full stock-layout recovery remains a separate procedure: after altered partition boundaries or overwritten sectors, it may require restoring both GPT copies and reinitializing `/data` and `/metadata`; neither vendor script restores GPT. Qualify the exact commands, recovery entry/return path and readback before persistent writes.
- The plan cannot be executed until the exact image set, free-space/layout proof, recovery set, and write-specific hashes are recorded in the manifest.

Before approving boot-triplet rollback, run tooling/persist/verify-android-rollback.py against the selected capture directory. Both retained capture directories passed. Substituting the official init_boot under the captured filename failed the hash check.

The post-write readback transport is now qualified for read-only maintenance sessions on this 3.0.309 tablet: the frozen maintenance RAM image exposed USB NCM/root shell, and complete reads of the A-slot boot triplet matched the retained captures on 2026-11-21. This does not authorize writes or prove that stock ABL has a fetch command. The host-only ABL slot fixture and offline verification of both retained Android triplets also pass.

Offline inspection of the retained Ubuntu v1.1.0 maintenance image matched its SHA-256 and the embedded gzip initramfs digest against the release manifest. The default maintenance init mounts proc, sysfs, devtmpfs, devpts and tmpfs, then starts the USB console and idle watchdog. The watchdog and explicit Fastboot exit use a reboot-mode driver targeting PMIC SDAM, with a normal reboot fallback for the watchdog. The 2026-11-21 tablet run confirmed the USB NCM shell, read-only UFS access, and explicit return to ABL; see the dated maintenance result. This qualifies observation and readback, not persistent storage preparation.

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

The ESP builder now accepts `--data-extent FIRST LAST` in inclusive 4 KiB LUN 0 LBAs. It derives the store binding from that extent and the seed, refuses a store that does not fit with its reserve, and records the explicit extent as the binding source. Host coverage builds the 24 GiB candidate against the provisional range and rejects an undersized range. This does not authorize or generate device write images; the GPT extent remains provisional.

The SHNGROOT v2 manifest for the retained 26C5035r OS and unshimmed SystemOS includes two per-block SHA-256 tables. The current repository generator ran on the Mac and produced a 21,100,494,848-byte layout with 1 MiB alignment. Within the provisional 20 GiB root extent, this leaves 374,341,632 bytes (357 MiB). Recheck the final shimmed SystemOS with its own hash table before fixing the write ranges. The retained manifest is a host layout artifact and the tables still require signatures bound to the final images.

The 27.3 ESP builder can encode the provisional ESP start at LBA 124289792. The host fixture confirmed the FAT32 hidden-sector field and record agree, and rejected an ESP overlapping the supplied Data extent. The historical maintenance session rejects records targeting that provisional ESP. This layout still requires a writer whose preflight and readback use the approved live GPT geometry.

### 4 KiB userdata geometry rehearsal (2026-11-21)

UbuntuPiano successfully formatted and checked a sparse file at the provisional shortened userdata byte length through a loop device configured with 4096-byte logical sectors. See [the host result](docs/results-2026-11-21-f2fs-4k-host.md). This qualifies only the F2FS capacity/sector geometry. The retained 3.0.309 archive contains stock userdata and metadata inputs. Xiaomi recovery, metadata key setup, and Android boot on this geometry remain unqualified. Do not treat this rehearsal as permission to rewrite GPT/userdata or as proof of rollback.

### Partition-only qualification sequence

The live LUN 0 geometry was rechecked over root ADB. Its 510,165,778,432 bytes correspond to 124,552,192 logical blocks of 4 KiB. The current userdata span is 3,632,128–124,552,186. Metadata occupies 36,352–52,735 and keeps that extent throughout the trial. The active recovery partition matches the packaged 3.0.309 image byte for byte by SHA-256. See [the recovery slot readback](docs/results-2026-11-21-live-recovery-slots.md).

The following Recovery-menu trial requires an operator. On stock GPT, ABL entered Recovery, and `adb reboot system` returned to Recovery on the same tablet ([device result](docs/results-2026-11-21-recovery-explicit-system.md)). A separate [stock-layout trial](docs/results-2026-11-22-unattended-recovery-exit.md) verified host-driven exit through ABL and RAM maintenance. That trial verified and cleared the exact Recovery request in the first misc sector before booting HyperOS on A. The shortened-layout trial must then verify encrypted storage initialization and capacity without a menu action. Qualify host access to maintenance for restoring both stock GPT copies if that trial fails.

1. Before scheduling a write, qualify the maintenance writer and independent readback for both GPT copies. Match their live bytes against the retained stock captures, verify both CRCs, and verify the proposed entry array against SHA-256 `ba67d6aa7863835bb019231cfe97054138f649e58e7e06d08abe858da3f9751e`. Keep the separate verified stock copies and the A-slot boot triplet accessible from the host. Require snapshot state `none` and successful, bootable slot A. Confirm the stock Recovery entry and return path before changing geometry.
2. After owner approval, apply only the candidate LUN 0 GPT boundaries in the maintenance environment with userdata and metadata unmounted. Write, flush and verify the backup copy before writing, flushing and verifying the primary. Validate both copies together and stop on any mismatch. Keep the macOS extents empty during this trial. A partial GPT write requires both stock boundaries to be restored and verified before another boot attempt. If maintenance access is lost, resume from the verified maintenance entry and read both copies before selecting the repair action.
3. Exit maintenance with `piano-maint fastboot`, confirm the same tablet is in ABL, and run `fastboot reboot recovery` before Android can mount the old filesystem through the shortened partition. This direct entry was verified on the stock layout in [the ABL Recovery result](docs/results-2026-11-21-abl-recovery-entry.md). Arrange for the owner to operate the stock Recovery menu during the trial. Reinitialize userdata through the stock wipe flow and record its outcome, including metadata handling. Boot HyperOS 3.0.309 on A, verify F2FS mounts for `/data` and `/metadata`, file encryption, user unlock, and a usable capacity consistent with the shortened geometry. Repeat after a cold boot and verify a test file survives. Qualify this stage before introducing the root, Data or ESP images.
4. If recovery or the first Android boot fails, return to maintenance and restore both stock GPT copies using the same write, flush and readback order. Reinitialize Android storage in stock Recovery before booting HyperOS. Compare the resulting mount capacity, encryption and slot state with the stock baseline. Restoring GPT alone cannot undo a wipe. If stock reinitialization fails, stop and collect the recovery error before considering further writes. The broad factory flash script is outside this qualification trial.

The tablet trial confirmed candidate GPT readback, Recovery entry, and HyperOS startup and file persistence on the shortened layout. The Recovery wipe outcome was not directly observed. Stock GPT rollback and Android reinitialization after rollback still require hardware verification.

Live root ADB readback matched the retained LUN 0 first and last 1 MiB captures on both E: and F: under `piano-gpt-boundaries-20261119-075429`. SHA-256 values were `0adfb85a53f909df3e77d6b4a9c2e49f7f444163a479be3f3784292945d2e51f` (head) and `dc0eb2ce8fa2c40e8752d5d7dba186ab76152540779920f3815193eb1cae418f` (tail). The separately measured GPT write spans (24 KiB head and 20 KiB tail) had SHA-256 `9c096761ee7b9c12c1a5c3ef3f100bc27abccbbafb798a141aa4320eb81af25d` and `f8b53b4538913ccf2ac385d8bd87eb8ad689aa3f439222480a95d05c625ca4fe`. The readback used Android ADB. Maintenance writer qualification and recovery entry remain open.

The verified GPT-only recovery fragments are materialized under ignored `out/rollback-3.0.309-gpt/`: `stock-gpt-head.bin` is 24,576 bytes with SHA-256 `9c096761ee7b9c12c1a5c3ef3f100bc27abccbbafb798a141aa4320eb81af25d`; `stock-gpt-tail.bin` is 20,480 bytes with SHA-256 `f8b53b4538913ccf2ac385d8bd87eb8ad689aa3f439222480a95d05c625ca4fe`. Both are extracted from the E:/F: boundary captures and independently matched the current device bytes. They contain GPT boundary sectors only, not a tablet image or user data. They are write inputs for the corresponding 4 KiB LUN 0 ranges, and only after explicit layout approval and qualification of the maintenance writer. Their presence does not qualify a restore attempt or recover userdata.

### Candidate GPT fragment host verification (2026-11-21)

Exact primary and backup candidate fragments have been generated from the verified captures and independently parsed from a fresh sparse host disk using 4 KiB sectors. See [the dated host result](docs/results-2026-11-21-candidate-gpt-fragments.md) for SHA-256 and parser output. This still does not qualify device selection or writer/rollback execution.

The latest read-only ABL baseline is recorded in [the ABL return result](docs/results-2026-11-21-abl-read-return.md). The A-slot values match the prior baseline. Slot B is successful=yes but unbootable=yes, retries=6; the test stayed on A. No temporary image boot was attempted.

Use `python3 tooling/persist/check-piano-309-gpt-pair.py PRIMARY BACKUP` on the saved 24 KiB and 20 KiB readbacks. The checker recognizes the frozen stock and candidate pairs and rejects mixed, unknown and incorrectly sized copies. Both retained pairs passed on the host. Pairing the stock primary with the candidate backup returned a failure, as expected after an interrupted backup-first update. The historical `maint-session.py gpt` uses different stock hashes and expects the ESP at LBA 71,527,424. The candidate ESP starts at LBA 124,289,792. A layout-specific writer and maintenance readback qualification remain pending.

The backup-first sequence in `tooling/persist/piano_309_gpt_write.py` passed a host fault injection using the frozen fragments. A truncated backup readback stopped before the primary write. A truncated primary readback failed the update, mixed inputs triggered no writes, and the normal path applied the candidate and restored the stock pair. The test used an in-memory backend. The maintenance backend still needs device selection, mount checks, flush and independent readback qualification.

The ARM64 BusyBox extracted from the frozen maintenance image passed a host QEMU rehearsal on a 510165778432-byte sparse file. For stock, candidate, then stock, `dd conv=notrunc,fsync` wrote the backup before the primary, and `dd iflag=direct` read each fragment back byte for byte. File size stayed constant. This verifies the packaged utility and command options on a host filesystem. UFS flush behavior, device selection and recovery remain device checks.

## First native root write on the verified candidate layout

The [live layout recheck](docs/results-2026-11-22-live-layout-recheck.md) verified both candidate GPT copies and return to HyperOS. The first MiB of the target read zero. Qualify the remaining range before the first write.

1. Bind RAM maintenance to the tablet seen in stock ABL. Verify both frozen candidate GPT hashes, 4096-byte sectors, 510165778432-byte LUN 0 and the unmounted `linux_rootfs` extent at inclusive LBAs 102269696 through 107512575. Resolve the partition node from that geometry in the same session.
2. Verify that the stock root is 17547919360 bytes and SHA-256 `db3629e5da012ce76e64c0f42739ada9541ed8cca5adaab32256fc9b64b34ef7`. Use its [verified companion KC](docs/results-2026-11-22-stock-kc-root-pair.md). Read the entire target partition before a first write and stop for identification if any range is nonzero.
3. Write partition blocks 0 through 4284159 inclusive and flush. Hash the full image range directly from the medium, compare the remaining partition tail against its preflight hash, and recheck both GPT hashes. Record the checks alongside the write result.
4. Record the blank preflight, source hash and extent before starting. After an interrupted transfer, reenter maintenance, confirm the previous writer has stopped, revalidate identity and geometry, and read back the target. A matching image hash qualifies the written range. A known partial write with a matching run record can be retried with the same image from block zero. Stop on unexpected contents or geometry. The historical `rootfs-clear` prerequisite belongs to a different layout.
5. Rehearse target mismatch, short transfer and readback mismatch on the host before running the writer on hardware. Return via ABL and verify the active slot and HyperOS boot completion. Data, ESP and boot image installation are later phases.

## v2 install sequence on the installed candidate layout (2026-11-24)

The candidate GPT is already on the tablet and read back (2026-11-21): userdata ends at LBA 102,269,695; macOS root 102,269,696-107,512,575 (20 GiB); Data 107,512,576-124,289,791 (64 GiB); ESP 124,289,792-124,551,935 (1 GiB), all 4096-byte sectors. The live names must be read back with `maint-session.py map` before any write; the table above is the offline record, not a substitute.

Verified inputs now available: `out/inputs/k273/kc/kc273-mac17j.bin` (124,518,400 B, SHA-256 `d1003b5143ffd88439b4d45b6017f587171cfe6855660bed05370a2ea5db9fe9`, the macOS 27.3 `26C5035r` KC base pin); `out/piano-stock-root-26C5035r.img` (17,547,919,360 B, fits the 20 GiB root window); `out/inputs/k273/first-install/scratch-data.img` (22,200,480 B, empty Data seed); `out/paclr53.adt` (15,736 B) with its collection `out/no-demura-control/paclr53.kc`; the Apple root/SystemOS AEA and decrypted DMGs on `D:\Download\piano-macos-273-inputs` (each SHA-256 verified against its IPSW-member JSON).

Still missing before the first device write, and therefore the current gate:

1. A FAT32 ESP image for the ESP window above: **offline build done** on 2026-11-24 (`esp-device.img`, SHA-256 `68c28df15bcf55a589441df37e9c5fd94614e2771c437e955f190e3eb0aab898`, built twice byte-identically). Its boot arguments bind root to `linux_rootfs` and the Data store to `linux_data` LBA 117,735,999. This is a host artifact only; it has not been written to or booted from the tablet.
2. A version-2 trust bundle for that ESP. `tooling/k273/make-trust.py` expects a 27.0.1 base bundle (`out/inputs/kernel.macho.trustcache`) that is not present; the three 27.3 IPSW modules are here as IM4P files (`141-22106-021.dmg.trustcache`, `141-22145-021.dmg.aea.trustcache`, `141-22233-021.dmg.aea.trustcache`).
3. The root image staged and byte-checked against the tablet's copy, and the Data seed written to its window, both with SHA-256 readback.

### ESP build record (2026-11-24, offline)

`tooling/persist/build-esp.py` now takes `--esp-first-lba`, `--rootfs-last` and `--data-extent FIRST LAST`. The extent derives the store binding instead of the historical `linux_rootfs` window: the macos entries get `native-ufs-root-part=linux_rootfs data-store-part=linux_data data-store-lba=117735999`, and an overlap between the ESP and the extent, or a store that does not fit with its 1 GiB reserve, is refused. `tooling/persist/test-esp-geometry.py` covers the device LBA, the `dd seek=485507`, both overlap directions and the binding arithmetic.

Built on the Mac with GNU mtools 4.0.50 and Python 3.9:

```sh
cd ~/Codex/k273-esp/repo
env PYTHONPATH=$HOME/Codex/k273-esp/stub PATH=$HOME/Codex/mtools/install/bin:$PATH   PIANO_DATA_STORE_RELAYOUT=1 python3 tooling/persist/build-esp.py --profile daily --from .   --kc ../inputs/piano-ufs264.kc --adt ../inputs/paclr53.adt   --trust ../inputs/kernel.macho.trustcache.3mod --seed ../inputs/scratch-data.img   --firmware-dir ../inputs/firmware --esp-first-lba 124289792 --rootfs-last 107512575   --data-extent 107512576 124289791 --utc 1795478400 --name esp-device --out ../out --default macos
```

| Item | Value |
|---|---|
| image | `~/Codex/k273-esp/out/esp-device.img` |
| bytes | 1,073,741,824 |
| SHA-256 | `68c28df15bcf55a589441df37e9c5fd94614e2771c437e955f190e3eb0aab898` |
| FAT | 4096-byte sectors, 1 per cluster, 262,144 total, hidden sectors 124,289,792 |
| write window | LUN 0, partition 37 `esp`, LBA 124,289,792-124,551,935; `bs=1M seek=485507 count=1024` |
| Data binding | `data-store-part=linux_data data-store-lba=117735999` (6,291,649 blocks, 1 GiB reserve at the tail) |
| record | `~/Codex/k273-esp/out/esp-device.json`, `status: OFFLINE_BUILD_NOT_DEVICE_TESTED` |

The ESP is still only one of the three write inputs. The trust bundle, the staged root image and the boot triplet remain separate gates; no write is authorized by this build.

Execution order once ADB is authorized, all with the tablet cabled and no manual key presses:

1. `adb reboot bootloader`; record `getvar current-slot`, `slot-successful:a`, `slot-retry-count:a`, `snapshot-update-status`, and the partition size of `boot_a`.
2. `fastboot boot <maintenance RAM image>`; `maint-session.py map` (read-only) and compare every name, first/last LBA and size with the table.
3. Write the ESP image into its window, drop caches, read the region back and match its SHA-256.
4. Write the root image window and the Data seed window the same way, each preceded by a region readback of what was there and followed by a SHA-256 readback.
5. RAM-boot the matching image with `data-persist=0` first (no store writes), confirm desktop, then the persistent profile.
6. Three cold starts, two Android->macOS->Android cycles with the slot success bit and retry counters recorded before and after each exchange, a Data write followed by a restart readback, a normal restart, and a hard-reset recovery.

Rollback: restore the captured stock GPT from `out/rollback-3.0.309-gpt`; slot A's HyperOS triplet is never written by these steps; userdata is already empty, so the shortened geometry needs only a first-boot format by Android.
## Slot A boot triplet write (2026-11-25, replacing the "wait for signed v5" gate)

The signed v5 image and the signing host key are not available, and the 2026-11-13 v4 evidence shows this unlocked ABL boots
a boot_a whose AVB chain does not match. The write therefore uses a locally rebuilt, unsigned v4 container and the Ubuntu
v1.1.0 companion partitions. Scope: **slot A only**, `boot_a` + `vendor_boot_a` + `init_boot_a`; no GPT, no userdata, no
slot B, no vbmeta/dtbo, no `set_active`.

Verified inputs on this host:

| Partition | New bytes | SHA-256 | Size |
| --- | --- | --- | --- |
| `boot_a` | `out/persistent/boot-piano-macos-persistent-v4.img` | `b267c758d0659c9975d71bd06549d4800ee98ad2390c8b8092c4648b075077bd` | 3,301,376 (zero-padded to 96 MiB) |
| `vendor_boot_a` | Ubuntu v1.1.0 `boot/vendor_boot.img` | `7cbb06b9eddb8051c201d7a1f672620a5440283f0d5edbf0ace9270c05a60174` | 100,663,296 |
| `init_boot_a` | Ubuntu v1.1.0 `boot/init_boot.img` | `96057eb263fbe09284ba8bbe888128dbc0b36525dec4b01da918dcb7c6fe10ef` | 8,388,608 |

Current-region backups, taken 2026-11-25 and hashed on this host (`out/backup-20261125/`):

| Partition | Backup SHA-256 | Size |
| --- | --- | --- |
| `boot_a` | `7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2` | 100,663,296 |
| `vendor_boot_a` | `7f766f1d728c39d779a3fb7cf0ac1310dac4306cd3b37a98a0143dd4fca5bd23` | 100,663,296 |
| `init_boot_a` | `56197797f2ec99870a4e42b7a24bbf88407e4409615aba61f401182f13ea5cdc` | 8,388,608 |
| `boot_b` (reference, untouched) | `ec345e825416210bacab262c2275fbfb74cd482a85756003477c0b1865861fa3` | 100,663,296 |

Steps, all in the maintenance RAM image over the NCM link (192.168.88.2:2323), with no key presses:

1. `maint-shell.py` `cat /proc/partitions` + the GPT map; confirm the live names of `boot_a`, `vendor_boot_a`, `init_boot_a`
   and that their sizes equal the table above. Abort on any difference.
2. Re-read each partition's first and last 1 MiB and compare with the backup hashes; abort if the region changed since the
   backup.
3. Push each new file into `/tmp` with the reverse `nc` channel, then hash it **on the tablet** and compare with the table.
4. `dd if=/tmp/<file> of=/dev/<part> bs=1M conv=fsync` (boot_a buffer zero-filled to 96 MiB), drop caches.
5. Read every partition back on the tablet and on the host, compare SHA-256 with the table; abort and restore the backup if
   any byte differs.
6. Read slot metadata before and after: `slot-successful:a`, `slot-retry-count:a`, `snapshot-update-status`. The write does
   not change them; record the values unchanged.
7. `restart2 bootloader`, then boot slot A normally. First cold start is the acceptance run; the ESP's `macos-diag` entry
   keeps `piano-reset-after=240` so a stalled kernel returns to the bootloader unattended.

Rollback: push the three backups back with the same dd/readback procedure. All three are on this host and hash-verified, so
the tablet never depends on a single copy of the stock partitions. `userdata` is empty (factory reset by the owner) and is
not written by this plan.

