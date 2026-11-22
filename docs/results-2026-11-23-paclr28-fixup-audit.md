# 2026-11-23: paclr28 chained-fixup slot audit

Offline audit of `/private/tmp/piano-paclr28/kernel.macho` (the kernel used for
`paclr28.kc`). The script parses `LC_DYLD_CHAINED_FIXUPS`, walks every
format-7 (`DYLD_CHAINED_PTR_ARM64E_KERNEL`) chain and decodes the auth fields.

```
format 7  total 117189
auth, addrDiv=1, diversity=0    285
auth, addrDiv=1, diversity!=0   30948
auth, addrDiv=0                 12624
```

The 117,189 slots are the kernel image only; the packed collection reports
364,813 chained pointers once the kexts are included.

The slot behind the paclr27 fault, `0xfffffff007dbc070`, decodes as:

```
raw      0x80110000052a3a74
isAuth   1
addrDiv  1
div      0x0
key      0 (IA)
target   0x52a3a74  -> 0xfffffff00c2aba74 (___chkstk_darwin)
```

So all 285 zero-diversity address-diverse slots were signed by paclr27 with a
modifier that had the top 16 address bits cleared. The generated paclr28 header
now takes the full slot address in that case for both signing paths:
`sign_pointer()` in `osfmk/mach/dyld_kernel_fixups.h` and the inline
`DYLD_CHAINED_PTR_ARM64E_KERNEL` branch in the same header.

The 30,948 non-zero-diversity slots keep the `low48(slot) | (div << 48)` form.

This audit proves the input encoding and the code path, not the runtime result.
A paclr28 RAM boot is still required.
