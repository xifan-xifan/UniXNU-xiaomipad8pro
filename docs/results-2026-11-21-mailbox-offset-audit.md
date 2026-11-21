# DarwinBoot mailbox offset audit (2026-11-21)

The retained 7,340,032-byte CVP dump was inspected against the mailbox layout recorded by the DarwinBoot patch. The expected base `0x9D1F5000` maps to offset `0x6F5000` when the dump base is `0x9CB00000`; all eleven 4 KiB checkpoint pages at that offset are zero. The `PANO EBS1` magic is absent from the entire dump.

The dump is not uniformly zero: 1,176,334 bytes are nonzero, with a small unrelated nonzero tail near offset `0x6FEFF8`. That rules out a simple all-zero transfer failure, but it does not identify a retained loader record. The existing reader therefore transferred nonzero CVP contents while the expected records were absent or cleared before readback. No tablet command was issued in this audit.
