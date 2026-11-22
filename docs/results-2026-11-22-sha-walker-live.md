# SHA-512 runtime fixup traversal

One RAM boot of the hash-verified pre-stack candidate used the existing walker probes and a targeted readback. After watchdog return, the temporary reader recovered stage 186, 364,815 visited slots, 364,815 rewritten slots, stop reason zero, and return code zero. The counts match the offline walk of the same KC. This rules out the walker stopping before the SHA-512 slots in this trial; it does not verify the signed pointer at the call site or entry into the target.

ABL preflight confirmed bootable and successful slot A, six retries, and no snapshot. HyperOS OS3.0.309.0.WPYCNXM returned unattended on slot A with boot completed. Local evidence is under `out/no-demura-control/sha-walker-*`. Next, distinguish the descriptor and target used at the actual SHA-512 call from authentication and target entry, recording only integer checks.
