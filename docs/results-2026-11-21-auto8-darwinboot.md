# Automatic DarwinBoot probe on piano — 2026-11-21

The candidate replaces only the same-size FD in the timed Probe 2 image and recomputes the Android header ID. The retained firmware volume inventory contains DarwinBoot GUID `1B64AF87-8415-4B57-9B16-680D70913B9F`. The payload, shim and DTB match the previous probe. The offline reference verifier reported zero failures.

Stock ABL reported slot A successful and bootable, six retries, and no snapshot. It accepted `fastboot boot`. Subsequent USB checks through 13:32:25 found neither ADB nor Fastboot. No screen or firmware log was obtained. USB loss does not identify the boot stage, and the 90-second reset was not observed. Capture the current panel before a physical reset, which can erase the mailbox. Return to HyperOS and the post-run slot state remain pending.

A follow-up comparison of the two generated FVMAIN volumes found the same 174 file GUIDs, including DarwinBoot in both. Thirty-seven file payloads differ byte for byte, including DarwinBoot and the UniXNU menu. Some differences may be build metadata. The earlier runtime message establishes that the loader protocol was unavailable when requested, despite the driver appearing in the firmware inventory. The whole-FD swap therefore does not isolate DarwinBoot dispatch.
