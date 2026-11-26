# Piano frame-time probe

`piano-frametime` opens a visible AppKit window on the Xiaomi panel. Default selection prefers the driver's published vendor `0x61a9`, product `0x8750`, with guarded matching for a synthesized CoreGraphics identity (below). A screen-bound `CADisplayLink` drives a depth-tested Metal scene of rotating cubes in a display-synchronized `CAMetalLayer`. The window covers three quarters of the usable screen in each dimension. `--load` sets the number of fragment shader iterations per covered pixel. Use `--load 0` for the light composition test and record the load and drawable dimensions for 3D comparisons. The probe requests a preferred frame rate. Select and settle the display mode separately.

## Build and run

Build the arm64 executable with the 27.3 macOS SDK selected through Xcode's SDK path. Set `DEVELOPER_DIR` for the Xcode installation that contains it, or pass the absolute SDK directory as `MACOS_SDK`.

```sh
make -C tooling/native/piano-frametime
make -C tooling/native/piano-frametime test
```

The binary is `out/piano-frametime/piano-frametime`. For a host compile with another installed SDK, use `make -C tooling/native/piano-frametime SDK=macosx all`. `test` runs the portable cadence analysis and panel-match predicates under ASan and UBSan. `--main-display` explicitly selects the main display, including when the panel's default identity match is unavailable.

Run the animation **as the logged-in console user inside the GUI session**, with the screen awake and unlocked and the window unobscured. An SSH login by itself may lack the console GUI bootstrap context. For a RAM qual spec, stage the executable at `/usr/local/libexec/piano-frametime` in the test payload or transfer it through the runner's existing staging path. The installed-system qual can use the same binary. Run the existing desktop gate first. For M3, verify that the active fixed mode is 60 Hz with the CoreGraphics probe in `docs/drivers/dpu-m3.md` before starting the measurement. Use one result set per mode and load. Allow at least 90 seconds per invocation in the spec budget, plus time for staging, desktop login, mode changes, and evidence collection. The defaults are 60 fps, 65 seconds and load 0. The minimum requested duration is 60 seconds, with a separate three-second warmup and two-second drain.

This example is the 120 Hz block of a qual spec. It assumes the runner's `rssh` and `rsudo` helpers, a host result directory in `$RESULTS`, the staged executable, and an M3 boot with `piano-dpu-snapshot=1 piano-dpu-cadence=2`. Run it after the GUI and mode gates. The two root reads and kernel log are collected alongside the app output. The GUI subprocess inherits the console bootstrap context and runs with the console user's credentials. The second read and log collection run even when the app returns a failure.

```sh
prefix="$RESULTS/frametime-120"
snapshot_rc=0
capture_rc=0
log_rc=0
log_start=$(rssh '/bin/date "+%Y-%m-%d %H:%M:%S"') || return 1
rsudo '/usr/sbin/sysctl -n machdep.piano_dpu_snapshot' > "$prefix.before" \
    2> "$prefix.before.stderr" || snapshot_rc=1

rsudo '
    u=$(/usr/bin/stat -f %u /dev/console)
    test "$u" -gt 0 || exit 1
    exec /bin/launchctl asuser "$u" /usr/bin/sudo -n -u "#$u" -- \
        /usr/local/libexec/piano-frametime --fps 120 --seconds 65 --load 0
' > "$prefix.json" 2> "$prefix.summary" || capture_rc=$?

rsudo '/usr/sbin/sysctl -n machdep.piano_dpu_snapshot' > "$prefix.after" \
    2> "$prefix.after.stderr" || snapshot_rc=1
rsudo "/usr/bin/log show --style syslog --start '$log_start' --predicate \
    'process == \"kernel\" AND (eventMessage CONTAINS \"PianoDpu: cadence \" OR \
    eventMessage CONTAINS \"PianoDpu: flips: \" OR eventMessage CONTAINS \"NativeGopDisplay: period \")'" \
    > "$prefix.kernel.log" 2> "$prefix.kernel.stderr" || log_rc=$?

out/piano-frametime/piano-frametime --kernel-report "$prefix.before" "$prefix.after" "$prefix.kernel.log" \
    > "$prefix.kernel.json" 2> "$prefix.kernel.summary" || snapshot_rc=1
test "$capture_rc" -eq 0 && test "$snapshot_rc" -eq 0 && test "$log_rc" -eq 0
```

