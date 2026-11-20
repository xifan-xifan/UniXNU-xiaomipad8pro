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

## Planned write order after approval

1. Enter ABL Fastboot and record product, serial, slot, success bit, retry count, userspace state, partition sizes, and current hashes.
2. Write only the explicitly approved target regions. Never write XBL, ABL, TZ, HYP, VBMeta, DTBO, persist, firmware/calibration, any B partition, or userdata without a separate approved plan.
3. Read back every written region byte-for-byte and compare against the intended hashes before rebooting.
4. Keep the device in Fastboot for a RAM diagnostic boot. Capture pstore, USB console, root, Data, mount, and userland write paths.
5. If diagnostic acceptance passes, perform the approved persistent layout transition with GPT primary and backup tables written as one validated pair. Read back both tables and run `sgdisk -v`.
6. Verify the macOS root/Data pair, seed binding, ESP selector, A-slot success bit, and retry count. Reboot only after all checks pass.
7. Test Android recovery using the verified 3.0.309 set, then read back the same state and verify slot success/retries. Repeat macOS to Android and Android to macOS twice.
8. Test normal restart, hard reset, file write followed by reboot, three cold starts, and required desktop, input, network, audio and video checks.

## Rollback

- A failed preflight or readback aborts before reboot.
- A failed RAM diagnostic returns to ABL Fastboot and uses no persistent write.
- A failed persistent step restores the immediately preceding verified region in reverse dependency order, then reads it back before reboot.
- Android recovery restores the verified A-slot boot triplet and approved GPT/root regions only. It does not erase userdata or use `flash_all.sh`.
- The plan cannot be executed until the exact image set, free-space/layout proof, recovery set, and write-specific hashes are recorded in the manifest.
