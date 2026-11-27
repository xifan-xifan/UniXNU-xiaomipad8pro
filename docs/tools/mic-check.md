# Piano microphone app check

`piano-mic-check` records the console user's default input with AVFoundation's `AVAudioRecorder`. It saves 48 kHz mono 16-bit PCM in an `.m4a` container. The optional stimulus plays a bundled 1 kHz WAV through the default output during capture. The optional playback check opens the saved file with `AVAudioPlayer` and waits for its completion callback.

The VPIO probe's `avrec` mode uses the same recorder and its `--tcc` reports the same AVFoundation authorization value. `piano-hal-rec` captures through the HAL's AudioQueue, `piano-mic-diag` reads the PianoAudio input before the HAL, and `audio-listen.py` captures the tablet's speaker output with the Mac's C920 microphone. [VPIO probe](../../tooling/native/piano-vpio-probe/piano-vpio-probe.m) · [Piano mic diagnostics](../../tooling/native/piano-mic-diag.md)

## RAM qualification on y12t-obs1 (2026-11-27)

The unattended **AVFoundation app recording** passed with the calibrated −36 dBFS stimulus, `piano-mic-unattended=1` and TCC pre-seeded for the signed app. The quiet and tone captures used the default **Mic** and **Speaker** at 48 kHz mono. The quiet control passed with zero clipped samples. The 4.000 s `tone.m4a` measured −12.90 dBFS RMS and −4.55 dBFS peak in the `second=1` window. The tone fit was 1002.09 Hz with 22.21 dB SNR and SINAD. Zero samples clipped, resolving the clipping from the earlier −18 dBFS run on `v131y12t-qual1`.

The **C920 acoustic playback check** also passed. In `replay.grade.txt`, the tone was −9.50 dBFS at 1 kHz with 18.09 dB SNR. It was 39.48 dB above the clean quiet control (−48.98 dBFS at 1 kHz).

