# Unattended stock Recovery exit

On stock GPT, Android ADB entered stock Recovery. `adb reboot bootloader` from Recovery reached ABL (`is-userspace=no`). ABL RAM-booted the verified maintenance image `932be704d5450fe7fbe722d074a96c2764862627862496ad7dd82fd82d1799e2` and USB NCM provided the maintenance shell.

Sysfs identified `sda12` as `misc`. Its first 4096 bytes contained `boot-recovery` at offset 0 and `recovery\n` at offset 64, with all other bytes zero (SHA-256 `8a7ad840906c3a1bfda429292e597ad7e66989a0bf92959fbbd22ef92c64c03a`). Maintenance checked the partition name and exact input hash, zeroed and flushed that sector, and verified its direct readback (SHA-256 `ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7`).

`piano-maint fastboot` disconnected the shell and returned to ABL. Slot A was successful and bootable, with six retries and no snapshot. `fastboot reboot` returned to HyperOS `OS3.0.309.0.WPYCNXM` on A with `sys.boot_completed=1`. The entire procedure used host commands. Shortened userdata initialization and GPT restoration remain unverified.
