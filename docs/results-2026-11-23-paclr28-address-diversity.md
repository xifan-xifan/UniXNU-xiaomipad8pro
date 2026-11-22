# 2026-11-23: PAC modifier high-16 fix, then address-diversity fix

Two RAM-boot trials on 2026-11-22/23. Neither wrote a partition, GPT entry,
boot image or userdata; each run returned unattended to completed HyperOS
`OS3.0.309.0.WPYCNXM` on slot A.

## paclr26: the first auth failure

The kernel stopped inside `_sysctl_register_oid_locked` at an `autda x16, x17`
whose x17 was built as `slot_address + diversity`. The failure path reported
`brk #0xc472`, which `llvm-nm` names `BRK_AUTDA_FAILURE`.

A clang cross-check of the same operation on the Mac produced:

```
mov  w8, w2
bfi  x1, x8, #48, #16
mov  x17, x0
mov  x16, x1
pacda x17, x16
```

So the modifier for a non-zero diversity is
`low48(slot) | (diversity << 48)`, not `slot + diversity`.

## paclr27: high-16 blend, live result

`paclr27` applied that form to both signing paths. Build artifacts:

| artifact | size | SHA-256 |
| --- | --- | --- |
| `paclr27.macho` | 28,034,272 | `d45152fa660b8df9f1c677ce6eb55d356b57d72431b3ffb4e2584deb3d94e50f` |
| `paclr27.kc` | `0x8184000` | `a7e6c5d2072de8c2d824d0f3ab54b9f31ebbbe3000c50e36670a0b767ca1a9a2` |
| `boot-piano-paclr27.img` | 185,733,120 | `5aa94fd888d4ef90b661a89f73127b5cac72d773faac653c91e4354823d937bd` |

Live run (`run-paclr27.ps1`, `fastboot boot` plus reader readback, then
unattended reboot):

- `port_crumb = 102`, i.e. `arm_static_if_init` returned; paclr26 stopped at 0x13.
- fixup walker: seen = fixed = 364,813, stop = 0, rc = 0.
- slot 0x4c pointed at the string `timeouts`; the kernel had passed the earlier
  `sysctl` and `timeouts` bootstrap stages.
- panic buffer: 131,072 zero bytes.
- device returned to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`,
  `sys.boot_completed=1`.

The sysctl probes were not observed and cannot be claimed as passed: their
codes 0x61..0x6b stay zero. The `0x6c` counter belongs to the exception probe,
not to the sysctl end probe.

## paclr27 residual fault: address-diversity auth pointer

The exception probe recorded the last of six kernel synchronous faults:

```
ESR  0x0000000072000000
FAR  0x0000000000000000
PC   0xfffffff00bc0447c
x16  0xb1d339f00c2aba74
x17  0xfffffff007dbc070
```

Symbolizing against `kernel.macho`: the PC is a `blraa x16, x17`; x16 is a
signed pointer to `___chkstk_darwin` (`0xfffffff00c2aba74`); x17 is the address
of `l___chkstk_darwin$auth_ptr$ia$0$addr`, i.e. the auth_ptr slot itself. A
zero FAR with a poisoned target is the expected shape of a failed instruction
authentication.

The canonical code in `rebase_chain()` in `osfmk/mach/dyld_kernel_fixups.h`
only blends when the discriminator is non-zero:

```c
if (hasAddressDiversity) {
    if (discriminator) {
        discriminator = ptrauth_blend_discriminator((void*)address, discriminator);
    } else {
        discriminator = address;
    }
}
```

`paclr27` masked every addrDiv slot to its low 48 bits. For diversity 0 that
dropped the top `0xffff` of the slot address, so the modifier no longer matched
what `blraa x16, x17` uses. The chkstk auth_ptr is signed against the full
64-bit slot address.

## paclr28: fix and build artifacts

`paclr28` changes both paths so that addrDiv with a non-zero diversity still
uses `low48(slot) | (diversity << 48)`, but addrDiv with diversity 0 uses the
full `(uintptr_t)slot` address. No other kernel change was made relative to
`paclr27`.

| artifact | size | SHA-256 |
| --- | --- | --- |
| `paclr28.macho` | 28,034,272 | `ed4de92f1d7d7b60bda5c03727ac85be50c827fe65092bbeae392edfc47339d8` |
| `paclr28.kc` | `0x8184000` | `b5200a561d1597c11e3b2a1eec94843c42cc82982bbbd6e35b1de43169f6921b` |
| `boot-piano-paclr28.img` | 185,733,120 | `0082bb8820bbba66e6c931ff5072d2d9b2cc7b9aee35f5641476e5f695fd8d11` |

Status: built, to be run. The tablet was not enumerated on USB when the image
was built, so no paclr28 result exists yet and none of the goals beyond the
previous boundary are proven.

## Unproven

- paclr28 boot behavior, including whether the chkstk auth_ptr passes.
- the sysctl registration path.
- anything past the `timeouts` stage: desktop, GPU, input, network, audio,
  Data persistence, Android recovery and switching.

