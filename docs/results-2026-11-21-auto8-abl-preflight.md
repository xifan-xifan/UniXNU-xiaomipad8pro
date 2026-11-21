# Automatic SMP probe preflight — 2026-11-21

The local automatic SMP probe matched the manifest SHA-256. Stock Fastboot reported `product=piano`, `is-userspace=no`, `snapshot-update-status=none`, `current-slot=a`, `slot-successful:a=yes`, `slot-unbootable:a=no`, and `slot-retry-count:a=6`. A normal Fastboot reboot returned to HyperOS `OS3.0.309.0.WPYCNXM` on `_a` with `sys.boot_completed=1`. The Android test file still matched SHA-256 `306a063b9b6d71bb48419efc7cf512c95c789d493a73d178e481a57dc82575f3`.

In the reviewed NativeUfs source, probe 2 returns before allocating I/O scratch memory or starting root initialization. NativeRamBlock mode 0 skips opening the persistent store. These checks cover those driver paths. The exact Xiaomi ABL variable and verified boot state write paths remain unresolved. The image also lacks the timed XNU reset, so a stall after firmware handoff has no demonstrated automatic recovery. The earlier hardware reset recovered HyperOS from a diagnostic attempt before any confirmed XNU execution. Capture the screen before any hard power-off because it clears the RAM mailbox.

The automatic probe was not loaded in this preflight.
