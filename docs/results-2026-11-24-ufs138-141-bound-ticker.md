# ufs138-141: a bound ticker, and the root media wait, 2026-11-24

Every run was `fastboot boot` only. Each image carried its own
`piano-reset-after=240` plus the Gunyah watchdog, and each returned unattended
to completed HyperOS `OS3.0.309.0.WPYCNXM` on slot A. No partition, GPT, boot
image or userdata write was issued.

## What changed and what it proved

| round | change | result |
| --- | --- | --- |
| ufs138 | ticker thread removed; the `delay()` hook owns the watchdog and the 240 s reset | NativeUfs brings up GPT, manifest and DMA and publishes two windows; `AKServices`, `piano:cfg`, `NativeRamBlock start` and its 8192 MiB window appear; the boot then freezes inside the 8 GiB seed hash and the watchdog bites at 81.6 s |
| ufs139 | `machine_idle` also services the timer | `idle-service` never prints; the boot stops on the same line as ufs138 |
| ufs140 | delay/idle stop calling `rtclock_intr`; a ticker thread masked-DAIF drives PPI 27 by hand | the boot passes NativeRamBlock, runs `bsd_init` and `bsd_autoconf`, walks the media registry and reaches `waitForMatchingService` for root UUID `84581E99-85BE-4957-AC53-2947E887B3E4`; the ticker then stops inside its first `rtclock_intr` |
| ufs141 | ufs126 per-statement prints silenced by starting `s126_n` at 2 | identical stop point, so the halt is not the console lock |

## Mechanism

`thread_yield_to_preemption()` only blocks while `AST_PREEMPT` is pending, so a
self-spinning ticker with no timer never yields (ufs135/136). The delay service
reaches the end of the boot path only until the 8 GiB SHA-256 in
`NativeRamBlock::start()` stops calling `delay()`.

The `thread_bind(thread, processor)` call used in ufs140 does not exist in this
tree: `thread_bind()` binds the *current* thread and takes one `processor_t`.
The ticker therefore printed `cpu=0`, and `cpu_to_processor(3)` was NULL. The
console shows `MAXCPU:8` but only `mpidr 80000000`: this build starts the boot
CPU alone, so a secondary-CPU ticker is not available yet.

The stop after the ticker's first beat is in `rtclock_intr()` on the boot CPU:
a thread-context fake interrupt can run while the interrupted thread holds
state that `timer_intr()` needs. ufs122's ticker ran 132k times because the boot
thread was in the early timer phase; at the root media wait the same call
stops.

## Next

Two independent items:

1. Timer delivery that is safe on the single boot CPU, or a real PPI 27 entry
   (`sleh_irq`) instead of a thread-context fake interrupt.
2. NativeUfs does not publish an `IOMedia` for the root UUID in ufs140/141, so
   the 30 s match would fail even if the timeout expired. The root publisher
   thread and the UFS media registration need their own audit before the wait
   can succeed.

Artifacts in `out/no-demura-control/` (not committed):

| round | kernel sha256 (prefix) | image sha256 |
| --- | --- | --- |
| ufs140 | `a59de386` | `b8e70224718c8ac7107367e6e85d9aaca48a8477b36509b79d8eac6ff2fe2531` |
| ufs141 | `819be797` | `f37e656c55f81b05e31dd70c7fc85c2416a149faf92e98d3a2fb5f74e8062dec` |
