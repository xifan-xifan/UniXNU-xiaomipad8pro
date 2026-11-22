# XNU pre-SCTLR tablet trial

The Android v2 image retained firmware, DTB and every other FAT file. The boot ID was recomputed and the changed KC was read back from FAT. Stock ABL reported A successful and bootable, six retries, and no snapshot update.

The watchdog returned to ABL. The reader collected 7,340,032 bytes and CVP offset `0x6f56a0` held `0x1000fc14793d`, the intended SCTLR_EL1 value. The entry and table completion records were present and the exhaustion marker was zero. Execution reached the store immediately before the SCTLR write; the write and subsequent ISB remain unverified.

The reader returned unattended to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, with `sys.boot_completed=1`. No persistent flash command was issued. The host log and capture are under `out/no-demura-control/pre-sctlr-live.*`.