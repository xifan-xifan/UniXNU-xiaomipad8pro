# Watchdog-enter firmware inventory

Decoded the LZMA-compressed FVMAIN from the gate control and the rebuilt handoff candidate. Both have 174 FFS entries in the same GUID and type order. Exactly two FFS entries differ by SHA-256: DarwinBoot (`1b64af87-8415-4b57-9b16-680d70913b9f`) and BdsDxe (`6d33944a-ec75-4855-a54d-809c75241f6c`). The rebuilt BdsDxe output contains both the watchdog enable diagnostic and the setup-failure reset diagnostic. The BdsDxe change still needs a byte-level explanation before attributing a hardware difference solely to DarwinBoot.

The bundled no-poll KC contains the console mailbox initialization and timed reset diagnostics. A recovered XNU console message would establish XNU execution. The earlier zeroed mailbox windows do not establish whether an XNU log survives the complete reset and reader sequence. This candidate was inspected on the host. The tablet reported HyperOS OS3.0.309.0.WPYCNXM, slot A, and completed boot over ADB.
