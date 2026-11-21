# Android re-enumeration and ABL readback, 2026-11-21

ADB serial `7ccdd855` reappeared. Read-only Android checks reported product `piano`, `OS3.0.309.0.WPYCNXM`, `sys.boot_completed=1`, slot `_a`, boot reason `bootloader`, uptime 71.84 seconds, and the original marker SHA-256 `306a063b9b6d71bb48419efc7cf512c95c789d493a73d178e481a57dc82575f3`.

ADB rebooted to stock ABL Fastboot for read-only verification. ABL reported product `piano`, `is-userspace=no`, `snapshot-update-status=none`, current slot A, slot A successful=yes, unbootable=no, retries=6. A normal Fastboot reboot returned to completed HyperOS 3.0.309 on A. The marker hash remained unchanged. No Android, GPT, boot-partition or userdata writes were made.

This closes the current device-reachability and ABL state uncertainty. It does not establish why the previous RAM boot returned, and does not count as a deployment cycle. Next, run the frozen watchdog-status RAM image, capture the complete UEFI log before it leaves USB, and compare the post-run ABL values before accepting the trial.
