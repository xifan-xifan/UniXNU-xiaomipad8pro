# 2026-11-23: paclr40 register_and_init_prng checkpoint run

`register_and_init_prng()` in `src/osfmk/prng/prng_random.c` was instrumented
with `0x350-0x357`, keeping the corecrypto `0x340-0x348` checkpoints.

- kernel SHA-256 `11501e7b9b93a5979b92ef6689c1c0ba92b550ed5068011b7b1d59ef32072b91`
- `boot-piano-paclr40-fips4.img` SHA-256
  `7b96fc920e000d4587788702fcbb5cdc67dd3eb23699ec22bcd27063e1ff5777`

Boot entries kept `fips_mode=4` and `vm_compression_limit=1572864`.

## Decoded `piano_ckptv_ring`

```
seq 44-51   0x350 0x351 0x352 0x353 0x354 0x355 0x356 0x357(0x31300)
seq 60-62   0x330(0x3300) 0x331(0x33100004) 0x332(0x3320)
seq 63-68   0x340 0x341 0x342(0x400003420) 0x343(0x3430)
            0x344 0x345(value 0x34000)
seq 69-71   0x346(id 0x946) 0x347 0x348
seq 72      0x350, no later checkpoint
```

## What this shows and what it does not

The corecrypto sequence confirms FIPS POST passed and execution reached the
tail of `corecrypto_kext_start()` before `register_and_init_prng()`. The second
`0x350` at seq 72 has no `0x351`, so the stall is at the top of
`register_and_init_prng()`: entry, the two asserts, or `entropy_init()`.

The seq 44-51 block is not trustworthy as an earlier successful call. The tree
has exactly one caller of `register_and_init_prng()`, inside
`corecrypto_kext_start()`, and only one `0x330` was recorded. The block also
contains torn records (`0x357` value `0x31300`, `0x345` value `0x34000`,
`0x346` id `0x946`) that match the 8-byte `piano_ckpt_ring` and ring-slot
collisions, so this dump mixes records from overlapping ring storage. Treat the
early block as evidence only that the checkpoint ring needs per-record
validation before it can be used for ordering.

Next: instrument inside `entropy_init()` and `cckprng_init_with_getentropy()`
with self-validating records, and record `prng_ready` at entry.

The device returned unattended to completed HyperOS OS3.0.309.0.WPYCNXM on
slot A. No partition, GPT or boot-image write was issued.