On a profile without the optional snapshot or with driver IOLog lines delivered only to the console, keep the app output and record the missing evidence. The runner can pass its filtered console capture to `--kernel-report` in place of the unified log. The kernel report exits 0 when both hardware counters advance and the supplied log, if any, contains a positive flip cadence window. Confirm which windows fall inside the run and apply the performance criteria to the saved results.

## Display selection and zero presentation times

Run `piano-frametime --list-displays` in the console GUI session for read-only numeric CG identities, builtin/main flags, pixel dimensions, filtered NativeGopDisplay attributes and `panel_match_available`. It opens no animation window and records no serials or arbitrary display names. Capture JSON also records the selected `display`; a selection failure prints numeric candidates.

The default matcher first prefers CG `0x61a9/0x8750`. A fallback requires the driver's fixed panel UUID and canonical ProductAttributes plus matching native pixel dimensions. It then accepts either the fixed public product name on both sides or a builtin driver/display with CG's conditional Apple/unknown identity (`0x610/0x756e6b6e`). Tied best matches fail. The fake second display is excluded from the driver fallback. This conditional identity was traced in 27.3; the supplied y12l run did not record its actual CG IDs, so the new matcher still needs tablet validation.

In the installed v13.1y12l baseline, every presented handler ran but every `presentedTime` was zero. CA has zero-time release/cleanup paths that invoke these handlers without setting a positive presentation time. Preserve the invalid capture: GPU completion and callback arrival cannot replace actual presentation timestamps. See [the presentation-feedback trace and HOLD gate](../drivers/presented-time.md) for the source gaps and read-only collection list.

## Reading the report

Standard output is one JSON document. Standard error has the run summary and presented interval histogram. Each row in `frames` follows `frame_columns`. The timestamps are in host monotonic seconds. `cpu_submit_s` is sampled immediately before Metal commit. `gpu_end_s` is Metal's GPU completion timestamp and `gpu_completion_callback_s` records when the CPU handler ran. `presented_s` is the drawable's `presentedTime` from its presented handler. Zero and unavailable timestamps are JSON null. A row without `cpu_submit_s` records a display-link tick that produced no command. A missing presented handler and a handler that returns zero are counted separately.

Presentation statistics use distinct positive timestamps inside the measurement window in time order. The report counts duplicates and samples outside that window. It includes nearest-rank p50, p95 and p99, the maximum, and an interval histogram with inclusive upper bounds. `cadence_hz` is the number of intervals divided by the span from the first to last present. `window_hz` divides the distinct count by the full measurement time. The leading and trailing gaps show stalls at the edges. `missed_vsync` estimates skipped periods between observed presents from `max(0, round(interval × preferred_fps) − 1)`. The report separately counts intervals over 20% away from one nominal period and intervals over 20% away from the nearest positive period multiple. This exposes irregular pacing even when the overall cadence is close to target. The same analysis on `targetTimestamp` describes the display link's requested schedule. The CoreGraphics rate and application presentation measurements are separate fields.

The process exits 0 for a completed capture with a stable, visible display, at least two distinct presented timestamps, and completed Metal submissions. Exit 1 indicates an invalid capture or an operational failure. An invalid capture may include partial JSON. CLI errors exit 2. A completed capture can still miss performance targets or show that the selected mode differs from the preferred fps.

## Kernel cross-check

