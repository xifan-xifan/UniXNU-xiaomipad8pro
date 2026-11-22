# paclr53: two pthread registrations and the lost early store

Date: 2026-11-23. Device `7ccdd855`. Source tree `/private/tmp/piano-k27-independent.zbovHflG`, builder `/tmp/piano-linkinfo-relink.yab5k88c/k27/kc-build273-piano.py`, target 26C5035r. The run used `fastboot boot` only; no partition, GPT or userdata write was issued. HyperOS returned on slot `_a` with `OS3.0.309.0.WPYCNXM` and `boot_completed=1`.

## Candidate

`pthread_kext_register` now records both calls in `piano_pt_calls[4]` and publishes them through `piano_kmem_val()` slots 0..10. It keeps the idempotent overwrite introduced in paclr51 and does not panic. The probe buffer `piano_kmem_probe` is at VA `0xfffffff00c944100`, physical address `0x8c5944100` (256 bytes of used slots, 1024 bytes dumped).

- `paclr53.macho` SHA-256 `a58a6d19f58c094eadc385386b7c2dbbfafeefaee6bd220ade1630ca38c02df2`
- `paclr53.kc` after the corecrypto FIPS stub SHA-256 `4efc1deaf3efacd0cc7d6386b976017d26832a606f7a0ae8e6fa1ab952e02535`
- `boot-piano-paclr53.img` SHA-256 `2621910d2fe5b75017720c440ee4733e78bfb6d5b80d0561e985e90a4d609830`
- `boot-piano-paclr53-fips4.img` SHA-256 `0944fc4a7f5c0866e322f242b128351e8773593c4e463b5f8075c26856c20b87`

## Device result

The kmem dump repeats each value in four quadwords. Two rows contained single-bit damage in one or two copies, so the values below are the majority of the four copies.

| slot | meaning | value |
| --- | --- | --- |
| 0 | `fns` argument | `0xfffffff007ec8848` `_pthread_internal_functions` |
| 1 | `callbacks` argument | `0xfffffff00c9b5bd8` `_pthread519_pthread_kern` |
| 2 | return address | `0xfffffff00c2a6da4` `_pthread_start+0x20` |
| 3 | `pthread_functions` on entry | `0xfffffff007f40000` |
| 4 | `&pthread_functions` | `0xfffffff007e32e70` |
| 5 | `(n << 32) | i` | `0x0000000200000001` |
| 6 | `pthread_functions` after call 0 | `0xfffffff007f40000` |
| 7 | `pthread_functions` after call 1 | `0xfffffff007ec8848` |
| 10 | first word of `fns` | `0` |

Two registrations therefore occur. Both are called from the same `pthread_start` body, which passes `fns = _pthread_internal_functions` and `callbacks = _pthread519_pthread_kern`. The first call ends with slot 3 still reading `0xfffffff007f40000`; the second call's store of `fns` survives and slot 7 reads the expected functions table.

`_pthread_start` itself is unchanged at runtime apart from the linked shim: its only explicit caller is `source_pthread_bootstrap`, and the kext start routine supplies the other invocation. The first invocation happens before the write becomes visible in the final mapping; the second invocation occurs after the runtime kext/VM layout is in place. Slot 3 already held `0xfffffff007f40000` at entry, which is the start of the kext `__DATA_CONST` region rather than a valid functions table. The exact code that installs that value is still unidentified; it is not a chained-fixup site in the retained KC and no other store to `_pthread_functions` exists in the kernel image.

The previous re-initialisation panic is gone. Boot proceeds through IOKit matching to `NativeUfs* start on K!14cAP probe=2 root=0 root-part=linux_rootfs`, the same stop as paclr51. The remaining blocker at this point is the absent UFS root, not pthread registration.

## Reproduction

`out/no-demura-control/make-paclr53-build.py`, `paclr53.argv.json`, `paclr53-package.py` and `run-paclr53-fips4.ps1` reproduce the image and the unattended run. The Mac build printed `kernel 28035640 a58a6d19...`; the WSL KC step printed the patched corecrypto hash. The device run took about 4 minutes and ended with HyperOS `boot_completed=1`.

## Limits

The byte pattern written to the probe buffer shows occasional single-bit damage in the dump path. Treat the four-copy majority as the observation. The ckptv ring does not contain the pthread records in this run and is not used as evidence for the call count.
