# Generated FAT16 menu replay (2026-11-21)

The full `--diagnostic-menu` build used the retained tracing FD, the boot DTB and the locked AOSP mkbootimg. Verification against the Ubuntu maintenance container passed with zero failures. The 13,766,656-byte output SHA-256 is `708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468`. The build record and verification log are in UbuntuPiano under `/tmp/piano-fat16-replay.3euyTK/`.

On the tablet, the generated container reported RAM sector CRC32 `75E9D342`, signature `55AA`, successful Simple File System and TOML reads, and a two-entry menu. The firmware reboot returned to HyperOS `OS3.0.309.0.WPYCNXM` with `sys.boot_completed=1` and slot `_a`. Before and after, ABL reported `is-userspace=no`, slot A successful and bootable, retry count 6 and snapshot status `none`. This run used only temporary boot and reboot commands. The firmware ring log SHA-256 is `26ed3c6b40527a8e7d224ebf13cb5179fbee60c96d9d68910507488f4160df21`.

The generated diagnostic menu has no XNU entry. Native desktop testing still requires the matching kernel collection and authenticated SystemOS/root inputs.
