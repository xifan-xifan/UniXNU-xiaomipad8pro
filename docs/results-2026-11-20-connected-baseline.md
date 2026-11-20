# Connected HyperOS baseline on 2026-11-20

Windows ADB using the retained platform-tools installation detected the Xiaomi Pad 8 Pro. Read-only Android shell queries confirmed product `piano`, build `OS3.0.309.0.WPYCNXM`, slot `_a`, boot completed, and `ro.boot.flash.locked=0`. Android reported Virtual A/B enabled. `/data` was mounted as F2FS with `inlinecrypt` through `dm-56`. `userdata` and `metadata` resolved to `sda34` and `sda20` respectively.

Android shell permissions prevented a fresh read of the raw partition geometry through sysfs and `/proc/partitions`. The geometry for the write plan still comes from the verified GPT captures. ADB properties do not provide fresh ABL slot success, retry or snapshot status. The device remained in Android during these checks.

The retained menu-only FD and Android boot container both matched the SHA-256 entries in the deployment manifest. The stock Xiaomi ABL write path is still unresolved. See [Stock ABL inspection](results-2026-11-20-stock-abl.md).
