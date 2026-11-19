# Stock HyperOS to Native Layout: Migration Difference

Audit date: 2026-11-19. This is a read-only comparison, not a partitioning or flashing plan.

| Area | Tablet now | Upstream persistent-install layout | Consequence |
|---|---|---|---|
| Android base | `OS3.0.309.0.WPYCNXM`, Android 16, active slot A | The maintainer's native system evolved from an existing Ubuntu/Linux layout | The documented procedure is not a stock-Android first-install recipe. |
| Root | Stock Android dynamic partitions under `super`; no `linux_rootfs` name | macOS root image on `linux_rootfs` | A desktop boot cannot assume the required UFS root already exists. |
| Data | `/data` on `userdata` (`sda34`), 461 GiB filesystem with 439 GiB free at audit time | Native Data store on `linux_data` with a seed/LBA binding | Existing Android data cannot be treated as spare space; F2FS shrinkability and preservation are unverified. |
| ESP | No named ESP partition | UniXNU payload in an ESP (the author's layout uses a dedicated GPT entry) | ESP creation and exact location require an explicit layout and restore design. |
| Boot set | Android `boot_a/b`, `vendor_boot_a/b`, `init_boot_a/b` exist; active suffix is A | Computer-side switching exchanges the active A-slot triplet between Android and macOS image sets | Read-only Fastboot on 2026-11-19 reported `product=piano`, `unlocked=yes`, `current-slot=a`, `is-userspace=no`; both slots `successful=yes`, retry counts 6; A-slot sizes were boot 96 MiB, vendor_boot 96 MiB and init_boot 8 MiB. Sizes match verified A-slot captures. |
| GPT / LUNs | Six UFS LUNs, all 4096-byte logical blocks; primary/backup GPT header and entry-array CRCs pass and tables match. LUN 0 has no unallocated tail after `userdata`. | The author's procedure assumes a particular pre-existing GPT and root/Data regions | Creating a native layout requires changing the userdata boundary or another concrete storage migration; verify all ranges and recovery before proposing writes. Do not transplant author LBAs. |

The verified LUN 0 GPT places `super` at LBA 224256-3632127 and `userdata` at LBA 3632128-124552186, using 4096-byte logical blocks. `userdata` reaches the last usable LBA, so there is no free tail range to repurpose. No `linux_rootfs`, `linux_data`, or ESP is present. The precise final Android capacity and macOS allocation have not been chosen.

## Switching tool source and fit

The latest Ubuntu mainline release observed on 2026-11-19 is v1.6.1 (published 2026-11-18). Its `tools/piano-switch.py` is 3111 bytes, Git blob `c504b603ba1cb3629057afeb501afe5740819766`, last touched in commit `ec551c584c8080cfbaf20797a0ee00e8978e6538`. The 35,336,074-byte `piano-ubuntu-v1.6.1-tools.zip` was downloaded to `D:\Temp` after confirming no local copy; its SHA-256 matches the release API digest, and the bundled script's Git blob matches the public tag. The archive itself is retained outside the repository. The release targets stock `OS3.0.303.0.WPYCNXM`; its installation erases Android data, so its installer and Ubuntu boot-image set are not applicable to the required preserved 3.0.309 installation.

The switching source verifies SHA-256 manifests for both image sets, checks Fastboot product/unlocked/A-slot/non-userspace state, and checks each image size against the bootloader-reported partition size. The tablet's Fastboot replies to all those queried fields matched (`piano`, unlocked, slot A, non-userspace, and the three captured image sizes). It writes `init_boot_a`, then `vendor_boot_a`, then `boot_a`; on a write error it attempts to restore the checked rollback triplet. It leaves the device in Fastboot for a RAM readback and does not reboot automatically. It does not check successful-slot flags or retry counters and does not implement readback. The tablet's current success/retry values were checked separately and are recorded in the manifest. This establishes preflight-field compatibility only; no image exchange or switching operation was attempted.

The stock OS recovery package is now locally verified on `mac-lan`: `/tmp/piano_images_OS3.0.309.0.WPYCNXM_20260906.0000.00_16.0_cn_d5071e52da.tgz`, 11,346,114,406 bytes, MD5 `d5071e52da7c5dff4479d45e2775c437`, SHA-256 `c7b0446a4704611d1d562e3b27cdd01fdd14bdee7b979973b76992c2200836e`; all 110 tar entries listed successfully. This is a stock OS recovery package, not a backup of userdata. No archive contents were extracted and no tablet write was performed.

The script has no dry-run mode, no boot-success/retry-counter check, and no readback implementation. Its fit to a matched macOS 27.3 image set and a verified 3.0.309 boot rollback set remains **unverified**. The official stock OS package is verified, but it does not preserve userdata. The script must not be run against the device until image sets are matched, boot rollback inputs and post-write readback are verified, the storage plan is resolved, and a concrete WRITE-PLAN is approved.

## Required design before writes

1. Read and validate every relevant LUN's primary and backup GPT; record native logical-sector units, GUIDs, boundaries and CRCs.
2. Validate a region-matched 3.0.309 recovery package plus verified backups of every boot component and any range proposed for change. Keep two copies on independent media and re-read/hash them.
3. Resolve how to retain Android userdata without an unverified online F2FS shrink. Allocate root, Data and ESP only after proving exact non-overlapping ranges and the resulting Android capacity.
4. Match root, Data seed/header/LBA, FD, ESP, ADT, trust cache, userland layer and boot signature as one manifest-backed set.
5. Produce a concrete `WRITE-PLAN.md` with target, range, hashes, commands, readback and rollback for each write. Any userdata clear/rebuild, GPT change or persistent boot write remains gated on explicit confirmation of that specific plan.

No WRITE-PLAN is issued here because the partition map, independent recovery copies and target sizes are not yet verified.
