# NativeUfs static audit (2026-11-25)

Host-side only. No image was built, nothing was booted; the tablet stayed off USB.

`com.unixnu.driver.NativeUfs` in `out/inputs/piano-ufs264.kc` has no `LC_SYMTAB`, so its
text was located by cross-referencing the kext's own `__cstring` section. Results:

| Function | Address | Evidence |
|---|---|---|
| `NativeUfsBlock::start` | `0xfffffff008b514f8` entry, log at `0xfffffff008b514cc` | sets `Content` = `Apple_APFS` and `Content Hint` = `EF57347C-...`, logs `media content Apple_APFS hint`, then `mov w0,#1; ret`. It does **not** register the service or call `publishMedia`. |
| `publishMedia` | `0xfffffff008b516c0` | `new IOMedia`, `init`, `attach`, then a vtable call at `+0x5e0` with `w1=2` (synchronous `registerService`), followed by `getState` (`+0x5d8`) and `getBusyState` (`+0x658`); the log at `0xfffffff008b51a78` is `media register returned state %x busy %u`. |
| publisher thread | around `0xfffffff008b5dbb4` | calls `registerService` with `w1=0` (asynchronous) on the window, logs `%s registered, state %x`, then calls `publishMedia` at `0xfffffff008b5dc5c`. |

So the shipped collection matches the current source: the window is registered asynchronously,
then the publisher thread creates and synchronously registers the `IOMedia`. The
`IOBlockStorageDriver` that Apple matches against the window runs `start` on IOKit's config
thread, and the ufs171 console ends inside its `acceptNewMedia` after
`recordMediaParameters`. That makes the stop independent of any later `publishMedia` step and
keeps the ufs265 probe (codes `0xA0`-`0xA8` at entry, `instantiateMediaObject`, `attach`,
`attachToParent`, `registerService`) the correct instrument.

Probe assets were re-hashed this session and match the handoff:
`boot-piano-ufs265-probe.img` `86b534f2…` (185,733,120 B),
`boot-piano-ufs266-smprobe.img` `a56f132d…`,
`piano-ufs266.kc` `9f276dff…` (147,095,552 B).

Nothing was written to the tablet. Next step is unchanged: run `run-ufs265-probe.ps1` once a
device appears in `fastboot devices`.
