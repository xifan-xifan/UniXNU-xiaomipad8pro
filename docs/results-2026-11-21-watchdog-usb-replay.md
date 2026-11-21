# Watchdog recovery with USB observation

Stock ABL accepted the previously hashed watchdog expiry RAM image. Windows sampled `piano-uefi` at 09:39:16.749 UTC and first sampled its absence at 09:39:45.939 UTC. Neither stock Fastboot nor ADB was sampled between that loss and the observer ending at 09:40:14.845 UTC. ADB probes timed out around the USB changes, so the sampling record cannot exclude a brief intermediate interface.

A later query at 09:40:50 UTC confirmed HyperOS `OS3.0.309.0.WPYCNXM` on `_a`, boot complete, boot reason `bootloader`, and the original Android marker hash. Android uptime was 41.69 seconds. No host reset command followed the RAM boot. The trace is `out/usb-observer-expiry-173854326.log`, SHA-256 `b8df8ee103b68fa6a512db6b877400fe59f84a5f6c9b1bbcc35a847f0f6493a5`. Use at least 120 seconds for future captures so Android re-enumeration can be observed.

This repeats unattended return from the firmware diagnostic. A successful ExitBootServices call followed by a deliberate stall is still required to qualify recovery at that boundary. The retained DarwinBoot source exposes the exact injection point on the successful return, just before `DarwinEnter`. A diagnostic there must record a device-visible checkpoint after the call and before stalling so an earlier loader reset cannot satisfy the test.
