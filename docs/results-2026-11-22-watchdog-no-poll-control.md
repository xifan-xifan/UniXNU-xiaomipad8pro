# No-poll watchdog control

Repacked `out/boot-piano-witness-watchdog-gate.img` with `out/verified-no-poll-23aca0b8/verified.kc`. The Android v2 verifier functions confirmed the original shim, FD and DTB bytes and validated the recomputed boot ID on readback. Extracting every file from both FAT volumes showed that only `kernel.macho` changed. Its SHA-256 is `72e5240f8a3286d9233136a540c7129dab4c66fc3d2789c8c5287f6e18332601`.

The inherited DarwinBoot stops after successful ExitBootServices, before calling DarwinEnter. This image can compare loader behavior across collections. A separate image must preserve DarwinEnter and record XNU entry independently. The menu sets `data-persist=0`. The storage audit and unattended recovery remain separate checks. No tablet run was performed.
