# Connected HyperOS baseline on 2026-11-20

Windows ADB using the retained platform-tools installation detected the Xiaomi Pad 8 Pro. Read-only Android shell queries confirmed product `piano`, build `OS3.0.309.0.WPYCNXM`, slot `_a`, boot completed, and `ro.boot.flash.locked=0`. Android reported Virtual A/B enabled. `/data` was mounted as F2FS with `inlinecrypt` through `dm-56`. `userdata` and `metadata` resolved to `sda34` and `sda20` respectively.

Android shell permissions prevented a fresh read of the raw partition geometry through sysfs and `/proc/partitions`. The geometry for the write plan still comes from the verified GPT captures. ADB properties do not provide fresh ABL slot success, retry or snapshot status. The device remained in Android during these checks.

The retained menu-only FD and Android boot container both matched the SHA-256 entries in the deployment manifest. The stock Xiaomi ABL write path is still unresolved. See [Stock ABL inspection](results-2026-11-20-stock-abl.md).


With the device's rooted `su` shell, read-only checks also returned a 4 KiB logical block size and `sda34` start `29057024`, size `967360472` in Linux 512-byte sector units. Converted to 4 KiB GPT LBAs, the inclusive range is `3632128–124552186`. These values match the captured stock userdata placement. Read-only SHA-256 values for `boot_a`, `vendor_boot_a` and `init_boot_a` were respectively `7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2`, `7f766f1d728c39d779a3fb7cf0ac1310dac4306cd3b37a98a0143dd4fca5bd23` and `56197797f2ec99870a4e42b7a24bbf88407e4409615aba61f401182f13ea5cdc`; all match the retained capture manifest. No block write command was issued.


The offline rollback verifier was rerun against both retained capture directories (`E:/piano-verified-readonly-20261119-073450` and `F:/piano-verified-readonly-20261119-073450`). Both verified all three A-slot images by exact length and SHA-256. This remains rollback preparation only; no image was written back to the tablet.


A root-only `snapshotctl dump` returned `Update state: none`, current slot `_a`, no merge or rollback indicator, and no userspace snapshot service active. This is a read-only snapshot preflight and does not authorize GPT or userdata changes.
