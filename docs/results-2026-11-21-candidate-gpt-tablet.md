# Candidate GPT on piano (2026-11-21)

The frozen writer verified the staged candidate fragment hashes in RAM. It wrote the backup copy before the primary copy and verified both. A separate direct readback returned `primary=candidate backup=candidate`. Logs are `out/maintenance-gpt/candidate-write.log` and `out/maintenance-gpt/candidate-readback.log`.

Explicit exit returned to ABL. Slot A was successful and bootable with six retries and snapshot state `none`. `fastboot reboot recovery` entered stock Recovery without booting Android. Recovery reported `OS3.0.309.0.WPYCNXM` and `789100544` sectors of 512 bytes for `sda34`. The stock Recovery wipe and Android startup on the new geometry are pending.