- `machdep.piano_dpu_snapshot` is the M1 read-only, root-only text sysctl. It exposes the INTF1/2 hardware frame counters (scanout VBL), the enable bits, the classified live mode, a serial, and a monotonic sample time in microseconds. The GUI probe attempts a read at both boundaries, and a normal GUI account will usually report permission denied. `--kernel-report` consumes the two privileged reads from the spec and derives each link's scanout rate from its unsigned 32-bit counter delta and the snapshot time delta. The bracket includes launch, warmup and drain, so take both reads in the same settled mode. The live probe also reads the panel's `DPTimingModeId` and `IOMFBDisplayRefresh.displayMinRefreshInterval` when present in IORegistry.
- PianoDpu keeps the new-surface flip total in the kernel. The periodic `PianoDpu: flips:` line includes that cumulative total. `PianoDpu: cadence N:` gives counts and interval statistics for successful new-surface latches. Same-surface reshow counts and gaps of at least 100 ms are reported separately. `NativeGopDisplay: period … VBL ticks` counts software timer wakeups, which can stop while idle. The offline report extracts these counters and cadence windows from the supplied log. If two cumulative flip lines bracket the run, their difference measures the larger bracket. There is no synchronous before/after read for flip totals or the software VBL counter in these drivers. Compare complete, positive cadence windows within the measurement using the log's timestamps and the JSON window wall-clock bounds.
- On an R3b GPU qualification profile, `machdep.piano_gpu_qual607_log` also carries periodic `DISPLAY` swap and copy aggregates for `check-gpu-qualification.py`. These records help diagnose a copy bottleneck alongside the presented timestamps.
- The driver already timestamps new-surface latches internally in `DpuFlip::cadenceFlip`. For one timestamp per flip, add one boot-gated read-only PianoDpu sysctl with a preallocated ring of monotonic latch timestamps indexed by the successful new-surface flip sequence. Record at the existing success point, after latch and fault checks, and exclude reshows and boot-surface restores. Return the first sequence, count and overwrite information with each coherent read. A 16,384-entry ring retains over 113 seconds at 144 Hz. A before/after sequence pair gives the exact flip delta and a read retains intervals for timestamp-level comparison. Existing kernels supply window aggregates through logs.

## Proposed goal 12 gate

Use the light load for the fixed-mode desktop composition check. At both 60 and 120 Hz, require a successful, visible capture with at least 60 seconds between the first and last presented timestamps in a 65-second measurement, both edge gaps at most three mode periods to allow for frames already in flight at the boundaries, and the active CoreGraphics mode within 1% of the target throughout. Require presented cadence and full-window throughput within 1% of the target, p50 within 10% of one period, p95 and p99 at most 1.20 periods, max at most two periods plus 0.5 ms, and at most 0.1% estimated missed vsyncs relative to the number of nominal slots. Keep the histogram and off-grid count with the result. A nearly correct aggregate rate can still fail the interval limits.

| Target | Nominal period | p95 and p99 ceiling |
| --- | ---: | ---: |
| 60 Hz | 16.667 ms | 20.0 ms |
| 120 Hz | 8.333 ms | 10.0 ms |

For 120 presented, require both hardware scanout links within 1% of 120 Hz, the expected DPU mode in both snapshots, and at least two complete in-run DPU cadence windows corroborating the user-space result. Require the new-surface cadence in those windows within 2% of 120 Hz, their p95/p99 within 1.20 periods, and zero reported gaps of at least 100 ms. Apply the same cross-check to the M3 60 Hz mode. A legacy configuration can show 60 Hz app presents on a panel scanning at 120 Hz. Record both rates.

Run a separate sustained 3D pass at a declared nonzero load, retaining all frame times, GPU completion counts, and the existing thermal guard and reset evidence. The appropriate load and 3D performance floor can be set after the first RAM qualification. Keep RAM and installed results distinct and repeat the desktop and 3D passes on the installed system for goal 12.
