# PianoMicNS: "Mic (noise reduced)"

A Core Audio AudioServerPlugIn with one input device, **Mic (noise reduced)**, that runs RNNoise (`third_party/rnnoise`, BSD-3-Clause)
on the tablet's raw microphone. Any recording app can choose it (QuickTime's microphone menu, Voice Memos and FaceTime-style apps
through the default input in Sound settings). PianoAudio's own **Mic** stays raw and stays the default.

- Frames: PianoAudio's microphone tap (`package/PianoAudio/AudioTap.h`, the service `piano-mic-tap`, level 10 with `piano-audio-tap`
  default 1), opened at the device's first StartIO and closed at its last StopIO: the microphone runs only while someone records from
  the device. The tap is read-only and only the audio driver host (coreaudiod, its driver service) may open it; a recording app gets
  these frames through the HAL, which enforces its microphone grant.
- Processing: `MicNsCore.c`: the tap's frames in 10 ms blocks through RNNoise into a FIFO held at a fixed fill (about 50 ms plus the
  read) against the DSP's clock by single-frame slips; the Mic's own mute and level apply.
- Clock: the host clock at a nominal 48 kHz, as PianoAudio's Mic and Speaker.
- Log: `PianoMicNS: ...` (subsystem `com.unixnu.piano.micns`): load, tap open, the first frames, every 30 s, each stop (RNNoise's
  time per block and CPU share).

Build: `tooling/k27/build-piano-micns.sh` (host tests, then `out/piano-micns/PianoMicNS.driver`, thin arm64e, ad-hoc signed; its
CDHash in `out/piano-micns/build.json`). arm64e, not arm64: on the tablet the HAL's driver service runs with pointer authentication on
and refuses an arm64 slice (`need 'arm64e'`), so the build's plug-in host test runs as an arm64e process, loads the arm64e bundle and
must refuse an arm64 build of the same sources. Install: `/Library/Audio/Plug-Ins/HAL/PianoMicNS.driver` with the userland layer, its
CDHash in the trust cache. Measured offline on the tablet's own recording (the core path of the host test, `micns_host --wav`): pink
noise 31.6 dB lower, speech -1.1 dB with envelope r 0.86. Provenance: `UPSTREAM`, `NOTICE`.
