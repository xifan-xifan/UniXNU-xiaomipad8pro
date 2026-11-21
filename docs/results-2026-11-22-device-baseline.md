# Read-only device baseline (2026-11-22)

ADB listed Xiaomi Pad 8 Pro serial `7ccdd855` as product `piano`, model `25091RP04C`. Read-only properties reported `ro.build.version.incremental=OS3.0.309.0.WPYCNXM`, `ro.boot.slot_suffix=_a`, and `sys.boot_completed=1`.

The by-name map resolved `esp` to `/dev/block/sda37`, `linux_rootfs` to `/dev/block/sda35`, and `linux_data` to `/dev/block/sda36`. `bootctl get-current-slot` was unavailable. The ordinary ADB shell could not read `/sys/class/block/*/{start,size}` and `su` was inaccessible, so this session did not verify live GPT extents or ABL successful/bootable/retry flags.

No reboot, block-device read, or write was issued. This check confirms the active Android build and partition labels only. It does not qualify any candidate GPT, writer, boot selector, or macOS startup set.
