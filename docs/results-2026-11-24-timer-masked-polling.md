# Masked GIC polling gives the delay path a working timer (ufs204-ufs212)

All runs are `fastboot boot` only. The tablet returned to HyperOS
`OS3.0.309.0.WPYCNXM` with `sys.boot_completed=1` after every run below.

## What changed

`delay_for_interval*()` on this port spins on `mach_absolute_time()`. The
architected counter runs, but nothing in the 79-kext set delivers PPI 27 to
XNU, so timer callouts, `thread_call` deadlines and wakeups never advance while
a thread spins. `piano_delay_timer_service()`, called from that spin, now arms
`CNTV` once, polls `CNTV_CTL_EL0.ISTATUS`, acknowledges the interrupt through
the CPU interface, and calls `rtclock_intr(0)` with IRQs masked. XNU processes
the timer queue itself; no real exception is taken.

| build | kernel | image | what it proved |
|---|---|---|---|
| paclr164 / ufs206 | `8ada2301...` | `bbb3faa7...` | first masked-polling service hung inside the service |
| paclr165 / ufs207 | `445c989a...` | `c3663e68...` | stage tracing: `ack enter ctl 7`, `iar 3ff`, then silence |
| paclr166 / ufs208 | `0536e095...` | `cdaf52a3...` | `pl: it 1`, `pit: svc 1` then no `it 2`: hang inside call 1 |
| paclr167 / ufs209 | `6cfa5682...` | `706551fc...` | `tail1`, `tail2` printed; `tail3` never: the DAIF write hangs |
| paclr168 / ufs210 | `c788e5f9...` | `52f5e277...` | timers work: `iar 1b`, `rtclock_intr` returns, 773 GIC acks, media walk completes |
| paclr169 / ufs211 | `a96c56e1...` | `9952514c...` | same without tracing; stops in the 30 s UUID wait |
| paclr170 / ufs212 | `6d1b899b...` | `25c25225...` | 1 s wait with `startDeferredMatches()` still never returns |

## Findings that carry forward

- `mrs daif, x` -> `msr daif, x` restores are unsafe here: the saved value did
  not survive the in-service prints, so restoring wrote 0 and unmasked IRQ,
  which parked the boot. The service now ends with `msr daifset, #3` and never
  re-enables interrupts.
- Writing `CNTV_CTL_EL0`/`CNTV_TVAL_EL0` is safe while IRQ/FIQ stay masked; the
  earlier hangs (paclr156-158, 162) came from doing it with the exception
  unmasked.
- `piano_gicv3_init()` leaves `ICC_IGRPEN1_EL1` set and installs
  `piano_gic_irq_handler`, but nothing in the tree ever clears DAIF.I or F, so
  no real interrupt is delivered. Polling works only while a thread spins in
  `delay_for_interval*()`.
- Because of that, any `thread_block` with a deadline hangs: ufs212 bounds the
  media wait at one second and never sees it return. `waitForMatchingService`
  cannot complete until real interrupt delivery works, which is the FIQ route
  described in `docs/results-2026-11-23-paclr71-gicv3.md` (`piano-gic-irq=0`).
- The UUID the wait asks for (`84581E99-...`) is not published by NativeUfs,
  whose root is the `linux_rootfs` window, so even a delivered timeout only
  reaches `Wait for root failed`.

## Next

1. Route PPI 27 as Group 0 (FIQ), unmask DAIF.F in the same place the port
   unmasks DAIF.I, and check that `piano_gic_irqs` advances without any thread
   spinning. This is the prerequisite for `thread_block` timeouts, launchd and
   the idle loop.
2. Only then re-run the root wait; a resolved root is needed before desktop,
   Data persistence or switching acceptance can start.