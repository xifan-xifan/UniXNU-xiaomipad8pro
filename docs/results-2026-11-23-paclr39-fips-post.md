# 2026-11-23: paclr39 corecrypto FIPS POST boundary

Three RAM-boot trials localized the first new barrier inside
`corecrypto_kext_start()`.

## Instrumentation correction

`piano_dbg_kv()` accepts only ids `0x0b-0xca`; higher ids are dropped. Value
records above that range must use `piano_dbg_ckptv()`, which writes into
`piano_ckptv_ring` and is decoded with `decode-ckptv.py`. paclr39c used the
wrong helper, so its `0x33x` records were empty by construction; paclr39d
fixed this.

## paclr39d: start of corecrypto

`boot-piano-paclr39d.img` SHA-256
`c04dd575f0f761dbe420e51cef6c99de5d3977239b5de296cc16dd7507632281`.

```
0x330  entered source_corecrypto14_bootstrap
0x331  fips_mode = 0
0x332  about to call corecrypto_kext_start
0x333  absent -> corecrypto_kext_start did not return
```

## paclr39e: inside corecrypto

Corecrypto `corecrypto_kext.c` was instrumented (`0x340-0x34a`) and only that
object rebuilt into `libcorecrypto_kernel_noasm-namespaced.a`
(SHA-256 `05a0959d117c72721ac50a5fdf3bf486584dec691a4f2d05bda98458aa77eafd`,
529 members). Default boot:

```
0x340  entry
0x341  before fipspost_post
0x342  absent -> fipspost_post did not return
0x343  0xfffff7e600003430  low32=0x3430, high32=-2074
```

The high word is the `fipspost_post` result; the code then panics with
"FIPS Kernel POST Failed". Without a serial console the panic display stalls,
which looked like a non-returning call.

## paclr39e-fips4: POST bypassed

Two boot entries had `debug=0x14e` replaced in place by `fips_mode=4`
(`vm_compression_limit=1572864` retained). Image SHA-256
`065781d11d638625c051ee27c33e3f8cefd608637f7bb9e7f27139b5f4451490`.

```
0x331 = 0x33100004       fips_mode parsed correctly
0x343 = 0x3430           fipspost_post returned OK
0x344-0x348 present
0x349 absent
```

`0x349` sits after `cc_kext_random_init()`, so the next boundary is
`register_and_init_prng()`. All runs returned unattended to completed
HyperOS OS3.0.309.0.WPYCNXM on slot A; no persistent write was issued.
