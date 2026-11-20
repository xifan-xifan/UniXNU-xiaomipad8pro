# First diagnostic RAM boot attempt (2026-11-21)

The tablet reported HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, and `sys.boot_completed=1` before entering ABL. ABL reported `product=piano`, `unlocked=yes`, `is-userspace=no`, `snapshot-update-status=none`, `current-slot=a`, `slot-successful:a=yes`, `slot-unbootable:a=no`, and `slot-retry-count:a=6`.

The host rechecked SHA-256 `40482bc38d1566cddc1c4b74dbf9004a616355581f99e399feaf496004aa6dd5` and issued one authorized `fastboot boot`. Fastboot reported `OKAY` for both sending the 13,444 KiB image and booting it, with 0.553 s total host command time. The tablet did not enumerate in ADB or Fastboot eight seconds later or at the follow-up check. There is no screen or console observation confirming that the diagnostic menu ran. No explicit flash, erase, or GPT operation was issued. The experiment did not measure firmware variable or rollback state changes.

Recovery is pending. Use the documented physical reset to reach ABL, read back the same slot and snapshot values, then reboot and confirm HyperOS build, slot and boot completion. Keep this attempt distinct from menu execution and from successful recovery. Do not repeat the same launch without evidence that distinguishes the failure stage.
