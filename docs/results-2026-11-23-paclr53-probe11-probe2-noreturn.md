# paclr53 probe 11 and probe 2: no unattended return, and what the older return really was

Date: 2026-11-23. Device `7ccdd855`. Only `fastboot boot` was used; no partition, GPT or userdata write.

## What ran

Both images carry the same payload as the rooted image (`kernel.macho` `4efc1dea…`, trustcache `002db2e6…`, ADT `4e904d90…`), so the kernel and the PSCI/ADT view are identical to the run that returned earlier the same day.

| image | SHA-256 | args of interest | result |
| --- | --- | --- | --- |
| `boot-piano-paclr53-probe11.img` | `723e871a…` | `native-ufs-probe=11 piano-reset-after=240 piano-reset-to=bootloader` | ABL preflight all good, `fastboot boot` OKAY, then no fastboot and no ADB for more than five minutes |
| `boot-piano-paclr53-probe2short.img` | `f6a8f7c1…` | `native-ufs-probe=2 piano-reset-after=45 piano-reset-to=bootloader` | no fastboot within 150 s |

The tablet stayed dark for both. The user power-cycled it by hand; Android then came up on slot `_a` with uptime 33.74 s and `ro.boot.bootreason=bootloader`. No pstore console was captured, because nothing returned far enough to be read.

Probe 11 already panics by design after `hostBringUp()` and `findRootLayout()`. A panic that resets the tablet was expected to end that run on its own even without the timer. It did not, so the run never reached the `panic()` call, or the panic path itself does not reset this configuration.

## The earlier return was not the timer

`paclr53-fips4-live.log` (start 04:28:28, end 04:31:27) shows the fastboot serial again before any `waiting for 7ccdd855` line, so the device came back in less than the 15 s poll interval after `fastboot boot` returned. Its effective entry is `macos-8cpu`, whose arguments end in `sheng-smp=1 piano-reset-after=90`. A 90 s timer cannot explain a return inside 15 s of the payload starting.

So the one paclr53 run that "returned unattended" most likely ended in a panic reset. That means `piano-reset-after` has never been observed to fire on this kernel. The earlier note in `HANDOFF.md` that the pthread run "returned" is not evidence for 0056.

## Why the timer may be silent

`patches/k27/0056-…patch` arms the timer in `STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, piano_reset_init)`. It returns without printing anything when

```
!PE_parse_boot_argn("piano-reset-after", &secs, sizeof(secs)) || secs == 0 || ml_psci_conduit != ML_PSCI_CONDUIT_SMC
```

`paclr53.kc` contains both gate strings, including `piano-boot-guard: PSCI is not through SMC (ADT /psci): native only, the guard stays off`, and the DTB found in the same image has a `psci` node (`smc` 4, `method` 3, `hvc` 0), so the conduit is probably SMC and the gate is probably not the cause. The remaining candidate is the scheduling itself: `thread_call_enter_delayed` is called from an EARLY_BOOT startup routine, and if that call does not take effect the timer never fires while the immediate tick is never re-entered.

The argument line is not the cause at the DarwinBoot boundary: probe 11 is 729/734 bytes and probe 2 is 693/698, well under the 1023-byte `boot_args.CommandLine` and under the 1000-byte limit recorded for DarwinBoot.

## Prepared next controls

Both use the short argument line that the returning fips4 run used, so the timer test does not carry the DroidVM arguments at all.

| image | SHA-256 | args |
| --- | --- | --- |
| `boot-piano-paclr53-timercheck.img` | `678f6729f9d2e6c8a9c59d58158cd99aa0ab7624b19de74da2553487fdca26f4` | `-v fips_mode=4 native-ufs-probe=2 data-persist=0 pstore-console=0x9cb00000 … piano-reset-after=20 piano-reset-to=bootloader` |
| `boot-piano-paclr53-rootshort.img` | `a324ebe1e17b8aa7b219757f844934bd229b10890aa7cc96eaf9abb12089c46f` | the same short line with `native-ufs-root=1 native-ufs-root-part=linux_rootfs native-ufs-diagnostics=1` |

`run-paclr53-timercheck.ps1` measures the seconds between `fastboot boot` and the fastboot serial reappearing, then boots `reader.img` and stages `paclr53-timercheck-console.bin` and `-panic.bin`. A return near 20 s proves 0056 works and only the long-argument images are affected. A return near 40–60 s with the panic zone populated says the watchdog bit, not the timer. No return at all says the timer is dead on this kernel and the return path has to come from the UEFI side (`docs/results-2026-11-21-watchdog-post-ebs.md`, FFS `6563739a…`).

## Limits

Nothing here is a diagnosis of the root-mode hang. Both measured runs produced no console. The pstore zone at `0x9cb00000` survives a reset, so the next run must return before it can say anything.
