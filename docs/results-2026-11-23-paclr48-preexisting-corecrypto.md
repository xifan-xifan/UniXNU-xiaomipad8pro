# 2026-11-23: paclr48/paclr49 pre-existing corecrypto registration

## Root cause

The verified base collection `/tmp/piano-verified-collection.68bZb6/apple-normalized.bin` already contains `com.apple.kec.corecrypto` version 27.0 (`BuildMachineOSBuild 23A344017`). The Apple 27.3 kernel was linked separately, but that old collection kext started first and published its older global function table before `source_corecrypto14_bootstrap()` called the linked 27.3 `corecrypto_kext_start()`.

paclr48 recorded the collision directly:

```
0x38b  0xfffffff005ec2fa8  &kpis passed by 27.3 corecrypto
0x38c  0xa5d36af00c29fdc4  _piano_kpi_digest
0x38d  0xfffffff008ab6d58  g_crypto_funcs after the call
0x38f  0xffffffff          register_crypto_functions() returned -1
0x390  0xf7ffbff008ab6d58  bootstrap's g_crypto_funcs
0x391  0                   digest_ctx_size_fn
0x392  0                   digest_fn
```

The old table lacks the 27.3 KPI suffix slots, so registration failed and the digest dispatch remained zero. This was a collection-composition fault, not a failure of the 27.3 corecrypto implementation.

## paclr49: disable only the old module entry

paclr49 keeps the complete paclr48 kernel and KC layout. It changes only the old collection kext's `_corecrypto_kext_start` entry at final VA `0xfffffff00b8ed4ec` from

```
5f2403d5 7f2303d5
```

to

```
00008052 c0035fd6    mov w0, #0; ret
```

The old kext's code and data remain present for its original dependants, but its module start can no longer pre-register the 27.0 table.

- `paclr49.kc` SHA-256 `9b1804099117f282be6307eef9fda39e55816ec004e61b45b01eb0e0c5a39d9e`
- `boot-piano-paclr49-fips4.img` SHA-256 `71b94e1d224b67b264b72221abed83f01a26986651824d601366ad288242312d`

Both boot entries use `fips_mode=4`; the image is 185,733,120 bytes. The kernel and all other KC bytes are inherited from paclr48.

## Device result

One unattended `fastboot boot` was issued. The ABL preflight reported `piano`, slot A, successful A, retry count 6 and no snapshot update. The tablet automatically reset from the test boot, the reader was loaded, all rings were captured, and Android returned with `boot_completed=1` on `OS3.0.309.0.WPYCNXM` (`_a`). No partition or GPT write was issued.

The corecrypto sequence advanced through the former barrier:

```
0x38b  0xfffffff007ec6fa8
0x38c  0xa5d26af00c29f9c4
0x38f  0x0                  register_crypto_functions() succeeded
0x333  0x3330               corecrypto_kext_start returned success
0x334  0xfffffff007ec6fa8   bootstrap retained the published table
0x390  0xfffffff007ec6fa8
0x391  0xd2bc62f00c29faf4   digest_ctx_size_fn non-zero
0x392  0x21d36af00c29fdc4   digest_fn non-zero
0x386  0x2                  digest selector
0x389  0xfffffff007ec6f60
0x380  0x38000              SHA-1 dispatch returned
0x381-0x385 present         SHA-1 and HMAC results passed
0x236  0x60003360
```

The kernel then continued through the early startup path. The stored log area reaches VM zone initialization and IONVRAM setup. The trace ring records a kext `start()` result of 0 (`0x2138=0`, `0x2136=0`) and load return 0 (`0x2105=0`). The panic region contains no printable panic text. The test boot later reset automatically, so the next diagnostic boundary is after the observed kext-start/IONVRAM stage rather than corecrypto.

## Reproduction files

- `out/no-demura-control/paclr49.kc`
- `out/no-demura-control/boot-piano-paclr49.img`
- `out/no-demura-control/boot-piano-paclr49-fips4.img`
- `out/no-demura-control/wsl-patch-corecrypto49.py`
- `out/no-demura-control/paclr49-package.py`
- `out/no-demura-control/run-paclr49-fips4.ps1`
- `out/no-demura-control/paclr49-fips4-*.bin` and `paclr49-fips4-live.log`

This is a diagnostic packaging change. A durable build should either source a 27.3 collection entry or move the module-entry suppression into the audited collection builder after the remaining startup path is known.