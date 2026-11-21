# Gunyah watchdog expiry on piano

Stock ABL accepted the RAM diagnostic. The retained DEBUG capture at `out/uefi-watchdog-expiry-172953/debug.log` reports EL1, `STATUS=0`, `SET_TIME=0` and `ENABLE=0; no pet armed`. Capture SHA-256 is `429a761c234ae60f91630e18d879bbdDB8d7ce4d584d078578102b16f5843cf8`. The diagnostic requests a 30-second bark and 32-second bite. It contains no explicit reset or restart-reason write.

The next observation at 17:30:45 local host time found ADB only. The tablet booted HyperOS `OS3.0.309.0.WPYCNXM` on `_a`, reported `sys.boot_completed=1`, and returned the original test marker SHA-256 `306a063b9b6d71bb48419efc7cf512c95c789d493a73d178e481a57dc82575f3`. Both Android boot-reason properties reported `bootloader`. The host issued no reboot after capturing the diagnostic in this run, and no operator action was recorded.

This establishes an unattended Android return in the observed run. The capture and follow-up did not cover the intervening USB transition or identify the hardware reset source. Android uptime was 43.72 seconds at follow-up, placing Android kernel startup near 17:30:02. Timing from the enable call itself was not captured. The first interface after reset, ABL slot retry count, and watchdog ownership through `ExitBootServices` remain unverified.
