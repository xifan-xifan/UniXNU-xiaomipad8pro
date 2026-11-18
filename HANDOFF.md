# Native macOS Deployment Handoff

Updated 2026-11-19. Goal status is active. The current objective remains deployment and local qualification of native macOS 27.3 while preserving HyperOS 3.0.309 and validating both switch directions and recovery.

## Current state

- **Upstream:** latest `main` equals requested baseline `2438556c77c90c22c7b76e57eb8b40c44b35f1af`; no newer commits to mix in. The complete source pins are unchanged.
- **Tablet:** read-only ADB audit confirms `piano`, `SM8750P`, `OS3.0.309.0.WPYCNXM`, Android 16, unlocked bootloader, root available, active slot A. No device writes or reboot occurred.
- **Layout:** all six UFS LUN primary/backup GPT CRCs pass and their entry arrays match. Stock `userdata` fills LUN 0 through its last usable LBA; native `linux_rootfs`, `linux_data`, and ESP are absent. The A-slot boot triplet and `misc` have length-checked, SHA-256-matched copies on E: and F:, with fresh device rereads matching. Read-only Fastboot reported A current, both slots successful, retries 6/6, and partition sizes matching the captures. Normal reboot returned to HyperOS Android 16 build `BP2A.250605.031.A3`, slot A. This does not qualify full recovery.
- **Recovery:** one historical external backup exists and is preserved. Its separate DCIM and Pictures archives parse (5519 and 571 entries), but are not file-by-file compared with the tablet. Several aggregate archive checks failed; it is not accepted as a verified 3.0.309 recovery set. Do not remove it or treat it as the only recovery copy.
- **Historical deployment evidence:** preserved checkout `piano-macos-c146` at `c146dfc6` reports native 27.3 desktop and persistent UFS/Data success on 2026-11-13/14. Current device GPT has no `linux_rootfs`, `linux_data`, or ESP, so this is historical, not current qualification; how the device returned to stock Android is unknown. Its tracked source edits and outputs are not adopted. The `macos27` files in `tmp.tar` are explicitly launched with `crosvm --hypervisor gunyah` and are DroidVM guest assets, not native boot inputs.
- **Build:** exact k27 source tree, Apple `26C5035r` inputs, matched native outputs, and local signing profile are not present in the current build tree. The configured Mac has no source checkout. CLT clang 17 is available through `DEVELOPER_DIR`, but Python is 3.9.6 (the repo requires 3.11+), and NASM/IASL are missing. No build has started.
- **Switching:** the Ubuntu upstream `tools/piano-switch.py` source has been located and reviewed. It writes the A-slot triplet and has rollback-on-error, but does not inspect boot success/retries or read back images; local bootloader fit and the release-tree copy are unverified.
- **Validation:** no macOS image has been built or booted in this task. No desktop, GPU, input, network, audio, Data persistence, or cycle result is claimed.

## Milestones

1. **Read-only baseline and source lock:** device identity and upstream commit confirmed; all six GPTs validated; A-slot boot triplet and misc backed up twice and reread-verified; Fastboot slot state/capacities read and normal Android return confirmed. Full recovery qualification remains open.
2. **Reproducibility and migration reports:** first versions created in this change; missing inputs and stock-to-upstream layout differences recorded.
3. **Complete rollback set and write design:** blocked on slot metadata, a verified 3.0.309 package, independent validated backup copies, and a concrete storage plan. All six GPTs have been read and checked.
4. **Matched build:** blocked on local k27 tree/generated state, Apple inputs, firmware/calibration, userland/signing pair, and Python 3.11+/NASM/IASL on the Mac.
5. **Diagnostic boot and desktop qualification:** not started; storage root dependency must be resolved first.
6. **Persistence, switching, recovery and daily-use acceptance:** not started; requires an approved concrete write plan and paired image sets.

## Next actions

- Keep the A/B successful/retry values as a timestamped Fastboot observation; recheck only before any future switching plan because bootloader state can change.
- Inventory existing photo and recovery copies, validate archive format and hashes without changing the source copies, and locate the authoritative independent backup.
- Find the exact c146 native image set and its k27/input provenance in preserved copies, if present; keep DroidVM assets separate. Determine the documented return-to-Android event without assuming the paused 3.0.308 wipe proposal ran.
- Locate the exact public/author Ubuntu release tree and compare its `piano-switch.py` with the upstream source; add host tests for success-bit/retry handling before any device use.
- Search upstream history/forks and existing local assets for the pinned k27 tree and the exact 27.3 Apple inputs before downloading anything.
- Request user physical interaction only if access to the Mac developer-tools installer or tablet bootloader is required; continue independent source and recovery research meanwhile.

See [DEVICE-BASELINE.md](DEVICE-BASELINE.md), [REPRODUCIBILITY.md](REPRODUCIBILITY.md), and [MIGRATION-DIFF.md](MIGRATION-DIFF.md). The dated read-only device record is [docs/results-2026-11-19.md](docs/results-2026-11-19.md); the machine-readable source lock is [outputs/manifests/native-macos-27.3.json](outputs/manifests/native-macos-27.3.json).
