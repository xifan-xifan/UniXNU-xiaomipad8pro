# The UFS root window read chain runs end to end; the k27 kernel still stops before bsd_init

Date: 2026-11-23. Device `7ccdd855`. Every run was `fastboot boot` only: no partition, GPT or userdata write.
Each cycle was unattended: the test image carries the Gunyah watchdog, `run-ufsNNmark.ps1` waits for the fastboot
serial to return, boots `reader.img` to stage the pstore console and panic zone, then reboots normally and waits for
HyperOS `OS3.0.309.0.WPYCNXM` with `sys.boot_completed=1`, which was reached after every run below without a manual
power cycle. The user's tablet had been factory-reset before this series, so no data was at risk.

## The DMA slot boundary is a contiguity limit, not a 2 MiB limit

Earlier notes said the first 2 MiB allocation hung. Per-image consoles show otherwise:

| Image | Result |
|---|---|
| ufs11 | 4K and 64K split buffers return; 512K never returns |
| ufs12 | 128 KiB returns, 133632 B (33 pages) hangs |
| ufs14 | repeated size probes themselves disturb the contiguous pages |
| ufs15 | 36 KiB descriptors and three 64 KiB data slots return; slot 4 hangs |
| ufs16 | one 64 KiB slot initialises, SMMU reads it back, then the first command stalls |
| ufs17 | instrumented ring path; no NOP completion line |
| ufs18 | NOP completes in one poll; reaches power mode query and device wake |
| ufs19 | logs the SCSI completion; stops right after `NativeUfs: wake sleep` |
| ufs20 | `hostSpinMs` built on `IODelay` still stops at the same point |
| ufs21 | direct `cntvct_el0` spin works; the whole root read chain completes |

A single 64 KiB data slot plus a 6 KiB descriptor block is reproducible; growing the slot count exhausts physically
contiguous pages after a few slots. The probe blocks that earlier images ran to size the region were themselves
consuming the pages they measured, so they were removed before ufs15 and are not part of the current layout.

## Kernel sleeps do not work before interrupts are enabled

ufs19 showed the sequence `stage device wake`, `nop done`, `query op=3 idn=0 rc=0`, `scsi lun=d0 op=1b rc=0 status=0`,
`NativeUfs: wake sleep`, and then nothing while `NativeGopDisplay`, `CoreAnalyticsHub` and `dlil_init` continued in
other threads. The START STOP UNIT command itself had already completed; the `IOSleep (50)` after it never returned.
ufs20 replaced that sleep with a loop of `IODelay (1000)` and still stopped at the same line, so both kernel delay
routes block here. ufs21 replaced it with a bounded loop reading `cntvct_el0` directly:

```
NativeUfs: wake sleep
NativeUfs: spin 50 ms ticks=1200000
NativeUfs: wake slept
```

1200000 ticks over 50 ms is 24 MHz, so the architected counter advances and only the timer-callout driven waits are
dead. This matches the earlier watchdog result (`docs/results-2026-11-23-paclr53-watchdog-return.md`): the
`piano-reset-after` `thread_call` never ran while the Gunyah bite did. Any remaining `IOSleep`/`IODelay` in the
pre-interrupt bring-up path must be replaced the same way; the ones after `hostEnableInterrupt ()` were left alone.

## NOP, query, wake and LUN ready all complete

ufs21 logs one line per command (`xfer end`, `query`, `scsi`) and every one of them completes in a single poll:

```
nop done rc=0 ocs=0 polls=1
query op=3 idn=0 rc=0 ocs=0
scsi lun=d0 op=1b rc=0 status=0
scsi lun=0 op=0 rc=0 status=0      (TEST UNIT READY, attempt 0)
```

## The root read chain completes and publishes both windows

After LUN ready ufs21 logs the stages in order, each backed by a completed command:

```
stage GPT header
stage GPT entries
stage root manifest
stage root window allocation/configuration
UFS window writable (development build)
stage root window attachment
stage signed window identities
UNVERIFIED development root: 2 windows published without owner policies
stage root publisher thread
```

This is the first run in this series where the UFS block layer produced media windows at all. The windows are
built without owner policies (`NATIVE_UFS_UNVERIFIED_ROOT`), which is a development configuration, not a product one.

## What still blocks a desktop

The console has no `bsd_init`, no `launchd` and no APFS container start. The kernel's last lines are the
`dlil_init` messages, so the run ends before BSD init hands over to userland, inside the ~80 s Gunyah watchdog. The
newly created publisher thread waits for IOBSD (`waitForMatchingService`, 60 s budget) before publishing media, and
that match only happens later in boot, so the two blockers are ordered: the kernel must pass `dlil_init` and reach
`bsd_init` first. The next diagnostic is therefore kernel-side timer interrupts (arch timer routed through the GICv3
that `ArmCpu` drives), not more UFS work.

## Images

All under `out/no-demura-control/`, `fastboot boot` only, built on the Mac from the pinned k27 tree with the
NativeUfs object rebuilt from `package/NativeUfs`:

| Image | SHA-256 |
|---|---|
| `boot-piano-ufs18mark.img` | `fcc19758c3802986dcf3af1938f5ee9adcde2ebddce20cee97f41f88d26ba78c` |
| `boot-piano-ufs19mark.img` | `e0aa46c73cdefe1ea6adff28566999e98c111da1eb0c09d9f5d06b5af740f1fd` |
| `boot-piano-ufs20mark.img` | `c9615fe1b13a2c6c392670b73e5c25ca484ba71802d221f3d4b4fe288594ad2f` |
| `boot-piano-ufs21mark.img` | `503c3151616c0bf58f4684d4886c5060c0019c08a50dcb306777e6ec7d830611` |

Acceptance is unchanged and not met: no desktop, no macOS userland, no Android-to-macOS switch, no Data persistence.
