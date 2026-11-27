# M120 offline diagnostics

`grade.py` reads retained console lines, `machdep.native_gop_notifications` JSON snapshots and versioned diagnostic records from one boot. Pass files in capture order. The report keeps notification deltas, pacing windows and cumulative pipe rollup deltas separate.

```sh
python3 tooling/native/m120-diag/grade.py --details "$CONSOLE" "$NOTIFICATIONS_BEFORE" "$NOTIFICATIONS_AFTER"
python3 scripts/ci/self-tests.py --only m120-diag
```

The grader returns 0 for recognized evidence, 1 for malformed records or inconsistent ticket outcomes and 2 when evidence is unavailable or input cannot be read. The first cumulative report serves as a baseline. Decreasing counters and non-advancing notification clocks break the corresponding delta sequence. Frame-info audit reasons can overlap. Historical aggregate records leave the raw admission and physical-counter fields unavailable. The fix-image decision is always HOLD.

## Record contract

Each future raw record is a JSON object on one line with `"m120_diag":1` first and a `kind` of `pipe`, `flush` or `irq`. Counters and masks are unsigned 32-bit integers. Observation clocks, worker serials and generations are unsigned 64-bit integers. Use continuous host ticks throughout the raw records and use nonzero, boot-relative swap IDs, worker serials and generations. Zero denotes an unavailable identity or clock and is a valid value for hardware counters and GPU sequences. One boot is the scope of all identity joins.

| Kind | Required fields |
|---|---|
| `pipe` | `swap worker preparation take ticket failure key_matches prepare program` |
| `flush` | `generation worker selected_irq armed enabled flushed latched disabled disarmed end mask latch_ctl frame1 line1 frame2 line2` |
| `irq` | `generation armed_flush ticks status ctl before1 after1 line1 before2 after2 line2` |

Both queue samples carry `ticks current created pending head layers ready drops main_surface`. The first is the most recent preparation before the observed take. The second is immediately before promoting the programmed head, after filtering. Preserve the raw request masks before cursor masking. Record an unavailable sample with zero fields. A head of zero in an evaluated sample means empty. `main_surface` and `key_matches` are 0 or 1. Key match is the result of the existing full key comparison before take, with only that boolean retained.

| Enum | Values in numeric order |
|---|---|
| `preparation` | Unobserved, power off, evaluated |
| `take` | Unobserved, used, fallback |
| `ticket` | Empty, pending, ready, failed |
| `failure` | Unknown, capture query, poll query, deadline |

The optional pipe fields `main_fence_signal_ticks` and `complete_ticks` record observed family signal and completion clocks for the same swap. The optional `fence` object accepts `capture_target program_target submitted retired level cap last_pending_ticks first_retired_ticks query_begin_ticks query_end_ticks`. Omit unobserved fields. The sequences are global watermarks, and the time between the last pending and first retired query is a CPU observation interval. The optional `feedback` object has a `disposition` of 0 (unobserved), 1 (suppressed), 2 (send accepted) or 3 (send error), with optional `mask send_result presentation_ticks` copied from the actual packet or send result.

The flush tuple carries both INTF frame and line readings from before the flush. Its clocks delimit arm, enable, post-barrier flush, worker CTL readback, disable, disarm and qualifier end. `latch_ctl` is the worker's raw CTL readback at `latched`. The optional `qualifier_reasons` is the actual audit bitmask when available. An IRQ row carries the arm generation it observed, physical status, observation time, CTL readback and the frame-before/frame-after and line values for each INTF. Preserve unstable raw IRQ readings. `selected_irq` identifies the exact IRQ stamp used by the qualifier, or zero when unavailable. Extra IRQ candidates remain visible in `--details` and are never substituted for the selected stamp.

The raw report partitions observed takes into buckets and counts independent head rejection fields. It compares both physical counter pairs modulo 2³² and independently flags IRQ status, arm mismatches, clock bounds and CTL bits. The swap join requires a unique matching worker serial. CA page ownership and selector timelines, surface-specific GPU execution and IOMFB/CA/SkyLight/app feedback require separate observations.

## Opt-in boundary

`M120Diagnostics.h` is a portable record and counter design for later review. This branch predates the two production feature paths and ships the header, grader and tests without driver wiring. The existing opt-in `native-gop-frameinfo-audit` value must be exactly 1 and the relevant feature path must already be active for a prospective emitter to collect records. Gate sampling at its call site, before reading clocks, queues, hardware or counters. The header performs no I/O and returns observations only. Record capture must use the existing serialization of the gate, worker and IRQ paths. Keep addresses, surface contents and client identifiers outside the record. This format defines no additional boot arguments, work, timers, queries or publication policy.
