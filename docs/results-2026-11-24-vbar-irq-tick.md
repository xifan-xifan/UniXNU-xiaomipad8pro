# Real IRQ delivery on piano: VBAR fixed, first tick complete, death is a synchronous exception

Every image below was `fastboot boot` only (RAM), with `piano-reset-after` returning the
tablet to ABL fastboot; no partition was written and HyperOS came back on `_a` each time.

`arm_vm_init.c` pointed `VBAR_EL1` at `ARM_KERNEL_PROTECT_EXCEPTION_START`, a negative
address in the protected range. On piano that is not executable, so every exception entry
faulted again and the kernel looked like a silent hang (ufs251-252 reset at 86 s with no
handler entry). ufs253 writes `VBAR_EL1 = ExceptionVectorsBase` immediately before
`msr daifclr` and takes a `brk` at 37.2 s; ufs254/255 do the same and reach the IRQ and
`sleh_irq` probes at 37.6 s.

ufs256 keeps only that write, no probe at all, and returns at 38.6 s: the real PPI 27
interrupt is delivered and handled. Probes placed through the first tick all fire and add
their spin exactly once — ufs257 (after the `sleh_irq` prologue), ufs258 (`IAR == 27`),
ufs259 (after `rtclock_intr`), ufs260 (after the epilogue) each reset at 41.0 s, i.e. the
2 s probe plus the 38.6 s baseline.

After that first tick the kernel dies. ufs261 (spin at the fourth tick) and ufs262 (spin at
the second tick) both reset at 38.8 s, so the second tick never happens: the hand-written
PPI 27 branch in `sleh_irq` writes `CNTV_CTL_EL0 = 2` (IMASK set, ENABLE clear), which
switches the virtual timer off. XNU's own FIQ timer path (`sleh.c`, the `DBG_INTR_TYPE_TIMER`
branch) does not do that; it relies on `ml_set_decrementer()` writing `CNTV_TVAL_EL0` while
ENABLE stays set from `fiq_context_init()`. ufs264 removes the `CNTV_CTL_EL0` write and
counts ticks in `piano_dbg_kv(0x50, n)`; it is built (`boot-piano-ufs264cfg.img`, SHA-256
`d74fad3a4b8425bbc3fe929c776a985eaa2a69aadba30770cc7baf63b1e83a4a`) and not yet run.

ufs263 puts a 3 s spin at the entry of `sleh_synchronous` with the PSCI reset after the
second hit and resets at 44.9 s, about 6 s over the baseline: the death is one or two
synchronous exceptions, roughly 1.2 s after the first tick returns.

Read-back of that fault was attempted with the menu image after the ufs256 reset and failed
at the transport: `oem dump 9cb00000 700000` answered `FAILED (Status read failed (Too many
links))` while the reader was still enumerated as `piano-uefi`. The next read-back is the
8 KiB one: `sleh_synchronous` already records ESR/FAR/PC/x16/x17 in `piano_dbg_kv` slots
0x7b-0x7f, which live in the image's own `piano_stage_slots` table at PA 0x8c5941080, and
`tooling/native/run-ufs256-dump2.ps1` reboots to the bootloader first so the reader is
booted from a fresh USB session. ufs264 and that script are ready; the run is waiting for
the tablet to leave the `adb` `unauthorized` state HyperOS came back in.
