# Android readback boundary (2026-11-21)

The stock HyperOS shell runs as `uid=2000(shell)` and can resolve `/dev/block/by-name/boot_a`, `init_boot_a`, and `vendor_boot_a`, but cannot read any of the three block devices. Direct full-partition hash verification therefore cannot run from ordinary Android ADB. The maintenance image remains the candidate transport for complete UFS readback; it has not been loaded.
