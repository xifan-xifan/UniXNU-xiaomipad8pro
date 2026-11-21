# ABL state read and Android return (2026-11-21)

From HyperOS `OS3.0.309.0.WPYCNXM` on slot A, the tablet entered ordinary ABL Fastboot via `adb reboot bootloader`. Read-only Fastboot queries reported product `piano`, `is-userspace=no`, `snapshot-update-status=none`, current slot A, slot A successful=yes, unbootable=no, retries=6. Slot B reported successful=yes, unbootable=yes, retries=6. No image was loaded and no write command was issued. A normal `fastboot reboot` returned to Android; ADB then reported `sys.boot_completed=1`, slot `_a`, and the same 3.0.309 build.

This refreshes the pre-boot slot baseline and verifies normal ABL entry/return on this device. It does not exercise `fastboot boot`, the ABL downloaded-image handler, or rule out writes on that separate path.
