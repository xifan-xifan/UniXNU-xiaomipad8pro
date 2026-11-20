# ADB baseline (2026-11-20)

The connected tablet responded to the local platform-tools ADB client. Read-only properties were: `ro.product.device=piano`, `ro.build.version.incremental=OS3.0.309.0.WPYCNXM`, `ro.boot.slot_suffix=_a`, and `sys.boot_completed=1`. `/data` is mounted from `/dev/block/dm-56` as F2FS with inline encryption. The stock Android shell does not provide `bootctl`; slot success and retry values therefore remain a Fastboot-only observation. No image was downloaded, booted, or written in this check.
