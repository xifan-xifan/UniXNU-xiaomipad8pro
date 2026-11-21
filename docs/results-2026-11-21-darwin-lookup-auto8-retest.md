# Automatic eight-core probe with prearmed capture

After the first probe, ADB verified HyperOS `OS3.0.309.0.WPYCNXM` on `_a`, `sys.boot_completed=1`, and Android marker SHA-256 `306a063b9b6d71bb48419efc7cf512c95c789d493a73d178e481a57dc82575f3`. The reset mechanism was not observed.

Before the second RAM boot, ABL again reported slot A successful and bootable with six retries and no snapshot update. The prearmed Windows capture reached `piano-uefi` and `oem log` returned OKAY in 90 ms. The capture script incorrectly treated the subprocess status as failure and skipped `get_staged`. The script now checks the reported OKAY and requires a nonempty output file after download. The manual retry missed the interface. Firmware log contents, XNU handoff, and the second Android return remain pending.
