# ufs111-126: PPI 27 delivery, the rtclock entry and cooperative tickers, 2026-11-23

This round moved the k27 boot from "the timer interrupt never returns" to
"the UFS root windows are published and the AMFI thread runs". Every run was
`fastboot boot` only; each image reset itself and returned unattended to
completed HyperOS `OS3.0.309.0.WPYCNXM` A.

What the GIC and the architected timer actually look like:

- The GICR has eight accessible redistributor frames at
  `0x16080000 + n * 0x200000` (stride `0x40000`), one per core. Touching a
  ninth frame hangs the core, so the sweeps stay at eight.
- With DAIF masked, letting CNTV expire raises PPI 27 in frame 0
  (`hp 1b ctl 5 pndmask 1`): routing from the timer into the distributor is
  correct. Polling `ICC_IAR1_EL1` from the ticker returns `0x1b`, EOI clears it,
  and the next deadline re-raises it (ufs118).
- Unmasking DAIF in the ticker kills the console (ufs115/116) before any
  handler sample prints. Polling and calling `rtclock_intr(0)` by hand works
  instead, so the ticker keeps DAIF masked and drives the interrupt itself.

`rtclock_intr` itself was the next stop (ufs118-121). With per-statement
markers the console shows the whole entry completing:

```
s121 rt enter / cdp / stat ok / sched stat ok / assert ok / abstime
s121 rt stall begin/end, rt timer begin, at enter/queue/running/resync, rt timer end
```

ufs120 proved `unixnu_dwc3_poll()` is not the blocker. ufs121 proved the entry
stops at `assert(!ml_get_interrupts_enabled())`, i.e. the ticker ran with DAIF
unmasked. ufs122 masks DAIF inside the tick loop and the full `rtclock_intr`,
`timer_intr`, timer queue expire and resync sequence then runs 132k times in
240 s without a fault (`abstime 885f6a2 ...`, 231000 ticker beats).

The ticker thread still starved everything else. Two variants failed:

- ufs123 returned from the kernel thread: the console stops right after
  `s123 ticker yields to idle ticks`, so a kernel thread may not return.
- ufs124/125 blocked it (`assert_wait` + `thread_block`) and instrumented
  `machine_idle()`: the idle path is never entered and the ticker never
  resumes, because another thread spins in the kernel `delay` path
  (`clock.c` was patched to spin on the timebase when callouts never fire).

ufs126 keeps the spinning ticker but adds
`thread_block_reason(THREAD_CONTINUE_NULL, NULL, AST_YIELD)` after every beat.
That lets higher-priority runnable threads run, and the boot moves far past
the previous stop:

```
NativeUfs: scsi lun=d0 op=1b rc=0 status=0
NativeUfs: stage GPT header / GPT entries / root manifest
NativeUfs: UNVERIFIED development root: 2 windows published without owner policies
NativeUfs: stage root publisher thread
mac101: initbsd AMFI thread run thread=0xfffffff7107d2e70
iokit: wait resource AKSKernelServices state 0x0/0x80000001 published 0
piano-reset: s118 tick 1 iar 1b irq 1 now 7554402031
```

The run then stalls at the AKSKernelServices resource wait; the ticker prints
one beat and is not rescheduled while a busy thread owns the CPU, so no timer
fires and the Gunyah watchdog resets at ~85 s. The next step is to keep the
tick alive from inside the spinning kernel waits (or repair real IRQ delivery),
so the wait's deadline can expire.

Artifacts in `out/no-demura-control/` (not committed):

| round | kernel sha256 (prefix) | image sha256 (prefix) |
| --- | --- | --- |
| ufs118 | `6d5d6f55` | `296bbcf0` |
| ufs119 | `24830e0c` | `0e8db79f` |
| ufs120 | `f5820cd5` | `303028e7` |
| ufs121 | `b2846d75` | `6ebd0173` |
| ufs122 | `fac6e04a` | `6425d60d` |
| ufs123 | `b12e35b3` | `0bccce1c` |
| ufs124 | `a999cf30` | `3d610c9a` |
| ufs125 | `b53814d7` | `1d0df1c7` |
| ufs126 | `70cd4a89` | `bc65ed56` |