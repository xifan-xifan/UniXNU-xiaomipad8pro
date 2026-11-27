# ufs104-110: ticker scheduling, the GIC group-1 window and the masked timer, 2026-11-23

ufs104-105 ran the AMFI worker with `thread_block(THREAD_CONTINUE_NULL)` in the
ticker so the boot thread could run.  Both consoles show the ticker reaching
`piano-reset: spin` beats while the AMFI worker stays silent: the zone callback
returns immediately with a still-held run-lock, so the block never reaches a
wakeup.  ufs105 also proved that `piano_gicv3_init` (GICD group enable, GICC
PMR/EOImode) still leaves `HPPIR1 = 0x3ff` and PPI 27 never fires.

ufs106-108 instrumented the GIC and the timer from inside the ticker and moved
the reset window to 240 s so the reader dumps a full picture:

- `pg: ctlr d3 ds 1 mode irq`, `grp ffffffff`, `igrpen1 1`, `sre 7` and
  `hppir 3ff` at init time: the distributor and the CPU interface are enabled
  in group 1.
- At boot `HPPIR1` briefly reads `0x1b` (PPI 27 pending) before the timer is
  masked again, so the routing from the timer to the distributor works.
- `NativeUfs: stage root publisher thread` is reached, `piano:cfg probe`
  succeeds 23 times and `NativeRamBlock` starts its window.

ufs109 replaced the 3 s `IOSleep` in front of the media walk with a spin and
lost the kext threads after `media walk begin`; ufs110 restored the ticker and
armed the architected timer itself (`CNTV_TVAL_EL0 = CNTFRQ/8`, `CNTV_CTL_EL0 = 1`)
right after entering the ticker.  Console (`out/no-demura-control/ufs110-console.txt`):

```
piano-reset: cntfrq 19200000 hppir1 3ff vbar fffffff00c913000 ih 0x53eed6700bbfbe78 ctl 7 cval 2a52180a
piano-reset: hppir1[0..9] 3ff ctl 1
piano-reset: daif before 200 / after 200
piano-reset: spin 27/57/.../237 s
piano-reset-after: 240 s reached, PSCI SYSTEM_RESET
```

`ctl 7` means ENABLE|IMASK|ISTATUS: the timer had already asserted while masked.
After writing `CNTV_CTL_EL0 = 1` the register reads 1 for all ten samples and
`HPPIR1` stays at 3ff.  The ten samples cover 100 ms in total while the deadline
was `CNTFRQ/8` = 125 ms, so the run ended before the programmed deadline could
expire; ufs111 repeats the experiment with `CNTFRQ/16` and twenty samples.

Artifacts, ufs110: kernel sha256
`43b564759ac9a7bdc6053668dc8ffed78ba5b851fe91478cad7786dbb7cde1ca` (28056008 B),
`piano-ufs110.kc` 0x8c48000 bytes / 364797 chained pointers, image
`boot-piano-ufs110cfg.img` sha256
`d356d1015a2317a8a781f80f8dfc34a2713c74af0148809ebc6deea047051801`.

Every run used `fastboot boot` only.  No partition, GPT, boot image or userdata
write was issued; each run reset itself through PSCI and returned unattended to
completed HyperOS `OS3.0.309.0.WPYCNXM`.
