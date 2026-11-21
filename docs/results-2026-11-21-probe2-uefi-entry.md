# Probe 2 UEFI entry and Android return (2026-11-21)

The inspect mailbox image matched SHA-256 `56c84e6547c1b21a18854a8acc754f050c1a1cbb6595977899b3d6c94e61a536`. Before loading it, stock ABL reported product piano, userspace=no, snapshot=none, slot A successful and bootable, and six retries.

One `fastboot boot` succeeded. RAM firmware enumerated as `piano-uefi` and uploaded the full 7,340,032-byte console mailbox. Its signature was `0x1106` before XNU selection. The firmware reboot returned to completed HyperOS OS3.0.309.0.WPYCNXM on slot A, and the Android persistence marker still matched its SHA-256.

This run ended at the firmware stage. XNU handoff, mailbox retention after XNU and hard-reset recovery remain unverified for this image. The host issued no flash or erase command.
