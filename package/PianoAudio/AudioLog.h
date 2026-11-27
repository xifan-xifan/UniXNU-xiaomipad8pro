// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoAudio's log line ("PianoAudio: ..."), shared by the kext's files (PianoAudio.cpp defines it), and the calls between them.
#pragma once
#include <stddef.h>
#include <stdint.h>

extern "C" void piano_audio_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// AudioDsp.cpp: level 2 (the DSP link) from the GPR port set `ports` (AudioDspLink.h audio_ports; try_ports: walk the others while
// nobody answers); 0 when the APM answered ready.
extern "C" int piano_audio_dsp_link(uint32_t wait_s, uint32_t settle_ms, int ports, int try_ports, char *summary, size_t size);

// AudioDsp.cpp: levels 4-7 (AudioSession.h) after level 2, or level 5 alone without the DSP (dsp 0); then, from piano-audio=8,
// levels 8-10 (AudioLive.h) for the rest of the boot when levels 4-6 passed. The arguments as parsed.
struct PianoAudioLevelArgs {
  uint32_t level;                     // 4..10
  int dsp;                            // level 2 reached "APM ready" in this boot
  uint32_t lpassRead, presetAny, mic, silenceMs;
  uint32_t tone, toneMask, toneMs, gapMs, toneGain;
  uint32_t i2cSwap, i2cHalfUs, smmu;
  int micRoute, micRouteDiag;         // signed route and opt-in pre-mix diagnostic
  uint32_t liveGain, liveCeilingDb, liveIdleMs, micGain;   // levels 8-10 (0: the level's default)
  uint32_t spk;                       // piano-audio-spk: speaker protection from the first start (odd: 1 + SPK_VAR_* bits, AudioSpkProt.h); 0 passthrough
  uint32_t ageOld;                    // piano-audio-age=0: duplex-age1's A/B control (the old unsigned sound-age test)
  uint32_t duplexHold;                // piano-audio-duplex-hold: duplex-hold1's mask (AudioLive.h audio_live_hold_select: 0 off, 1, 3)
  uint32_t miplay;                    // piano-audio-miplay: MIPLAY inside the protection plan (AudioSpkProt.h spk_miplay_valid; 0 off)
  uint32_t spkProbe;                  // piano-audio-spk-probe=1: machdep.piano_audio_spk_probe may start spk-probe1 (AudioLive.h audio_live_spk_probe)
  uint32_t tap;                       // piano-audio-tap: the microphone tap for "Mic (noise reduced)" at level 10 (AudioTap.h; default 1)
};

// Require an explicit =1 token. The last mention of the switch wins.
inline bool piano_audio_spk_probe_enabled(const char *args)
{
  if (!args) return false;
  constexpr char key[] = "piano-audio-spk-probe";
  constexpr size_t keyLength = sizeof key - 1;
  bool enabled = false;
  while (*args) {
    while (*args == ' ' || *args == '\t') ++args;
    const char *end = args;
    while (*end && *end != ' ' && *end != '\t') ++end;
    const size_t length = static_cast<size_t>(end - args);
    if (length >= keyLength && __builtin_memcmp(args, key, keyLength) == 0 &&
        (length == keyLength || args[keyLength] == '=')) {
      enabled = length == keyLength + 2 && args[keyLength + 1] == '1';
    }
    args = end;
  }
  return enabled;
}

// PianoAudioTap.cpp: the microphone tap (AudioTap.h) on sh: its shared memory and the service "piano-mic-tap" whose user client the
// "Mic (noise reduced)" plug-in opens (shared: AudioLive.h's AudioLiveShared); wake() runs the live thread at an open or close.
// 0, or -1 (no memory; no tap this boot).
extern "C" int piano_audio_tap_publish(void *shared, void (*wake)(void));
// Runs until the levels are done (levels 4-7 bounded; levels 8-10 for the rest of the boot, until PianoPower's health stops
// them); the summary for the log and ioreg. 0 when every level that ran passed.
int piano_audio_levels(const PianoAudioLevelArgs *args, char *summary, size_t size);
// AudioDsp.cpp: the dsp transport's shared state (levels 8-10), made once before the devices are published; volume_percent is the
// master volume of level 9 (piano-audio-volume). 0, or -1 (no memory: the devices then use the null transport).
extern "C" int piano_audio_live_prepare(uint32_t volume_percent);
// PianoAudio.cpp: a property on the PianoAudio service (the levels' summaries, levels 8-10's state while they run).
extern "C" void piano_audio_set_property(const char *key, const char *value);
