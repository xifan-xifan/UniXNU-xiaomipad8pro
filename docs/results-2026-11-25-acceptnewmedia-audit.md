# acceptNewMedia audit: where IOBlockStorageDriver::start can stop (2026-11-25)

Host-side only: Apple's IOStorageFamily sources, the local macOS 27.3 kernelcache
and its symbols. No image was built or booted and the tablet stayed off USB.

## The start path, from Apple's source

`out/IOStorageFamily-e45fbe30a7879e0b16e26dddb78538f1b00e0bff/IOBlockStorageDriver.cpp`
(the copy kept for this bring-up) gives the chain:

| Line | Call | Console evidence in ufs171 |
|---|---|---|
| 203 | `handleStart(provider)` | — |
| 1247 | `reportRemovability` | `reportRemovability` entered |
| 1373 | `checkForMedia()` | — |
| 951 | `reportMediaState` | `reportMediaState` entered |
| 968 | `mediaStateHasChanged(Online)` | — |
| 1602 | `recordMediaParameters()`: `reportBlockSize`, `reportMaxValidBlock`, `reportWriteProtection` | all three entered |

`recordMediaParameters` returning is the last event ufs171 logged, so the stop is
after it, inside `acceptNewMedia` (line 840; reached from `mediaStateHasChanged`):

1. `instantiateMediaObject(0, nbytes, blockSize, name)` — `new IOMedia`, its
   `init`, two `setProperty` and `setName`;
2. `m->attach(this)` — the parent link;
3. the `IOUnit` walk and `m->attachToParent(parent, gIODTPlane)` — a registry
   plane insert;
4. `_mediaObject = m; m->registerService(kIOServiceAsynchronous)`.

Any of those four can be the terminator. The registry never shows an `IOMedia`
node, which only rules out a return from step 3 onwards plus a later republish;
it does not say which of the four stopped.

## Symbols this audit leaves ready for a binary probe

From `out/inputs/piano-ufs264.kc` (147,095,552 bytes; kernel `com.apple.kernel`
`LC_UUID` `c9f3efdd10213d598275f32cd24564b6`, 81 fileset entries), read with the
local `kc-preflight` parser:

| Symbol | Entry |
|---|---|
| `IOBlockStorageDriver::acceptNewMedia()` | `0xfffffff00aeb8260` |
| `IOBlockStorageDriver::instantiateMediaObject(...)` | `0xfffffff00aeba1d4` |
| `IOBlockStorageDriver::recordMediaParameters()` | `0xfffffff00aeba5d0` |
| `IOBlockStorageDriver::mediaStateHasChanged(...)` | `0xfffffff00aeb8d18` |
| `IOService::startMatching(unsigned int)` | `0xfffffff00c1450ec` |
| `_piano_dbg_kv` | `0xfffffff00bbc7590` |
| `_piano_stage_slots` (192 entries of 32 bytes, codes 0x0b..0xca) | `0xfffffff00c912880` |

`IOStorageFamily` keeps its `LC_SYMTAB` in the collection (1,477 symbols), so a
probe can be placed at a named entry without a relink.

## What a rebuild needs here, and what is missing

Verified present on this host:

- the KC builder: `out/k27-independent-20261120/host-run/kc-preflight/` with
  `k27/kc-build273-piano.py`, `k27/kc27-layout.py`, `scripts/kc-build.py`,
  `scripts/kc_fileset`/`fileset_kc.py`, `kc_native.py` — the 27.3 line's builder;
- the base collection `out/inputs/piano-ufs264.kc`, which already carries the 26
  native kexts and the relinked kernel;
- kernel Mach-Os from the probe series (`out/no-demura-control/paclr53.macho`,
  `paclr150.macho`, ...), each with an `LC_UUID` to check pairing against the base.

Missing: `PIANO_K27_TREE`. `tooling/k27/build-native-kext.py` compiles a kext from
the tree's own compile recipes and generated headers
(`<tree>/out/x12377/build/xnu/k27-native-obj/.../*.json`); no copy of that tree
exists on this host or on the Mac (`~/Downloads/XiaomiPad8Pro-Ubuntu`, `~/Codex`,
`/private/tmp`), and its locked commit `11ac316b...` has no public source. Kernel
kext sources can therefore not be compiled here today.

Consequence for the next image: a kernel-only instrumentation (patch a kernel
Mach-O, rebuild over the base with every native bundle kept) or an in-place patch
of the collection's code bytes needs no tree; a change to `package/NativeUfs`
does.

## Next step

Instrument `acceptNewMedia` at its four steps with distinct `piano_dbg_kv` codes
and read the table back with `oem dump` after the run. That one image separates
"stuck creating IOMedia" from "stuck linking it" from "stuck inserting it in the
driver tree" from "created and registered, matching never runs", which is
currently three hypotheses in one.
