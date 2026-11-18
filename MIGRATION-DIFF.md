# Stock HyperOS to Native Layout: Migration Difference

Audit date: 2026-11-19. This is a read-only comparison, not a partitioning or flashing plan.

| Area | Tablet now | Upstream persistent-install layout | Consequence |
|---|---|---|---|
| Android base | `OS3.0.309.0.WPYCNXM`, Android 16, active slot A | The maintainer's native system evolved from an existing Ubuntu/Linux layout | The documented procedure is not a stock-Android first-install recipe. |
| Root | Stock Android dynamic partitions under `super`; no `linux_rootfs` name | macOS root image on `linux_rootfs` | A desktop boot cannot assume the required UFS root already exists. |
| Data | `/data` on `userdata` (`sda34`), 461 GiB filesystem with 439 GiB free at audit time | Native Data store on `linux_data` with a seed/LBA binding | Existing Android data cannot be treated as spare space; F2FS shrinkability and preservation are unverified. |
| ESP | No named ESP partition | UniXNU payload in an ESP (the author's layout uses a dedicated GPT entry) | ESP creation and exact location require an explicit layout and restore design. |
| Boot set | Android `boot_a/b`, `vendor_boot_a/b`, `init_boot_a/b` exist; active suffix is A | Computer-side switching exchanges the active A-slot triplet between Android and macOS image sets | Android and macOS sets must be captured, size-checked and recoverable before any switch. The current fastboot partition sizes and boot-control metadata are not yet read back from the bootloader. |
| GPT / LUNs | Six UFS LUNs, all 4096-byte logical blocks; primary/backup GPT header and entry-array CRCs pass and tables match. LUN 0 has no unallocated tail after `userdata`. | The author's procedure assumes a particular pre-existing GPT and root/Data regions | Creating a native layout requires changing the userdata boundary or another concrete storage migration; verify all ranges and recovery before proposing writes. Do not transplant author LBAs. |

The verified LUN 0 GPT places `super` at LBA 224256-3632127 and `userdata` at LBA 3632128-124552186, using 4096-byte logical blocks. `userdata` reaches the last usable LBA, so there is no free tail range to repurpose. No `linux_rootfs`, `linux_data`, or ESP is present. The precise final Android capacity and macOS allocation have not been chosen.

## Switching tool source and fit

The Ubuntu upstream repository `yzddmr6/xiaomipad-8pro-mainline` contains `tools/piano-switch.py`. The file's last-touch commit is `ec551c584c8080cfbaf20797a0ee00e8978e6538` (2026-10-06 21:28:16 UTC); its Git blob is `c504b603ba1cb3629057afeb501afe5740819766`. The source verifies SHA-256 manifests for both image sets, checks Fastboot product/unlocked/A-slot/non-userspace state, and checks each image size against the bootloader-reported partition size. It writes `init_boot_a`, then `vendor_boot_a`, then `boot_a`; on a write error it attempts to restore the checked rollback triplet. It leaves the device in Fastboot for a RAM readback and does not reboot automatically.

The script has no dry-run mode, no boot-success/retry-counter check, and no readback implementation. The documented maintenance flow separately handles `slot-success`, but that behavior is not implemented by this script. The exact copy bundled with the maintainer's Ubuntu release tree has not been compared against this upstream file. Its fit to this tablet's actual bootloader responses and current 3.0.309 Android-set is therefore **unverified**. It must not be run against the device until the three current Android images and an independently tested recovery set are verified and a WRITE-PLAN is approved.

## Required design before writes

1. Read and validate every relevant LUN's primary and backup GPT; record native logical-sector units, GUIDs, boundaries and CRCs.
2. Validate a region-matched 3.0.309 recovery package plus verified backups of every boot component and any range proposed for change. Keep two copies on independent media and re-read/hash them.
3. Resolve how to retain Android userdata without an unverified online F2FS shrink. Allocate root, Data and ESP only after proving exact non-overlapping ranges and the resulting Android capacity.
4. Match root, Data seed/header/LBA, FD, ESP, ADT, trust cache, userland layer and boot signature as one manifest-backed set.
5. Produce a concrete `WRITE-PLAN.md` with target, range, hashes, commands, readback and rollback for each write. Any userdata clear/rebuild, GPT change or persistent boot write remains gated on explicit confirmation of that specific plan.

No WRITE-PLAN is issued here because the partition map, independent recovery copies and target sizes are not yet verified.
