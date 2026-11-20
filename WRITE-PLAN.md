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

The diagnostic boot gate comes before any device write. Once the temporary firmware path is qualified, test only the diagnostic stages that can run with the stock layout. A desktop test requires the root storage expected by its matched image set. If that storage requires persistent preparation, finalize the affected ranges, pre-write evidence, readback hashes, and failure recovery before requesting approval. The provisional ranges above do not authorize any write.

After authorized storage preparation and successful desktop testing, qualify the paired boot images, ESP and Data binding before installing a persistent selector. Verify the A-slot success and retry state before and after the switch, then record three cold starts, two Android/macOS round trips, Data readback after reboot, normal restart, hard-reset recovery and the v2 daily-use checks.

## Rollback

- A failed preflight or readback aborts before reboot.
- A failed RAM diagnostic returns to ABL Fastboot and uses no persistent write.
- A failed persistent step restores the immediately preceding verified region in reverse dependency order, then reads it back before reboot.
- Boot-triplet rollback uses only the three verified device-captured A-slot images (`boot_a`, `init_boot_a`, `vendor_boot_a`); the offline verifier checks size and SHA-256. The official package has a different `init_boot` ramdisk and is not interchangeable.
- The official package `flash_all_except_storage.sh` (SHA-256 `180b79a08b7ffcf66df678c07e4afb3b66cd2c01a3a6bb5b7b855f5d6204de28`) erases `boot_ab`, flashes firmware and system partitions including `init_boot_ab`, `vendor_boot_ab`, `super`, `misc`, recovery and `boot_ab`, sets slot A, then reboots. Static review found no GPT, `userdata` or `metadata` write in this script. It is a broad stock-system recovery operation, not a boot-triplet rollback or GPT restoration procedure.
- The package `flash_all.sh` (SHA-256 `8610cf859ab4fdda40acaa81c1c25ef27b233d8622df716587e7c518360ccb10`) additionally erases and flashes `metadata` and flashes `userdata.img`. It destroys existing userdata and is excluded from the preservation plan. Neither vendor script has been run.
- Full stock-layout recovery remains a separate procedure: after altered partition boundaries or overwritten sectors, it may require restoring both GPT copies and reinitializing `/data` and `/metadata`; neither vendor script restores GPT. Qualify the exact commands, recovery entry/return path and readback before approval.
- The plan cannot be executed until the exact image set, free-space/layout proof, recovery set, and write-specific hashes are recorded in the manifest.

Before approving boot-triplet rollback, run tooling/persist/verify-android-rollback.py against the selected capture directory. Both retained capture directories passed. Substituting the official init_boot under the captured filename failed the hash check.
