# Live HyperOS userdata audit (2026-11-20)

Read-only ADB queries confirmed `piano` / `25091RP04C`, Android 16 `BP2A.250605.031.A3`, HyperOS `OS3.0.309.0.WPYCNXM`, active slot `_a`, and an unlocked bootloader. No user files were enumerated or read. No tablet backup, reboot or persistent write was performed.

`userdata` resolves to `/dev/block/sda34`. Linux sysfs reports start `29057024` and length `967360472` in 512-byte sectors. The disk reports 4096-byte logical blocks. Converting the sysfs values gives GPT LBAs `3632128–124552186` inclusive and `495288561664` bytes, exactly matching the existing GPT captures. The mounted `/data` device, `dm-56`, reports the same block-device byte length. F2FS `statfs` reported 120919547 blocks of 4096 bytes, with 115225716 available to non-root at the time of measurement. That availability is within the current filesystem.

Root could list `/sys/fs/f2fs/features` during this audit. The global list has no `resizable_tail` entry. The mounted `/data` instance exposes no `resizable_tail_secno` control and the mount options omit it. The mounted instance reports `encryption, extra_attr, projquota, quota_ino, verity, casefold, pin_file`. Its mount options include `inlinecrypt`. The public offline shrink test used a synthetic unencrypted filesystem and does not qualify this encrypted Android layout. Keep online shrink out of the proposed layout. A separate userdata reformat proposal would need evidence that stock recovery can initialize and mount the shortened partition, a verified rollback procedure and explicit user authorization.

The Windows SSH alias, the Mac mDNS name, and the WSL alias all failed name resolution. The Mac checkpoint was not accessed. The current build remains at the three-driver checkpoint.
