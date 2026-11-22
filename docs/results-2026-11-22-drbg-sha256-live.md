# Early-random DRBG barrier cleared on the tablet

Date: 2026-11-22
Image: `out/no-demura-control/boot-piano-drbgsha256.img`
Image SHA-256: `19C094B7179F0C493E887DA33679E2D50960ACC46E8E378E0AEF2D9D8ABEEA51`
KC SHA-256: `e6ed0bd7f8554f0f55583a651ebd0c72a890d45d7fdae505b5461fb675a3525a`
Kernel (`kernel.macho`) SHA-256: `85299f0310e4b8ca093e5f2db3e2e4a4d018780a0d77509ac536d0efd0484371`
`cchmac_init.o` SHA-256: `ba36b8bc1d458f488053de672deeeda433bd4f08b934eb4aa5b3c91d4415b6c1`
Android boot v2 ID: `2f54fbb1838903a6d18472d45c35bce9bc431f1f`

## Prior boundaries

`drbgdirect` (`boot-piano-drbgdirect.img`, SHA-256
`4370DAE75112483447491DBFB1BF808287078792B89C6FCA1D505FC531B7549B`) stopped at
phase 232, immediately before the DRBG `update()` call. `drbgupdate`
(`boot-piano-drbgupdate.img`, SHA-256
`0471C9E3203CAF47CCEDDCEB7ED47E87358ED89D99DE2A2408169B6987A02C72`, boot v2
ID `79969940eeda7b7180524743c66031cb3851ae90`) stopped at phase 236, at the
first loop entry inside `update()` and before the first `cchmac_init()`
returned. Both runs returned unattended to HyperOS A.

## Root cause

The project keeps a local `cchmac_init()` patch that calls the compression
entry directly and skips `di->compress`. That field is a chained-fixup
authenticated function pointer, and the local fixup signer cannot reproduce the
discriminator form. The patch only covered `di->output_size == 64`, so the
SHA-512 seed path worked while the SHA-256 early-random DRBG still branched into
`di->compress` and faulted there.

The DRBG uses `&ccsha256_ltc_di` (`prng_random.c`), so the first `cchmac_init()`
inside `update()` was the first place to take that unpatched branch. Instrumented
runs bounded it exactly: `drbgdirect` stopped at phase 232, `drbgupdate` stopped
at phase 236 (loop entry, before the first `cchmac_init()` returned).

## Change

`cchmac_init()` now also routes `output_size == 32` to `ccsha256_ltc_compress()`
for both the outer and inner state compression, mirroring the existing SHA-512
branch. `ccdigest_update()` and `ccdigest_final_64be()` already had the same
SHA-256 direct-call branch.

## Live result

ABL preflight: `product=piano`, `current-slot=a`, `is-userspace=no`,
`slot-successful:a=yes`, `slot-unbootable:a=no`, `slot-retry-count:a=6`,
`snapshot-update-status=none`.

`port_crumb` after the RAM boot was **13**. In `arm_init.c` that value is written
after `early_random()` returns and before `machine_startup(args)`. Phase 63 and
the whole bootseed/DRBG construction now complete. The previous boundary at
`ccdrbg_init()` is cleared.

The device returned unattended to HyperOS `OS3.0.309.0.WPYCNXM` on slot `_a`
with `boot_completed=1`.

No partition, GPT, boot image, or userdata write was issued; this was a
`fastboot boot` RAM run.

## Next boundary

`machine_startup()`. The next diagnostic should place breadcrumbs or console
capture inside the startup path rather than repeating any DRBG image.