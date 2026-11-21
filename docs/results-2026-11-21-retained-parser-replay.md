# DarwinBoot parser replay for retained collections

The UbuntuPiano DarwinBoot checkout at HEAD `5243c68ec332a227fe3680cd5da6d5a0e127035c` has local changes. Its existing `Test/test-kernel-parser.py` compiled the working-copy parser with Clang and ran its built-in checks before checking each archived input separately.

| Collection SHA-256 prefix | Host parser result |
| --- | --- |
| `d36b8047` (original ESP candidate) | Rejected |
| `3729e762` (archived subtype-2 candidate) | Accepted |
| `afe7646e` (separate loose collection) | Accepted |

The original collection has a reproducible host parser failure. The subtype-2 archive already passed this parser and its historical installation proves ESP readback. The exact failing predicate and the equivalence of this working-copy parser to the original tablet firmware remain unverified. SystemOS and root pairing and unattended recovery still require independent evidence before a tablet trial.
