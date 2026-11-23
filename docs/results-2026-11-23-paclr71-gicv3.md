# paclr71: minimal GICv3 bring-up for the architected timer, 2026-11-23

`fiq_context_init()` enables `CNTV_CTL_EL0` but nothing on this platform
programs the interrupt controller. The k27 arm64 FIQ path itself is complete:
`locore.s` installs `fleh_fiq` in the vector table, `sleh_fiq()` classifies the
interrupt with `ml_get_timer_pending()` (which reads `CNTV_CTL_EL0.ISTATUS`)
and calls `rtclock_intr(TRUE)`. What was missing is the route from the timer's
PPI to that FIQ, which Apple silicon gets from ArmCpu and this kext set does
not have.

paclr71 adds `piano_gicv3_init()` to `osfmk/arm/arm_init.c` and calls it before
both `fiq_context_init(TRUE)` sites, so every CPU programs its own redistributor
when it starts. The distributor and redistributor addresses come from the
mainline `sm8750.dtsi` (`0x16000000`, 64 KiB, and `0x16080000`, 2 MiB,
redistributor stride `0x40000`), which matches the `arm,gic-v3`,
`redistributor-stride = 0x40000` node in the ADT captured from the running
device (`logs/adt/piano-k27-adt.txt`). The init maps both regions with
`ml_io_map`, sets `GICD_CTLR.ARE_NS` and `EnableGrp1NS` once, finds the
redistributor frame whose `GICR_TYPER` affinity matches `MPIDR_EL1`, wakes it,
puts PPI 27 in group 1 at priority `0xa0` with level trigger, enables it, and
sets `ICC_SRE_EL1`, `ICC_PMR_EL1=0xff`, `ICC_CTLR_EL1=0` and
`ICC_IGRPEN1_EL1=1`. A `piano-gic=0` boot argument skips the whole block for an
A/B control, and the function prints the distributor `GICD_CTLR` before and
after, plus the chosen frame, `GICR_WAKER`, `ICC_SRE_EL1` and
`ICC_IGRPEN1_EL1` per CPU.

`piano-paclr71/kernel.macho` (sha256
`e9f5bca43abc8263d53feacebc6429c2d4624364deef229bc40f2d281a9bc89e`, 28053304
bytes) links `_piano_gicv3_init`; `piano-ufs49.kc` was rebuilt from it with the
same 79 kexts, and `boot-piano-ufs49gic.img` (sha256
`085ed127b269a0a4681e8a0ab5cdb32d84522a7aca61f6aab92fea29ba9d3aa0`,
185733120 bytes) carries `io=0x20`, `piano-reset-after=90`,
`piano-reset-to=bootloader` and the `piano:cfg` probes. It inherits paclr70's
spin in `delay_for_interval()`, so a boot that still hangs in a delay will not
hide the GIC result.

The image has not been run: the tablet has been off USB since the manual
restart. `out/no-demura-control/sup-ufs49.ps1` polls for it and runs
`tooling/native/run-ufs49gic.ps1` unattended, which boots the image, waits for
the fastboot serial, stages `9cb00000` through `reader.img` as
`ufs49gic-console.bin`, dumps the panic zone, and leaves the tablet on HyperOS.
What to read first: a `piano-gic: distributor ctlr` line means the map and
distributor write succeeded; `piano-gic: no redistributor for mpidr` means the
typer match is wrong; and any `IOSleep`-dependent line that now completes
(after `piano-ring`/`piano-gate` stops) is evidence the timer interrupt is
live. If the GIC writes fault with an SError or panic, the next control is the
same kernel with `piano-gic=0`.
