# Stock Recovery entry on piano (2026-11-21)

From completed HyperOS `OS3.0.309.0.WPYCNXM` on slot A, `adb reboot recovery` reached the `recovery` ADB state on the same build. Recovery mounted `/cache` read-write. No wipe was requested and `/data` did not appear in the mount inventory.

A plain `adb reboot` and a reboot through ABL both returned to Recovery. The shell lacks permission to read `/tmp/recovery.log`, and the production adbd refuses root. The device remained in Recovery at the end of this check. Select Reboot to System in the device menu, then verify completed HyperOS on slot A over ADB. Return from Recovery and shortened userdata initialization remain open.
