# The UFS DMA stall is inside the first contiguous buffer, and the recovered KC builder cannot re-emit a built collection

Date: 2026-11-23. Device `7ccdd855`. Only `fastboot boot` was used; no partition, GPT or userdata write. Every image in this note carries
`piano-reset-after=20 piano-reset-to=bootloader`, so each run returned by itself: the Gunyah watchdog bit 32 s after arming and the
tablet came back to Android with `boot_completed=1` in 67.6-67.7 s. No key press or power cycle was asked for.

## Where the root path stops (established image)

`paclr53-rootshort.img` boots the base collection. Its pstore console shows the UFS stages and then

```
NativeUfs: stage controller map
NativeUfs: HC cap=d57030f ver=00000400 hcs=0000010f hce=00000001 is=00000400
NativeUfs: stage DMA allocation
NativeUfs: DMA attempt 0: 2138112 bytes (slots 16, wide 1)
```

There is no `DMA attempt 1`, so the first `IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut |
kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, 2138112, 4096)` never returned. The same console keeps printing later boot
progress from other CPUs (`CoreAnalyticsHub start`, `mbinit`, `NativeGopDisplay: start on AppleParavirtGPU`) after that line, so this
is a thread that is stuck inside the allocator, not a kernel panic and not a dead machine. The allocator call is the first physically
contiguous, mapper-less 2 MiB allocation of the boot, and it is made from the kext's `start` path.

## What the instrumentation cost

`package/NativeUfs/NativeUfs.cpp` now logs `entering inTaskWithOptions`, `returned no buffer`, `returned a buffer` and the
virt/phys/contiguous rejection reason around that call. The kext object compiles on the Mac (`strings` confirms the new literals), but
the collection it has to live in cannot currently be built correctly, see below.

## Re-emitting the built collection as the KC base produces an unusable collection

The Mac copy of the builder is the one recovered from `out/k27-independent-20261120/host-run/kc-preflight`; the only local KC base is
`out/no-demura-control/paclr53-unpatched.kc`, which is itself a built collection (81 entries, 135,806,976 bytes). Four builds from it
were tried and each was booted unattended:

| build | keep | native | size | result |
| --- | --- | --- | --- | --- |
| `ufslog.kc` | `configs/kc-keep.txt` (47 entries) | NativeUfs | 147,095,552 | boots to IONVRAM, then nothing |
| `piano-ufs2.kc` | all 80 base entries except NativeUfs | NativeUfs | 147,095,552 | same |
| `piano-id.kc` | all 81 base entries | none | 147,095,552 | same |
| `piano-id-c.kc` | all 81 base entries | none, then `compact-kc-linkedit.py` | 114,442,240 | same |

The identity build (`piano-id.kc`) re-links nothing: it keeps every entry, native and Apple, as it already is in the base, and it still
fails. Console comparison against the working image is unambiguous:

| marker | rootshort (works) | piano-id |
| --- | --- | --- |
| `IONVRAM` | 32 | 3 |
| `ktv` probe lines | 72 | 9 |
| AMFI / security policy lines | present | none |
| `piano-reset-to` arming | present | none |
| `StartIOKitMatching` | present | none |
| `NativeUfs` | 28 | 0 |

So the kernel runs, gets through the VM and OSLog setup, and stops right after IONVRAM, before AMFI and before kext matching. Linkedit
compaction does not change that, so size is not the cause.

Two further facts fit the same picture: the builder emits a 147 MB collection where the base is 135 MB (its `__LINKEDIT` grows from
37 MB to 49 MB, even when nothing is re-linked), and the documented 26-kext recipe does not fit in it at all ("no room for a
0x430d0-byte native kext __DATA" at PianoWifi) because the recovered `scripts/kc-build.py` lacks `patches/k27-kc/0001` (best-fit
placement). The 27.3 line expects Apple's own `kernelcache.release.mac17j` as `KC_BASE` (`tooling/k273/build-k273-image.sh`:
`KC_BASE=$I/kc/kc273-mac17j.bin`, `KC_BUILDER=k27/kc-build273-piano.py`, `KC_COMPACT_LINKEDIT=1`), not a previously built collection.

## Blocked on

`out/inputs/k273/kc/kc273-mac17j.bin` and a k27 tree carrying `patches/k27-kc/0001` and `0002`
(`tooling/k273/prepare-k27-tree.sh`). With those, the recipe is: keep the Apple entries, `--native` the 26 descriptors of
`configs/k27-native-kexts.txt`, best-fit placement on, compact linkedit on. Without them the DMA instrumentation cannot reach the
tablet, and the next step of the root bring-up (why the contiguous allocation blocks: deferring `startRoot` past `IOService::start`,
or allocating after the pageable pool exists) cannot be built either.

## Host and device check

`tooling/native/run-paclr53-ufs2.ps1`, `run-paclr53-id.ps1` and `run-paclr53-idc.ps1` repeat the ABL preflight, boot with
`piano-reset-after`, dump `9cb00000 700000` and `8c5945000 20000`, then require Android `boot_completed=1`. Each run took 67.6-67.7 s
from `boot OKAY` to fastboot without any human action.