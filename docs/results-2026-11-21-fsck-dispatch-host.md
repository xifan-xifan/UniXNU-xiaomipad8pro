# Host fsck dispatch check (2026-11-21)

The Mac attached the retained SystemOS image as read-only media and resolved APFS volume UUID `284548A8-FC87-434E-A0C8-A4038E83704D`. The host `/sbin/fsck -n <volume>` printed usage and exited 1. Direct `/sbin/fsck_apfs -n <volume>` checked that UUID and returned 0. The attachment was detached.

A host wrapper option probe with `-fny` and no device unexpectedly selected the Mac host container `/dev/rdisk3`. The APFS helper refused the mounted writable container and returned 65. Treat wrapper calls without an explicit target as potentially addressing the host filesystem. Use the direct APFS helper with an isolated read-only attachment for further image checks.

Static inspection of the retained 26C5035r `/sbin/fsck` (SHA-256 `044c972629a232f620a326adf494644b3b8625692c853d12aa95236a74ed9c7d`) found `fstab` and root diagnostics and the same usage string. The 26C5035r binary was inspected on a read-only attachment and was not executed. Its `-R 1` behavior and runtime target selection on the tablet remain unverified. No tablet command was issued.
