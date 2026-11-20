# Mailbox readback on the tablet (2026-11-21)

The verified FAT16 menu image (`708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468`) booted temporarily and enumerated as `piano-uefi`. The OEM dump of `0x9cb00000` with length `0x700000` uploaded all 7,340,032 bytes. The local capture is `D:/Temp/piano-mailbox-be8ca0aa-2e47-4d11-9e89-40161b5ec95e/mailbox.bin`, SHA-256 `9517a0311440b8e0bc75c6afdaa01a24726d1bbb58518688829304f749c69d86`. Its initial bytes `06 11 00 00 08 ff ff ff 44 10 00 00` contain no expected XNU mailbox header. The menu image did not load XNU. This test establishes address accessibility and transfer length. XNU log retention after reset remains unverified.

ABL reported `is-userspace=no`, `snapshot-update-status=none`, `current-slot=a`, `slot-successful:a=yes`, `slot-unbootable:a=no`, and `slot-retry-count:a=6` before and after the test. Android returned to `OS3.0.309.0.WPYCNXM`, slot `_a`, with `sys.boot_completed=1`.
