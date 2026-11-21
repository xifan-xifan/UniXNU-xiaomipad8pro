# HyperOS on the candidate GPT (2026-11-21)

ADB reported OS3.0.309.0.WPYCNXM on slot A with `sys.boot_completed=1`. `/metadata` mounted as F2FS on `sda20` and `/data` mounted as F2FS on `dm-56` with `inlinecrypt`. Android reported file encryption, user 0 `RUNNING_UNLOCKED`, and credential-encrypted storage available. `df -k /data` reported 394548224 KiB total, consistent with the shortened userdata extent.

A synced marker in `/data/local/tmp/piano-layout-20261121-check` matched SHA-256 `306a063b9b6d71bb48419efc7cf512c95c789d493a73d178e481a57dc82575f3` before and after `reboot -p`. The kernel boot ID and ADB transport changed. On return, HyperOS completed boot on slot A with both filesystems mounted. The power-off interval and Recovery wipe result were not directly observed. This is Android persistence evidence. Cold boot counting, macOS Data persistence and stock GPT rollback still require separate evidence.
