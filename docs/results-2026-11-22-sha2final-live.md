# SHA-256 HMAC final trial

The `sha2final` RAM candidate advanced past the native HMAC path that previously stopped at phase 211. The boot image was hash-pinned before the unattended run. ABL reported product `piano`, active slot `_a`, `slot-successful:a: yes`, `slot-unbootable:a: no`, six retries, and `snapshot-update-status: none`. It was started with `fastboot boot`; no partition was written.

The eight-byte marker at physical `0x8c589be50` was `0x3e` (62). In the fixed tree this store follows the return from `bootseed_init()` in `early_random_init()` (`src/osfmk/prng/prng_random.c`); the next marker is 63 after `ccdrbg_init()` returns. Reaching 62 therefore proves the previous phase-211 boundary is gone and that `cchmac_final()` and `bootseed_init()` returned. The final trial's more detailed 212-227 sub-markers were overwritten by the subsequent 61 and 62 stores, so this run does not identify which direct final branch was taken.

The next boundary is between `ccdrbg_factory_nisthmac()` and the return from `ccdrbg_init()`: phase 62 was retained and 63 was not. The candidate must therefore be diagnosed in DRBG initialization before another boot.

The reader returned unattended to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, with `sys.boot_completed=1`.

## Artifact identity

| Object | SHA-256 |
|---|---|
| Boot image (`boot-piano-sha2final.img`, 185,733,120 bytes) | `F77C8F83538A4FEEF6D1F8893B9274415F6855BF4C1783A11F4A3A07F7D504F4` |
| Packed KC (`sha2final.kc`, 135,806,976 bytes) | `0ABF77FBFA77B493EFA26DDF75509F32DD391418F572C94262D20E7E5C6E9489` |
| Linked kernel (`sha2final.macho`, 28,033,600 bytes) | `B0B4A0CAA660670CDA0803E4E609C6E5BAEBE37097422B3045EF47C42FCF19A1` |
| Boot v2 ID | `835cc2bffb8ab5c069a99c9a3a58f996d224fe68` |
| Stage capture (`sha2final-stage.bin`) | `0334E6B55D71F1DC204118D5A631267C8AC81714F9031D8A0BE8D9CC4DE12282` |
| Live transcript (`sha2final-live.log`) | `A91CC7DB945506701D19F7BA6AEDCB5AB23903D089CF97611A549978C0DA2883` |

The image and capture are ignored run artifacts under `out/no-demura-control/`; their hashes identify the exact objects without committing binaries or logs.