Hands-on QuickTime and Voice Memos recording and playback on the installed system and the active installed duplex ledger remain open. [November 27 mic RAM results](../results-2026-11-27.md#calibrated-microphone-and-c920-playback)

## Ubuntu mainline comparison

References are from the local `XiaomiPad8Pro-Ubuntu` clones at `xiaomipad-8pro-mainline` commit `b43fc2b` and `linux-sm8750-piano` commit `b5100d5f2e0b`.

| Path | Ubuntu mainline reference | UniXNU check |
| --- | --- | --- |
| Capture route | `arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-desktop.dts:58–71` declares the VA backend. `device/audio/ucm2/Qualcomm/sm8750/Xiaomi-Pad-8-Pro/HiFi.conf:54–93` maps VA DEC0/1 to DMIC0/2 on two-channel ALSA capture PCM 2. | The app records the selected CoreAudio input through AVFoundation. [`record_to`](../../tooling/native/piano-mic-check/piano-mic-check.m) |
| Capture evidence | `tools/mic-capture-check.py:123–195,230–268` grades the saved ALSA WAV offline, with an optional known-frequency source. | The host grades the saved app capture offline, using a quiet take and a known 1 kHz stimulus. [`analyze_wav`](../../tooling/native/piano-mic-check/analyze.py) |
| Speaker evidence | `tools/speaker-playback-device.sh:71–124` plays one bounded tone through ALSA. `tools/speaker-playback-check.py:139–221` grades the stream, amplifier state and a current proxy. | The app checks the output route and AVAudioPlayer completion. The Mac captures and grades the acoustic result. [`replay`](../../tooling/native/piano-mic-check/piano-mic-check.m) · [`audio-listen.py`](../../tooling/native/audio-listen.py) · [`analyze_replay`](../../tooling/native/piano-mic-check/analyze.py) |

## Build on the Mac host

```sh
make -C tooling/native/piano-mic-check SDK=macosx
make -C tooling/native/piano-mic-check SDK=macosx test
```

The arm64 build uses the selected Xcode macOS SDK and targets macOS 14 or later. The Makefile generates the WAV, packages it with the signed `out/piano-mic-check/PianoMicCheck.app`, and verifies the signature. Keep that bundle together when staging. Host tests cover the `afconvert` round trip, the capture grades, and replay grading with synthetic tones, noise, and damaged WAVs.

## Run on the installed system

Use an installed test boot whose command line contains exactly one `piano-mic-unattended=1`. The opt-in defaults off. Verify the running line with `/usr/sbin/sysctl -n kern.piano_bootargs` on the tablet. The block falls back to `kern.bootargs` on earlier kernels.

Use the harness's desktop gate to reach the unlocked desktop with the same user as SSH. The tablet needs **Mic** as the default input and an unmuted **Speaker** at a comfortable volume. For a run with the **Mic (noise reduced)** input selected, pass `noise-reduced` in place of `raw`. On the Mac, have FFmpeg and the **HD Pro Webcam C920** microphone available, with macOS microphone access already granted to the host capture process. The installed harness can use its C920 GUI recorder daemon for that access. Keep the room quiet and stop other audio on both devices.

From the checkout with the signed bundle, use these lines after the installed-session desktop gate. The block uses `R`, `rssh` with stdin forwarding, `rsudo`, `pull`, `verdict`, and `fexpect`.

```bash
repo=$PWD
source "$repo/tooling/native/piano-mic-check/mic-check.spec"
rssh '/usr/sbin/sysctl -n kern.bootsessionuuid; /usr/sbin/sysctl -n kern.piano_bootargs'
mic_check_block "$repo" raw
```

The preflight checks the staged app with `--tcc` in the console user's LaunchServices session. On an opt-in boot, an undetermined or denied status triggers an app-only seed. The seed follows the earlier camera and VPIO microphone runs that used the console user's ProtectedSystem TCC database and restarted the user's `tccd`. It writes one microphone row for `com.unixnu.piano-mic-check` bound to the staged arm64 CDHash. The grant persists on installed Data for that code signature. The database receipt alone is insufficient. A fresh app launch must report `mic_status=3` and `result=ok` before either recording starts.

The block stages the signed bundle in a private, fresh directory using `rssh` and verifies the archive hash and app signature on the tablet. It uses `rsudo` to get the current UID from `/dev/console`, enters its bootstrap session with `/bin/launchctl asuser`, and drops to that UID with `sudo -n -u "#$u"`, as in the frametime check. It opens the bundle through LaunchServices so TCC can identify the GUI app, and reads the app's result line from its redirected standard output. [Frametime console launch](frametime.md#build-and-run)

The block records four seconds of room noise, then four seconds with the stimulus. The bundled WAV has a half-second of silence, a one-second burst at −36 dBFS peak with 20 ms ramps, and another half-second of silence. [Measured capture headroom and C920 replay levels](#ram-qualification-on-y12t-obs1-2026-11-27)

After both saved recordings pass the tablet and host checks, the block opens `quiet.m4a` and `tone.m4a` in separate AVAudioPlayer launches. Each launch waits for a first-sample receipt from a fresh 18-second C920 take. Each replay must finish on the tablet's **Speaker** while the C920 is still recording.

Without the boot arg, the attended flow uses an existing grant or `MIC_CHECK_REQUEST_ACCESS=1` to request consent for an undetermined app. Click **Allow** within 60 seconds when prompted. The app replays the tone take after capture and the operator records the audible result.

## Read the evidence

The app reports the microphone authorization state and numeric `mic_status` using AVFoundation's values 0 (not determined), 1 (restricted), 2 (denied), and 3 (authorized). The capture log has the input name and HAL stream format, the saved recording format, per-second RMS and peak dBFS, and the default output name during a speaker run. Both capture analyzers report `clipping_detected=yes` and `clipped_samples` when they find samples at full scale. Exact digital zero displays as −200 dBFS.

The printed `$R/mic-check.*` directory holds `boot.txt`, the `tcc*.txt` receipts, the pulled M4As, and the input grades. `remote-stage.txt` points to the private copies on the tablet. For an unattended PASS, look for `mic_status=3` in `tcc.txt` or `tcc-after-*.txt`, `playback_complete=yes` in both `replay-*.txt` logs, and `grade=pass` in `replay.grade.txt`. The paired `c920-quiet.wav` and `c920-tone.wav` preserve the acoustic evidence. Check the adjacent `*.record.txt` logs when the recorder fails to start or complete.

| Native exit | Result | Meaning |
| --- | --- | --- |
| 0 | `ok` | Requested operation completed |
| 1 | `error` | Audio, file, route change, duration, or playback failure |
| 2 | `usage` | Invalid command or arguments |
| 3 | `no_permission` | AVFoundation microphone access is unavailable |
| 4 | `silence` | No 200 ms window reaches −85 dBFS AC RMS |
| 5 | `tone_missing` | Capture has signal but the stimulus check failed |

The native binary accepts `SECONDS OUTPUT.m4a [--tone] [--playback]` with an unused output path, 1–300 integer seconds, and at least 3 seconds for the tone. `--tcc` reads authorization and `--request-access` prompts when it is undetermined. `--analyze FILE [--tone]` checks a saved mono file. On opt-in boots, `--replay FILE.m4a` plays a saved file through **Speaker** without opening a microphone.

To regrade evidence on the Mac, use the standard-library analyzer. The capture modes normalize M4A or WAV to mono 16-bit PCM with `/usr/bin/afconvert` in a temporary directory. Replay mode reads the C920's 16 kHz mono PCM WAV directly.

```sh
python3 tooling/native/piano-mic-check/analyze.py /path/to/tone.m4a --mode tone --seconds 4
python3 tooling/native/piano-mic-check/analyze.py /path/to/quiet.m4a --mode quiet --seconds 4
python3 tooling/native/piano-mic-check/analyze.py /path/to/c920-tone.wav \
  --mode replay --control /path/to/c920-quiet.wav --seconds 18
```

- **Tone** requires two adjacent 200 ms windows with a fitted 980–1020 Hz fundamental at −55 dBFS RMS or higher and at least 12 dB SNR. Both analyzers refine the 5 Hz scan to fit a sine at fractional frequencies. `tone_sinad_db` compares the fundamental with all remaining AC power. For windows with at least 1% full-scale samples and at least 6 dB SINAD, `tone_snr_db` removes the fitted second through ninth harmonics below Nyquist from that residual. Other windows use the full residual for SNR.
- **Quiet** requires each full second's AC RMS to fall between −85 and −25 dBFS, the capture's peak to stay at or below −6 dBFS, and no test tone to be detected. The reported noise floor is the median of those full-second AC RMS levels. These are initial qualification bands for a quiet room.
- **Replay** requires two adjacent 200 ms windows at 980–1020 Hz, each at −75 dBFS tone RMS or higher and at least 12 dB SNR. The weaker window must also be at least 10 dB above the quiet replay's strongest sustained 1 kHz level. A qualifying tone in the quiet replay fails the control. Both C920 takes must contain nonconstant PCM and match the requested duration within 0.25 s.
- The capture modes also require the duration to be within 0.25 s when `--seconds` is supplied. The host analyzer exits 0 for a pass, 1 for audio outside the grade, and 2 for invalid or unreadable input.

In the clipped qualification take, the 1.4–1.6 s window had 6,586 full-scale samples out of 9,600. The old fit put the fundamental at the 1000 Hz bin (−3.85 dBFS), leaving leaked fundamental power and distortion in the residual (0.30 dB SNR). Fitting 1002.06 Hz recovers the fundamental at −1.25 dBFS with 11.93 dB SINAD. The third harmonic alone is about −14 dBFS. Both capture analyzers now detect the tone at 24.48 dB SNR and report 20,052 clipped samples across the take. The paired quiet take passes at a −44.04 dBFS noise floor with no tone detected.
