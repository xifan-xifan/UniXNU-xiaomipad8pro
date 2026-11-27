# v13.1y12u daily combination audit

**GO for combined RAM qualification.** Use the same candidate collection for the
four changes. Promote it through an ESP-only update after the combined checks
pass and the fresh owner Data backup has a recorded SHA-256 verified on
read-back. The installed v13.1y12q userland layer can carry forward unchanged.
The source review found one ESP argument-placement issue, fixed below.

## Interaction results

| Interaction | Verdict | Evidence |
| --- | --- | --- |
| Fixed timing, active scanout and Night Shift | **ours-OK** | PianoDpu captures the DSPP0/1 base before accepting the fixed-mode request. M3 timing, PCC and flips share the CTL transaction lock. Timing handover preserves VIG address and stride. Any timing writes invalidate WC epochs, then replay PCC after the new or restored timing verifies. Inherited 120 Hz verifies without a timing rewrite. PCC stages both DSPPs and applies one CTL flush. The timed host model forces PCC loss on a timing change and covers both successful handover and verified rollback. |
| Fixed 120/144 Hz and strict WC flips | **ours-OK** | NativeGopDisplay normalizes the linear plane layout and passes producer completion only for covered frames. The strict path checks effective page attributes and cleans the full wired backing before its first skip. It keeps a DSB on re-shows and reprograms VIG for a changed offset or stride. Missing handoff or a failed predicate takes the strict clean path. A failed required clean refuses the flip. Slot mapping retains page-table maintenance and TLB sync. The host test runs real flipper requests interleaved with PCC ramps at both rates. |
| NativeGopDisplay publication and display wake | **ours-OK** | GOP publishes one internal timing, fixed refresh intervals and absolute VBL deadlines from PianoDpu's verified rate. A verified rollback publishes the recovered rate. The 144 Hz request needs the explicit boot arg and a successful MM0 vote. With `piano-dpu-panel=0`, display-off suppresses PCC writes and wake queues PCC replay with the same reference while both timing engines keep running. |
| Write-back and post-mount concurrency | **ours-OK** | A bound read-write UFS store opens in single-gate mode with `data-store-concurrent=2`. Write-back caches eligible overwrites of already-present pages. First-touch data and bitmap publication use FUA. The mode sysctl fences admission and syncs a dirty epoch before allowing parallel requests. Flush failure leaves admission serial and latches a store error. The host test covers a resumed store, the serial-to-parallel handoff, failure and durability after a barrier and reboot. |
| Volatile RAM isolation | **ours-OK** | `data-persist=0` skips the UFS store open. NativeRamBlock leaves write-back off and the mode sysctl absent. The daemon exits without a mode write. The host model carries the requested disk flags into the volatile boot and verifies RAM writes, reboot isolation and zero store commands. |
| ESP boot-argument placement | **fixed** | The old split enlarged the loader prefix by 57 bytes and pushed `piano-usb-shell=1` past the legacy view. It also hid `piano-usb-net=1` in the diagnostic entry. The ESP splitter now puts Night Shift and write-back in the ADT with the two PianoDpu words. The installed USB words stay readable in the legacy view of both entries. The visibility check passes using the installed KC's 0066/0076 markers and requires no staged userland manifest. |
| Combined hardware behavior | **tablet-RAM-qual-required** | The published 120 Hz contract, M2 optical pass and Night Shift sweep come from separate runs. The Disk M5 power-cut and speed evidence covers a scratch store. Run the combined display and disk workload before promoting this daily profile. |

[Fixed modes and MM0](drivers/dpu-m3.md) · [Night Shift results](results-2026-11-27.md#unlocked-night-shift-ns-base) · [M2 optical qualification](drivers/dpu-m2-analysis.md#automated-optical-qualification-on-v131y12u-m2opt2) · [Disk M5 evidence](drivers/data-store-m4-m5.md)

## Exact daily profile delta

Add the following four keys once to the effective v13.1y12q line through the
ESP builder's `--arg` edits. The installed profile carries none of these keys.
Use the existing split layout. For the 144 Hz opt-in, replace
`piano-dpu-modes=120` with `piano-dpu-modes=144`.

```text
native-gop-display-night-shift=1 piano-dpu-clean-skip=1 piano-dpu-modes=120 data-store-write-back=1
```

Preserve `data-persist=2`, `data-store-concurrent=2`, the root and Data bindings,
and the existing seed. Leave adaptive `piano-dpu-refresh` unset. Keep the 80/75%
charge hold and `piano-power-b4pd=0`. The effective settings for
`piano-dpu-panel`, `piano-video-enc`, `piano-torch`, `piano-extra-ram`,
`piano-gpu-tier` and `data-store-queue` are all zero through their current
defaults. The kernel defaults remain opt-in.

The host replay read the installed ESP's `unixnu.toml` and ADT, checked them
against the build record and round-tripped the edited menu. The four words
add 100 bytes to the merged line, including separators. Both rates have the
same byte counts.

| Entry | Loader bytes | ADT tail bytes | Merged bytes | Legacy text bytes |
| --- | ---: | ---: | ---: | ---: |
| v13.1y12q daily | 816 | 2663 | 3480 | 1023 |
| v13.1y12u daily | 816 | 2763 | 3580 | 1023 |
| v13.1y12u diagnostic | 864 | 2763 | 3628 | 1021 |

The split stays within the default builder cap of 999 bytes, DarwinBoot's
1023 text bytes and the 4095-byte k27 0066 merge. All four words reach the
post-merge PE parser. Patch 0076 exposes the same merged text through
`kern.piano_bootargs`. The visibility guard reports the existing
brightness-sync advisory in both profiles. Brightness readiness still comes
from the driver properties. [Boot argument transport](boot-args-length.md)

## ESP-only install and qualification

The installed KC lacks the write-back argument and cached UFS write export.
The ESP update needs a rebuilt 0066/0076 collection carrying the current
PianoDpu, NativeGopDisplay, NativeRamBlock and NativeUfs. The existing
`piano-data-concurrent` LaunchDaemon and script already wait for APFS Data
and resumed store state 2 before writing `machdep.piano_data_concurrent=1`.
They use the same sysctl and status format with write-back enabled.

- Boot the combined 120 Hz candidate with Night Shift initially off so
  location 9 captures the neutral reference. Verify both INTFs and the GOP
  contract, then exercise optical off/warm/off, WC and fallback flips, and
  display blank/wake under load. Check for stale frames, underruns, fence
  failures and failed PCC replay. Qualify 144 Hz separately as the opt-in
  variant. App presentation at 120 fps is still an open measurement.
  [120 Hz cadence and presented-time evidence](drivers/m120-throughput.md)
- Include a volatile `data-persist=0` boot and a bound scratch
  `data-persist=2` boot with `data-store-concurrent=2`. On scratch, verify
  the post-mount mode switch, barrier and durable read-back during the
  display workload. Preserve the owner store until the verified backup and
  combined qualification gates have both cleared.
  [Disk M5 procedure and evidence](drivers/data-store-m4-m5.md)
