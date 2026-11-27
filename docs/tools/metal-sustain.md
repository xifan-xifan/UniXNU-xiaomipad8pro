# Installed Metal 3D sustain test

Goal 12 calls for a real 3D workload on the installed system, with frame-level timing and a sustained run free of GPU hangs and resets.

## Measured results

- **2026-11-27, installed v13.1y12q (`inst-y12q-obs4`).** On Adreno 830 in High Power mode, the onscreen 1920 × 1280 workload
  with 257 draw calls per frame recorded `METAL_SUSTAIN_END rc=0` at **600.000 s**. It completed **35,985 frames** at **59.98 fps**
  across **60/60 undisturbed windows**, with a **59.80 fps** minimum 10 s window, **19.97 ms** worst-window completion p99 and
  **0 stalls > 50 ms**. The GPU sustained **967–1050 MHz** with **0 faults or recoveries**. Drawable `presentedTime` was unavailable
  (`presented=0`), so FPS and frame intervals measure Metal completions. [Full November 27 results](../results-2026-11-27.md#sustained-metal-3d-installed-v131y12q)
- **2026-11-23, historical RAM baseline.** `piano-gpubench` on `gpu/bench3d-1` (`3f1217fa`) rendered at 3200 × 2136 with 4,096 lit
  icospheres. Its 600-second pass reported **51.78 completed frames/s** and a **25.31 ms** completion p99. It spent 534 of 600 seconds
  at 734 MHz. Drawable callbacks supplied zero presentation timestamps, so those figures describe rendering throughput.
  [Support status](../STATUS.md) · [Presentation feedback](../drivers/presented-time.md)

The installed run's SSH wrapper hit its 650 s silent-connection timeout after the benchmark logged `METAL_SUSTAIN_END rc=0`.
`run.txt` lacked `METAL_SUSTAIN_REMOTE_RC=0`. The full `frames.csv`, `summary.log`, before/after GPU state and counter snapshots,
and console audit were pulled intact. The recovered evidence passed every numeric, fault and confound gate in `grade.py`
and all thermal checks. An automated `PASS` from the checked-in grader also requires the wrapper receipt.

## Implementation

| Existing source | Role in the installed test |
| --- | --- |
| `tooling/native/piano-frametime/piano-frametime.m`, `scene.h`, `Makefile` | AppKit and Metal setup, an embedded shader, drawable callbacks, and a single arm64 executable. |
| `tooling/native/gpu-qualification-load.m`, `gpu-qualification-pixels.m` | Existing composition load and separate pixel checks for the GPU frequency qualification. |
| `package/PianoA830/board/PianoA830.cpp:1647–1682`, `package/PianoA830/adreno830/Adreno830Start.cpp:2958–2999` | Read-only `machdep.piano_gpu_state` publishes the voted GMU level, MHz, ceiling, and binding limits. |
| `package/PianoA830/board/PianoA830.cpp:1356–1383` | Read-only `machdep.piano_gpu_recovery` exposes recovery, watchdog, and fault counters. |

The CLI is explicitly launched by the operator and consumes the installed driver's existing interfaces. There is no driver port or boot argument for this test.

## Build and launch

```sh
make -C tooling/native/piano-metal-sustain SDK=macosx
bench=out/piano-metal-sustain/piano-metal-sustain
results=$(mktemp -d /tmp/piano-metal-sustain.XXXXXXXX)
"$bench" --offscreen --seconds 12 > "$results/offscreen.csv" 2> "$results/offscreen.log"
"$bench" --onscreen --seconds 12 > "$results/onscreen.csv" 2> "$results/onscreen.log"
```

The single executable compiles the shader at runtime. Both modes render an orbiting view of a tiled arena with animated, textured crates, diffuse and specular lighting, and a depth attachment. The default 1920×1280 render has 256 separate indexed crate draws and one floor draw, for 442,370 submitted triangles per frame at one sample per pixel. Use `--width`, `--height`, and `--draws` to vary the load. The qualification keeps those defaults fixed. Offscreen mode submits continuously to a private render target. Onscreen mode uses a display-synchronized `CAMetalLayer` in an AppKit window on the main display, with up to three submitted commands in flight. Launch onscreen mode in the non-root console user's GUI session with the window visible and the display unlocked. Use `--minutes N` for a soak. The two-second warmup precedes the measurement in both modes.

## Frame and frequency evidence

- Stdout contains one CSV row per measured submission with CPU frame begin, encode begin, submit, Metal `GPUStartTime` and `GPUEndTime`, completion callback, `presentedTime`, presented callback, and command status. Times are host monotonic seconds. Empty cells mark unavailable timestamps. A presented callback with an empty `presented_s` supplied a zero or invalid timestamp. Offscreen mode has no drawable callbacks.
- Stderr gives the capture bounds and one `WINDOW` row per ten-second submission window, including a partial final window when needed. Each row gives nearest-rank p50, p95, p99, maximum, and samples over 50 ms for CPU submit cadence, CPU encode duration from drawable acquisition to commit, completion cadence, GPU execution duration and end cadence, and drawable presentation cadence. Cadence includes the interval crossing each window boundary. Windows are assigned by CPU frame begin and reported after up to two seconds for callbacks. `render_fps` counts distinct successful completion callbacks per window second. `present_fps` uses distinct positive drawable timestamps and is `na` when none are available. Retain the per-frame CSV to inspect individual stalls and distinguish GPU execution from host callback timing.
- The tool polls `machdep.piano_gpu_state` every 500 ms under load and records fresh driver snapshots as `GPU_STATE`, once per driver timestamp. `WINDOW` gives the sample count, min/max level, and max MHz, with `na` for unavailable state. The state reports the driver's voted level. It also carries the current cap, binding cause, and temperatures. Keep the original rows. Frequency sysctls are ordinarily unavailable on a Mac.
- `METAL_SUSTAIN_COMMAND_ERROR` is a command-buffer failure and makes the process exit nonzero. A queue or drawable timeout also fails the run. A watchdog exits if no command completes for 15 seconds. Onscreen rendering can complete on a system that supplies zero `presentedTime` values. In that case report Metal completion fps and frame times, and record presentation cadence as unavailable.

## Installed-system qualification

Use the installed boot's existing policy and the active USB console recorder. Allow at least 900 seconds for the desktop gate, smoke, capture and verified transfers after boot. Run after the normal desktop and lit-panel gate used by the main session. The block stages a hash-checked binary in a new remote temporary directory and checks the same windowed workload for three seconds before proceeding. It brackets the long process with unique `/dev/console` markers and brackets the run with boot UUID, GPU state, and GPU recovery counter reads. It retains the remote directory for re-pull and writes the host result to `$R/metal-sustain/`.

Add these lines to the installed-system runner's check phase. They use `rssh`, `rsudo`, `pull`, `lines`, `mark`, `waitfor`, and `verdict` from `tabletrun.sh`, as used in `v131y12s-qual1.spec`.

```sh
. tooling/native/piano-metal-sustain/installed.spec
metal_sustain_installed
```

The long run invokes the staged binary with `--onscreen --minutes 10 --width 1920 --height 1280 --draws 256` via `launchctl asuser` and `sudo -u` for the console UID. The exact privileged reads at both bounds are:

```sh
rsudo 'printf "BOOT="; /usr/sbin/sysctl -n kern.bootsessionuuid; /usr/sbin/sysctl -n machdep.piano_gpu_state; /usr/sbin/sysctl -n machdep.piano_gpu_recovery'
```

The offline grader requires all 60 windows and the process exit receipt, unchanged boot UUID and recovery/fault/watchdog counters, and both console markers with an in-window GPU heartbeat and no hang, recovery, reset, or loss line. Every window needs matching CSV and summary counts, positive Metal GPU timestamps, and GPU frequency state. A sampled level 3 or below at the pre-run read or during the run must precede a sampled level above 3 during the run. The performance floor is **at least 30 completed frames/s and at most 40 ms completion p99 in every 10-second window**. The grader recomputes those metrics from per-frame completion timestamps and reports >50 ms stalls. `gpu_work_max_ms` gives the longest measured GPU execution in each window, computed from the CSV's GPU start and end timestamps. Read it alongside completion p99 to see whether GPU execution lengthened during a cadence stall. The grader reports whether drawable presentation timestamps were available. Record the thresholds and scene settings when comparing results. The RAM benchmark used different geometry, draw counts, and resolution.

Keep the run unattended. The grader scans `console.txt` between the matching `BEGIN` and `END` markers for `PianoTouch` contact-down records and accelerator opens by Safari, Google Chrome, or `com.apple.WebKit` processes. It reports `touch_contacts` and `browser_accelerator_opens`. A process can produce multiple open records. The summary's `confound_windows` lists windows that may contain an event, using the logged accelerator-open timestamp or the surrounding GPU heartbeats for untimestamped records. If a heartbeat is missing on one side, the possible interval extends to the capture bound. `undisturbed_windows` counts windows outside those intervals. These fields locate the observed activity and leave the duration of any contention undetermined.

| Exit | Verdict | Conditions |
| --- | --- | --- |
| `0` | `PASS` | Valid capture, every window meets the floor, and the bounded console has no detected confounds. |
| `1` | `FAIL` | GPU hang, reset, recovery counter change, or command error even when confounds are present. Also covers invalid evidence or a clean run that misses the performance floor. |
| `2` | `VOID` (`CONFOUND`) | The capture and GPU checks are valid and the bounded console contains external interaction evidence. The grader reports any floor misses and the installed runner records `VOID` and returns 2. |

Regrade a saved result locally with `python3 tooling/native/piano-metal-sustain/grade.py "$R/metal-sustain"`.

## Host verification

The arm64 build and ad hoc signature verified on the M5 Pro host with SDK 27.2. The background host process returned no Metal device, and the onscreen launch found no logged-in console session. Neither launch produced real GPU frame timings in that environment. The formatter and offline grader passed a 60-window fixture and rejected a missing frequency step, low fps, high p99, a reset, changed recovery counters, a command error, a missing console marker, and missing frame evidence. The installed smoke is the hardware test of shader compilation and frame capture on Adreno.

Run the offline grader tests with `make -C tooling/native/piano-metal-sustain SDK=macosx test`. They generate synthetic captures to exercise clean passes, performance failures, confounds, and GPU fault precedence.
