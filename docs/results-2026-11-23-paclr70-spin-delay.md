# paclr70 spin delay and the dead-timer root cause, 2026-11-23

The ufs45/ufs46 consoles show the IOResources configuration thread parked with
`ConfigRunning|busy=1` while `IOSleep` never returns. The k27 tree explains both
the wait and the absence of a wake: `fiq_context_init()` in
`osfmk/arm64/machine_routines.c` only calls `_enable_virtual_timer()`, which
writes `CNTV_CTL_EL0.ENABLE`; it does not program any interrupt controller, and
the 79-kext collection kept for these runs contains no GIC or ArmCpu kext, so
the timer PPI is never routed to FIQ. `machine_routines.c` also has no FIQ
install path on arm64 beyond that stub. The timebase counter itself runs, which
matches the earlier observation that `cntvct_el0` spins advance while every
`assert_wait_deadline` based delay hangs.

paclr70 therefore makes `delay_for_interval()` and
`delay_for_interval_with_leeway()` spin on `mach_absolute_time()` until the
deadline instead of `_clock_delay_until_deadline()`. The rebuilt kernel
`piano-paclr70/kernel.macho` (sha256
`ca8c4ae8243e9f0dc5a998a7edec491eafb1740d01fe94e4d1bdeacf39004172`, 28053096
bytes) was checked by disassembly: `_delay_for_interval` now loops
`bl _mach_absolute_time; cmp; b.lo` and no longer calls
`_clock_delay_until_deadline`. `piano-ufs48.kc` was rebuilt against it with the
same 79-kext set and `boot-piano-ufs48spin.img` (sha256
`03ee3301103190c98a0bd36a9a8eecfb34980f5f65f77a298f28b5b659b13fa2`,
185733120 bytes) carries the same `io=0x20`, `piano-reset-after=90`,
`piano-reset-to=bootloader` arguments and the `piano:cfg` probe points as
ufs47cfg. It is a RAM-only `fastboot boot` image.

The image is built and verified but not yet run: the tablet has not enumerated
on USB since the manual restart (`adb devices`, `fastboot devices` and
`pnputil /enum-devices /connected` show no `VID_2717`, `VID_18D1` or
`VID_05C6` device), and `out/no-demura-control/sup-ufs48.ps1` is polling for it
so the run starts without any further user action. A matching `ufs48spin` run
script exists at `tooling/native/run-ufs48spin.ps1`.

Two follow-ups are queued for whatever console this returns: if
`piano:cfg` stops between `probe begin` and `probe rc`, the spin change did not
fix that candidate and the probe indices identify it; if the thread now reaches
`leave`, the IOResources guard is done and the next check is IOMedia and
`boot-uuid`. The AMFI thread's `wait resource AKSKernelServices` remains a
separate, expected wait for a SEP service that does not exist here; the kext
exports an `amfi_get_out_of_my_way` boot argument that is a candidate for a
later image, but it is not part of this one so the comparison stays controlled.
