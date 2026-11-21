# Unattended return after ExitBootServices on piano

On November 21, 2026, stock ABL accepted the RAM diagnostic. The operator reported a green and magenta rectangle in the upper left. The diagnostic draws those colors only after `ExitBootServices` returns success, then deliberately stalls with the previously armed Gunyah watchdog. This run exercised firmware directly before DarwinBoot.

`out/usb-observer-post-ebs-175354462.log` sampled stock Fastboot at 09:54:05.456 UTC and ADB at 09:55:16.907 UTC. ADB was the first interface sampled after the stall. The diagnostic has no explicit reset call. The hardware reset source was not independently captured.

ADB readback reported `OS3.0.309.0.WPYCNXM`, slot `_a`, `sys.boot_completed=1`, and `ro.boot.bootreason=bootloader`. The Android persistence marker read back as `306a063b9b6d71bb48419efc7cf512c95c789d493a73d178e481a57dc82575f3`.

The next control belongs after successful ExitBootServices in DarwinBoot and before DarwinEnter. XNU watchdog takeover requires its own test.
