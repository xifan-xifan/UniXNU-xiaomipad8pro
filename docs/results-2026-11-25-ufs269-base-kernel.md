# The 27.3 candidate's base kernel carries no timer or GIC path (2026-11-25)

Read-only audit of the image that `tooling/native/run-ufs269pub.ps1` is queued to boot. No device action was taken.

## What the candidate is actually made of

`tooling/native/build-ufs269-upstream-pub.sh` replays the recorded 2026-11-22 link command, so its kernel and Apple base
are that run's bytes:

| input | bytes | SHA-256 |
| --- | ---: | --- |
| `out/no-demura-control/blendfix.macho` (kernel, built 2026-11-22) | 28,034,000 | `459904b314ac1228879562c089c8fa35f315ca2a554bc8adac71c6beb6829173` |
| `out/upstream-pub-20261125/piano-ufs269.kc` | 135,806,976 | `dafd48f127783c3348439457a76f7bb5a2daa1c3d298a49a398391ebf9426ab1` |
| `out/k27-smprobe/piano-ufs266.kc` (the 147 MB probe lineage) | 147,095,552 | `9f276dff4a35ef6ed89ffd2c8a535d3742ed4f843623b744f310c8fa68aca64b` |

A byte scan for the port's own symbols (`rg -a -o -e piano_gicv3_init -e piano_delay_timer_service -e piano_gic_irq_handler
-e piano-gic`) finds **zero** occurrences in `blendfix.macho` and its linked collection, and finds all of them plus
`_piano_stage_slots` and `piano_dbg_kv` in `piano-ufs266.kc`. `blendfix.macho` is not an empty kernel: it carries
`piano_boot_guard*`, `piano_cpuidle*`, `piano_boot_args*` and friends, so the scan is not a false negative on the naming
scheme, only on the interrupt path. The 11-22 kernel predates the GIC work, which started 2026-11-23 with paclr71.

## Why that settles nothing about the driver

The upstream publication sequence the new object implements is wait-based, end to end:

- `package/NativeUfs/NativeUfs.cpp:2904` blocks on `IOService::waitForMatchingService(matching, 60 s)` for the IOBSD
  resource before publishing any window;
- `package/NativeUfs/NativeUfs.cpp:2927-2946` polls `mediaWithUuidExists` with `IOSleep(100)` up to 300 times, and the
  unverified-root variant at line 2930 loops on `getBusyState()` with the same `IOSleep(100)`.

Two recorded facts make both of those dead on this base: `docs/results-2026-11-23-native-ufs-root-window.md` records that
kernel `IOSleep`/`IODelay` never return before interrupts are enabled, and `docs/results-2026-11-24-timer-masked-polling.md`
records that only the newer kernels (paclr168/169, run as ufs210/211) have a timer path that advances with IRQs masked, and
that real delivery (PPI 27 routed to Group 0 plus unmasking DAIF.F) is still needed before any `thread_block` deadline can
return. `blendfix.macho` has neither the masked-polling service nor the GIC init.

So the sentence "the driver is the only variable" in `docs/results-2026-11-25-upstream-publication-build.md` holds for the
collection inputs and not for boot behaviour: this candidate would park in its first wait and would read as a publication
failure. Do not spend a device slot on it as built.

## The corrected next candidate

1. Link the same `out/native-kexts/NativeUfs-upstream/NativeUfs.o` through the lineage that does have the interrupt code:
   `tooling/kc/build-kc-k27.sh` takes the native kext objects (`--native`), which is how the 147 MB collections that reached
   `bsd_init` and the media walk were rebuilt (ufs193 onward).
2. Route PPI 27 to Group 0 and let the port unmask DAIF.F where it unmasks DAIF.I. The Group-0 side already exists in the
   patch generator (`tooling/native/mkpaclr117-full.py:2207` clears the redistributor `IGROUPR0` bit, line 2247 sets
   `ICC_IGRPEN0_EL1`); what is missing per `docs/results-2026-11-24-timer-masked-polling.md` is the unmask. Without it every
   wait still blocks forever.
3. Keep the root identity in view while doing so: the wait target is the APFS *volume* UUID
   `84581E99-85BE-4957-AC53-2947E887B3E4` (`configs/k273-adt.toml` `root_match_volume_uuid`), not the container UUID
   (`A7672058-...` for the OS window), so the window has to survive long enough for `IOAPFSFamily` to scan it before the
   root wait can be satisfied.

## Device state while this is open

ABL still refuses every RAM image. Both host builds were tried against the same serial this session, against the 8 MiB
`out/no-demura-control/reader-fat.img`:

```text
fastboot 37.0.1-15733141  Sending 'boot.img' OKAY  ->  Booting FAILED (remote: 'Failed to load/authenticate boot image: Load Error')
fastboot 33.0.3-8952118   Sending 'boot.img' OKAY  ->  Booting FAILED (remote: 'Failed to load/authenticate boot image: Load Error')
```

so this is not the platform-tools version issue recorded for `boot-ram-maintenance.img` in
`docs/results-2026-11-24-ufs264.md`. The release condition stays one cold power-off at the tablet
(`docs/results-2026-11-25-usbc-console-try.md`); nothing on the host clears it. No partition, GPT, boot image or userdata
write was made.
