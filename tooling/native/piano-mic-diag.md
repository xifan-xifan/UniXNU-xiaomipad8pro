# Direct PianoAudio microphone diagnostic

`piano-mic-diag.c` is an explicit root-only engineering probe for PianoAudio's
`mic-delivery1` input ABI. It opens the repository's Mic service through IOKit,
uses its existing start/stop and control selectors, and copies the mono S16
48 kHz input ring into a WAV. It does not load CoreAudio, request microphone
authorization, or change TCC policy. This measures the driver delivery path;
an authorized application recording remains a separate acceptance.

Build with the command in the source header, then ad-hoc sign the executable
for the target. `--selftest` touches no device and needs no privileges.
Run `--control 100 0` for input unity/unmuted, `--control 50 0` for the declared
approximately -18 dB input level, or `--control 100 1` for input mute.
`--capture UNIQUE_PREFIX MAX_SECONDS` requires an idle Mic and creates a new
WAV exclusively. It publishes `PREFIX.ready` after two seconds; create
`PREFIX.stop` to stop. A timeout, interrupted capture, changed epoch, stale
anchor, ring overrun, changing copied samples, or cleanup failure is a failure.
Successful explicit stop/unmapping is recorded in `PREFIX.done` and stdout.
The default input controls are not changed by capture itself.

The fixed 7680-frame ring publishes timestamps at each wrap. The probe reads
each 480-frame block at least 20 ms after its end on the interpolated engine
clock, verifies the copy again after 2 ms, and reserves 20 ms before
overwrite. A delayed poll can catch up while the unread PCM is still inside
that window. `CAPTURE_TIMING` reports the maximum gap between coherent polls
and the number of split timestamp reads. `CAPTURE_CHECK_FAIL` identifies a
failed check and logs the last observed clock, anchor, seed and reader cursor.
Acceptance also requires the saved driver READ/FIFO/late/PCM diagnostics, two
distinct start epochs, and complete graph/VA/map/vote release after both stops.
There must be no concurrent input client; the probe refuses an already running
engine and does not take ownership away from another client.

Use the supplied coded 997 Hz acoustic fixture from an independent source at
a fixed recorded volume. Collect source-off silence, unity, lower level,
mute and recovered unity in one continuous recording, retaining five seconds
around each scored burst. Preserve a separate source microphone witness.
`check-mic-diag.py` consumes saved files only; it supplements the fixture's
carrier/envelope/gain measurements with five-second windows and exact PCM
mute. Neither PCM energy alone nor an advancing ring is a pass.

The probe is compatible with the committed microphone ABI first tested in
`b49b73d2`. Source availability does not establish acoustic success on hardware.

### Pre-mix route fixture

Generate a 2.4 s coded 997 Hz fixture with
`python3 tooling/native/piano-mic-route.py fixture fixture.wav`. Start
with -20 dBFS source and record the existing speaker settings. In the same
input run, capture a settled source-off section, the full fixture burst,
and a second source-off section. Warm output using a separate non-carrier
primer before the scored burst if the tablet is the source.

Use `machdep.piano_audio_mic_route_state` together with current-console
`mic-route1: start` and `mic-route1: window` to obtain generation, run, and
route. After the arm completes, save the root-only opaque sysctl response:

```sh
sudo sysctl -b machdep.piano_audio_mic_route_pcm > window.pmr
python3 tooling/native/piano-mic-route.py extract window.pmr window --gen 2 --run 1 --route 4
python3 tooling/native/piano-mic-route.py score window-DEC0.wav --onset 3.2
python3 tooling/native/piano-mic-route.py score capture.wav --onset 3.2 --probe-log capture.log --seed 1
```

Onset is the source-on position in each specific WAV, estimated from
recorded phase markers and searched in a short interval. Use separate
estimates for raw, driver and source-witness recordings. The checker reports
the aligned carrier level over source-off, envelope correlation, on/off RMS,
clipping and the longest zero segment. The raw export contains period arrival
times and DSP timestamps alongside DEC0 and DEC1. The reconstructed selected
WAV is pre-FIFO. The direct reader's mono WAV and the console's READ/FIFO/HAL
counters provide the delivery evidence.
