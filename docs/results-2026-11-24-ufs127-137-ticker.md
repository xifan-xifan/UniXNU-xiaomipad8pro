# ufs127-137: the ticker starves what it protects, 2026-11-24

Every run below was `fastboot boot` of a keep-all KC image with the in-image
`piano-reset-after=240` and the Gunyah watchdog as the backstop. Each image
returned unattended to completed HyperOS `OS3.0.309.0.WPYCNXM` on slot `a`;
no run needed a key press or a manual reset.

## What each round changed and where it stopped

| round | change | outcome |
|---|---|---|
| ufs127 | `delay` spin calls a timer service, ticker also spins | delay service ran 1024 beats; reached the media registry walk, then hung inside the ticker's `piano_gicv3_init()` |
| ufs128 | ticker no longer re-inits the GIC | full 240 s, but the ticker's own CPU never enabled PPI 27 (`iar 3ff irq 0`) |
| ufs129 | per-frame redistributor sweep in the ticker | hung in `piano_gic_all_rd`, 80.9 s watchdog |
| ufs130 | media wait polls with `copyMatchingService` and services timers | reached `vfs_mountroot`, then `netboot: no suitable interface` / `We are hanging here`; NativeUfs never published |
| ufs131 | timer service gated on ticker start | NativeUfs still never published; the polling wait starved the match thread |
| ufs132 | `thread_block_reason(AST_YIELD)` replaced by `thread_yield_to_preemption()` | NativeUfs published, GPT and manifest read, `2 windows published`, AKServices, `piano:cfg`, then the ticker stopped after its first yield |
| ufs133 | ticker yields every 100 beats | eight beats, five real PPI 27, NativeUfs published, **NativeRamBlock started and its 8192 MiB window logged**, then everything froze |
| ufs134 | ticker unmasks DAIF for 1 ms each beat | dies the instant DAIF drops (`sleh_irq` path); 81 s watchdog |
| ufs135 | ticker spins and never yields | ticker ran all 240 s at 1000-beat prints, but NativeUfs never even started: a pure spin starves the boot thread |
| ufs136 | ticker started at priority 95 | ticker yields at 100 and resumes at 100, yields at 200 and is never scheduled again; boot never reaches NativeUfs |

## The mechanism the rounds pin down

`thread_yield_to_preemption()` only blocks when an `AST_PREEMPT` is pending on
the current processor. Without a timer tick nobody raises that AST, so a
routine that spins instead of blocking never hands the CPU back. ufs136 proves
the yield itself works (the ticker resumed once at beat 100) and that priority
is not the lever: a high priority ticker simply starves the boot thread earlier.

The two halves that each advanced the boot are therefore complementary:
ufs127 showed that servicing `rtclock_intr` from the delay spin keeps timeouts
alive; ufs133 showed that a yielding ticker lets the boot reach
`NativeRamBlock`. ufs133 and ufs136, however, carry a **bare** delay spin, so
the first `IOSleep` loop in the boot thread never services the timer and the
ticker never gets an AST to yield on.

ufs137 merges them: the ufs133 ticker (default priority, yield every 100 beats)
plus the ufs127 delay-path timer service appended to `clock.c`.

## Image identity

| round | kernel sha256 (prefix) | image sha256 (prefix) |
|---|---|---|
| ufs127 | `ce7f18e1` | `c4728416` |
| ufs128 | `8e03f541` | `0022c2a3` |
| ufs129 | `f677eb4c` | `29f97943` |
| ufs130 | `558c3730` | `3113b510` |
| ufs131 | `21956776` | `534e2d63` |
| ufs132 | `b1c0125f` | `c8b651d8` |
| ufs133 | `c8a495e0` | `0561e0e7` |
| ufs134 | `db4506bc` | `6b517f10` |
| ufs135 | `d0867db4` | `c7da82ef` |
| ufs136 | `4ae88d1e` | `5d64240a` |
| ufs137 | `06ca0b86` | `dec100fd` |

`fastboot boot` and `data-persist=0` do not mean "writes nothing": the images
still publish the UFS root window, read the GPT and the paired manifest, and
run the AMFI and IOBSD paths. The unattended reset only reclaims the RAM disk;
no partition was written and slot `a` stayed `successful:yes retry-count:6`.