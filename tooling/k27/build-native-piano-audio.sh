#!/bin/bash
# Build package/PianoAudio (the speakers and microphones, docs/drivers/audio.md) as a k27 native kext: the host tests first, then the
# objects in out/native-kexts/PianoAudio for tooling/k27/kext/PianoAudio.json. Inert without the boot argument piano-audio.
#   host tests: tests/audio_host.cpp (level 1: what the survey loads and never loads, the facts tables, decoders, verdicts, the SMMU
#   scan; levels 4-6: the pin values, the amplifier table, the bypass plan of the DSP's PCM streams), tests/gpr_host.c (the GPR codec,
#   sheng's), tests/audioreach_host.c (piano's APM/PRM packets), tests/sequence_host.c (one command at a time, the reverse unwind, the
#   speaker, PRM and microphone plans, the period pump), tests/fs19xx_host.c (the amplifiers against a register model: probe, a
#   synthetic preset container, the safety filter, the power sequences, status; the preset's SHA-256 identity), tests/amps_host.c
#   (the four as a group: reset sequence, all or none, test gain, mute-first shutdown), tests/spkprot_host.c (speaker protection's and
#   MIPLAY's files, packets and plans), tests/pcm_host.c (the speaker-safety chain:
#   slots, DC blocker, ceiling, fades, test tone, the walk, guard; the PCM ring), tests/dsplink_host.c (level 2: the DSP guard on
#   PianoPower's health, the inbox, the APM readiness exchange, the GPR port sets), tests/session_host.c (levels 4-7 against a DSP
#   model and the amplifiers' model in simulated time: the order of every command, speaker safety at every write and buffer, every
#   failure path; keep: the hardware handed to levels 8-10), tests/live_host.c (levels 8-10 after the real session handed over, against
#   the DSP and amplifier models and both macOS devices' engines: the plans in order, one command at a time over both, speaker safety
#   at every amplifier write and every buffer played, start at the first sound and down after silence, the clocks' drift, the master
#   volume's ramp, every fault and plan failure, the Mic's capture plan with the VA macro's DMIC path), tests/control_host.c (the IOAudio2
#   selector-2 scalar mappings), tests/va_host.c (the VA
#   macro's DMIC path against a register model: values, order, waits), tests/tap_host.c (the microphone tap's ABI, AudioTap.h, shared
#   with the "Mic (noise reduced)" plug-in: layout, wrap, lapped readers), tests/preset_host.c (the preset loader on an mtools FAT32
#   image as build-esp.py makes the ESP; skipped without mtools)
# The copies stay byte for byte their originals but for their provenance lines (FatVolume from PianoPower, SmmuStream from PianoDwc3,
# BitI2c from PianoKeyboard). Then the import check (PianoPower's client functions, package/include/PianoAdspContract.h, and NativeUfs's
# partition reader are the library imports) and tooling/k27/check-audio2-family.py: IOAudio2Family's classes in the KC base, which
# the macOS devices are linked under at run time.
# The amplifier preset (docs/drivers/audio.md 10.3, docs/kc-space.md 6): the kext carries only its pin, PianoAudio/fs19xx.fsm (4348
# bytes and the SHA-256 xiaomipad-8pro-mainline firmware/piano-audio.json publishes for OS3.0.303.0.WPYCNXM; AudioSha256.c holds
# the same two facts), written by tooling/persist/firmware.py stage. When the owner's own copy of the stock file is on this Mac
# (PIANO_AUDIO_PRESET, else private/audio/fs19xx.fsm, else the Ubuntu project's checkout next to this repository:
# ../xiaomipad-8pro-mainline/firmware/foursemi/piano/fs19xx.fsm), it is checked against the pin and staged in the ignored
# out/firmware/PianoAudio/, from where build-ram-images.py puts it into a RAM image's addon (PIANO_FW_ADDON=1) and build-esp.py
# onto the ESP (EFI/UniXNU/firmware/PianoAudio/fs19xx.fsm); never into the kext or this repository. Without a copy the kext is built
# the same, with its pin (level 5 then reads the ESP: the pinned path, then build-esp.py --audio-preset's EFI/UniXNU/piano-fs19xx.fsm).
# No DSP firmware or topology is needed or read.
#   environment: PIANO_K27_TREE (the k27 tree, read only: its compile recipes, its KC base), K27_OBJ (default k27-native-obj), CC/CXX
#                (host compilers for the tests, default cc/c++), K27_KERNEL (for the import check; skipped when absent), KC_BASE,
#                PIANO_AUDIO_PRESET (the owner's stock fs19xx.fsm), PIANO_AUDIO_SPK_PARAMS / PIANO_AUDIO_SPK_RE25 / PIANO_AUDIO_SPK_DOLBY
#                (the private files below)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
P=$D/package/PianoAudio; O=$D/out/native-kexts/PianoAudio; T=$O/host-tests
mkdir -p "$T"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/audio_host.cpp" -o "$T/audio_host"
"$T/audio_host" || { echo "Error: PianoAudio host tests failed" >&2; exit 1; }
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "$P/tests/level_dictionary_host.cpp" -framework CoreFoundation -o "$T/level_dictionary_host"
"$T/level_dictionary_host"
C=("${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$P")
"${C[@]}" "$P/tests/gpr_host.c" "$P/PianoGpr.c" -o "$T/gpr_host"
"${C[@]}" "$P/tests/audioreach_host.c" "$P/PianoAudioReach.c" "$P/PianoGpr.c" -o "$T/audioreach_host"
"${C[@]}" "$P/tests/sequence_host.c" "$P/AudioSequence.c" "$P/PianoAudioReach.c" "$P/PianoGpr.c" -o "$T/sequence_host"
"${C[@]}" "$P/tests/spkprot_host.c" "$P/AudioSpkProt.c" "$P/AudioSequence.c" "$P/PianoAudioReach.c" "$P/PianoGpr.c" -o "$T/spkprot_host"
"${C[@]}" "$P/tests/fs19xx_host.c" "$P/Fs19xx.c" "$P/AudioSha256.c" -o "$T/fs19xx_host"
"${C[@]}" "$P/tests/amps_host.c" "$P/AudioAmps.c" "$P/Fs19xx.c" "$P/AudioSha256.c" -o "$T/amps_host"
"${C[@]}" "$P/tests/pcm_host.c" "$P/AudioSafety.c" -lm -o "$T/pcm_host"
"${C[@]}" "$P/tests/control_host.c" -o "$T/control_host"
"${C[@]}" "$P/tests/timing_host.c" -o "$T/timing_host"
"${C[@]}" "$P/tests/dsplink_host.c" "$P/AudioDspLink.c" "$P/PianoGpr.c" -o "$T/dsplink_host"
"${C[@]}" "$P/tests/session_host.c" "$P/AudioSession.c" "$P/AudioAmps.c" "$P/Fs19xx.c" "$P/AudioSha256.c" "$P/AudioSequence.c" \
  "$P/PianoAudioReach.c" "$P/PianoGpr.c" "$P/AudioSafety.c" "$P/AudioDspLink.c" -o "$T/session_host"
"${C[@]}" "$P/tests/preset_host.c" "$P/AudioPreset.c" "$P/AudioAmps.c" "$P/Fs19xx.c" "$P/AudioSha256.c" "$P/FatVolume.c" -o "$T/preset_host"
"${C[@]}" "$P/tests/live_host.c" "$P/AudioLive.c" "$P/AudioSpkProt.c" "$P/AudioSession.c" "$P/AudioAmps.c" "$P/Fs19xx.c" "$P/AudioSha256.c" "$P/AudioSequence.c" \
  "$P/PianoAudioReach.c" "$P/PianoGpr.c" "$P/AudioSafety.c" "$P/AudioDspLink.c" -lm -o "$T/live_host"
"${C[@]}" "$P/tests/mic_fifo_host.c" "$P/AudioSpkProt.c" "$P/AudioSession.c" "$P/AudioAmps.c" "$P/Fs19xx.c" "$P/AudioSha256.c" "$P/AudioSequence.c" \
  "$P/PianoAudioReach.c" "$P/PianoGpr.c" "$P/AudioSafety.c" "$P/AudioDspLink.c" -lm -o "$T/mic_fifo_host"
"${C[@]}" "$P/tests/va_host.c" "$P/AudioVa.c" -o "$T/va_host"
"${C[@]}" "$P/tests/tap_host.c" -o "$T/tap_host"
"${C[@]}" "$P/tests/prime_host.c" "$P/AudioSpkProt.c" "$P/AudioSession.c" "$P/AudioAmps.c" "$P/Fs19xx.c" "$P/AudioSha256.c" "$P/AudioSequence.c" \
  "$P/PianoAudioReach.c" "$P/PianoGpr.c" "$P/AudioSafety.c" "$P/AudioDspLink.c" -o "$T/prime_host"
for t in gpr_host audioreach_host sequence_host spkprot_host fs19xx_host amps_host pcm_host control_host timing_host dsplink_host session_host live_host prime_host mic_fifo_host va_host tap_host; do
  "$T/$t" || { echo "Error: PianoAudio $t failed" >&2; exit 1; }
done
# The preset loader on a FAT32 image with 4096-byte sectors, one per cluster, as build-esp.py makes the ESP (mtools).
if command -v mformat >/dev/null && command -v mmd >/dev/null && command -v mcopy >/dev/null; then
  rm -rf "$T/esp" "$T/esp.img"; mkdir -p "$T/esp"
  "$T/preset_host" --write "$T/esp"
  mformat -C -F -S 5 -M 4096 -T 70000 -h 1 -s 1 -c 1 -i "$T/esp.img" ::
  mmd -i "$T/esp.img" ::/EFI ::/EFI/UniXNU
  for f in "$T/esp"/*; do mcopy -i "$T/esp.img" "$f" "::/EFI/UniXNU/$(basename "$f")"; done
  "$T/preset_host" "$T/esp.img" || { echo "Error: PianoAudio preset_host failed" >&2; exit 1; }
  rm -f "$T/esp.img"
else
  "$T/preset_host" || { echo "Error: PianoAudio preset_host failed" >&2; exit 1; }
  echo "SKIP the preset loader on a FAT image: no mtools (brew install mtools)" >&2
fi
# The copies stay byte for byte their originals (but for their provenance lines), which their owners never have to change.
for f in FatVolume.c FatVolume.h; do
  diff <(tail -n +4 "$P/$f") <(tail -n +2 "$D/package/PianoPower/$f") > /dev/null \
    || { echo "Error: package/PianoAudio/$f has drifted from package/PianoPower/$f" >&2; exit 1; }
done
diff <(tail -n +4 "$P/SmmuStream.h" | sed 's/namespace pianoaudio/namespace pianousb/') <(tail -n +2 "$D/package/PianoDwc3/SmmuStream.h") \
  > /dev/null || { echo "Error: package/PianoAudio/SmmuStream.h has drifted from package/PianoDwc3/SmmuStream.h" >&2; exit 1; }
diff <(tail -n +4 "$P/BitI2c.h" | sed 's/namespace pianoaudio/namespace pianokeyboard/; s/PIANO_AUDIO_BIT_I2C_H/PIANO_KEYBOARD_BIT_I2C_H/') \
  <(tail -n +2 "$D/package/PianoKeyboard/BitI2c.h") > /dev/null \
  || { echo "Error: package/PianoAudio/BitI2c.h has drifted from package/PianoKeyboard/BitI2c.h" >&2; exit 1; }
python3 "$D/package/common/tests/run-firmware-tests.py"   # the shared run-time firmware loader (docs/kc-space.md 6)
python3 -I "$D/tooling/audio/spk-prot-extract.py" --self-test    # the private files' tools, on synthetic data
python3 -I "$D/tooling/audio/spk-dolby-extract.py" --self-test
# The preset's pin. The pin's path is PianoAudio/<the file's name>: a copy under another name is staged through fs19xx.fsm.
PRESET_SHA=ab060b8476065bd789f327f7f8e724a2e17d94e93a85e2e8aa4b4954ce88b90e
grep -q "${PRESET_SHA:0:8}" "$P/AudioSha256.c" || { echo "Error: the preset's pin and AudioSha256.c disagree" >&2; exit 1; }
PRESET=${PIANO_AUDIO_PRESET:-}
for f in "$D/private/audio/fs19xx.fsm" "$D/../xiaomipad-8pro-mainline/firmware/foursemi/piano/fs19xx.fsm" \
         "$HOME/Downloads/XiaomiPad8Pro-Ubuntu/xiaomipad-8pro-mainline/firmware/foursemi/piano/fs19xx.fsm"; do   # the last: a worktree
  [ -n "$PRESET" ] || [ ! -f "$f" ] || PRESET=$f
done
mkdir -p "$O/gen"
if [ -n "$PRESET" ] && [ "$(basename "$PRESET")" != fs19xx.fsm ]; then
  [ -f "$PRESET" ] || { echo "Error: PIANO_AUDIO_PRESET=$PRESET is missing" >&2; exit 1; }
  mkdir -p "$O/preset"; cp "$PRESET" "$O/preset/fs19xx.fsm"; PRESET=$O/preset/fs19xx.fsm
fi
# Speaker protection's two private files (docs/drivers/audio.md section 12; package/PianoAudio/AudioSpkProt.h): the owner's ACDB
# parameters and this unit's Re25, both made by tooling/audio/spk-prot-extract.py from the owner's own copies. When both are on this
# Mac (PIANO_AUDIO_SPK_PARAMS / PIANO_AUDIO_SPK_RE25, else private/audio/spk-prot.bin and spk-re25.bin; PIANO_AUDIO_SPK=0 builds
# without them) they are pinned as PRIVATE files (firmware.py stage with a bare FILE: size and SHA-256 taken here, never printed or
# committed) and staged in the ignored out/firmware/PianoAudio/, from where a RAM image's addon and build-esp.py carry them. A kext
# built with these pins accepts only these two files: it is this unit's, never a published image's. Without them the kext is built
# the same with no pin, and piano-audio-spk=1 logs "PianoSpeakerProtV1: OFF" and plays through the passthrough graph.
SPK_PARAMS=${PIANO_AUDIO_SPK_PARAMS:-$D/private/audio/spk-prot.bin}
SPK_RE25=${PIANO_AUDIO_SPK_RE25:-$D/private/audio/spk-re25.bin}
# MIPLAY's private file (AudioSpkProt.h; docs/drivers/audio.md section 12.1): the owner's ACDB calibration of the Dolby/MiSound subgraph
# with the Dolby tuning of the owner's dax-default.xml, made by tooling/audio/spk-dolby-extract.py (PIANO_AUDIO_SPK_DOLBY, else
# private/audio/spk-dolby.bin), pinned and staged the same way, only together with the two protection files (MIPLAY runs only inside
# the protection plan). Without it piano-audio-miplay logs "PianoMiplayV1: OFF" and the protection plan runs without MIPLAY.
SPK_DOLBY=${PIANO_AUDIO_SPK_DOLBY:-$D/private/audio/spk-dolby.bin}
SPK=()
rm -rf "$O/spk"
if [ "${PIANO_AUDIO_SPK:-1}" != 0 ] && [ -f "$SPK_PARAMS" ] && [ -f "$SPK_RE25" ]; then
  mkdir -p "$O/spk"
  cp "$SPK_PARAMS" "$O/spk/spk-prot.bin"; cp "$SPK_RE25" "$O/spk/spk-re25.bin"   # the pins' names, whatever the copies are called
  SPK=("$O/spk/spk-prot.bin" "$O/spk/spk-re25.bin")
  printf '#pragma once\n#define PIANO_AUDIO_SPK_FILES 1\n' > "$O/gen/PianoAudioSpkFiles.h"
  echo "PianoAudio: speaker protection's private files pinned and staged (private: this unit's kext, never published)" >&2
  if [ -f "$SPK_DOLBY" ]; then
    cp "$SPK_DOLBY" "$O/spk/spk-dolby.bin"
    SPK+=("$O/spk/spk-dolby.bin")
    printf '#define PIANO_AUDIO_DOLBY_FILE 1\n' >> "$O/gen/PianoAudioSpkFiles.h"
    echo "PianoAudio: MIPLAY's private file pinned and staged (private: this unit's kext, never published)" >&2
  else
    printf '#define PIANO_AUDIO_DOLBY_FILE 0\n' >> "$O/gen/PianoAudioSpkFiles.h"
    rm -f "$D/out/firmware/PianoAudio/spk-dolby.bin"
    echo "PianoAudio: no MIPLAY private file ($(basename "$SPK_DOLBY")): built without it (protection without MIPLAY)" >&2
  fi
else
  printf '#pragma once\n#define PIANO_AUDIO_SPK_FILES 0\n#define PIANO_AUDIO_DOLBY_FILE 0\n' > "$O/gen/PianoAudioSpkFiles.h"
  rm -f "$D/out/firmware/PianoAudio/spk-prot.bin" "$D/out/firmware/PianoAudio/spk-re25.bin" "$D/out/firmware/PianoAudio/spk-dolby.bin"
  echo "PianoAudio: no speaker-protection private files ($(basename "$SPK_PARAMS"), $(basename "$SPK_RE25")): built without them" \
    "(passthrough only)" >&2
fi
python3 "$D/tooling/persist/firmware.py" stage --kext PianoAudio --header "$O/gen/PianoAudioFirmwarePins.h" \
  --source "$O/gen/PianoAudioFirmware.c" "${PRESET:-$O/preset/fs19xx.fsm}=4348:$PRESET_SHA" ${SPK[@]+"${SPK[@]}"}
rm -rf "$O/preset" "$O/spk"
python3 "$D/tooling/k27/build-native-kext.py" PianoAudio "$P/PianoAudio.cpp" "$P/AudioDsp.cpp" "$P/AudioBoard.cpp" "$P/PianoAudioDevice.cpp" "$P/PianoAudioTap.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" "$P/PianoGpr.c" "$P/PianoAudioReach.c" "$P/AudioSequence.c" "$P/Fs19xx.c" "$P/AudioSha256.c" \
  "$P/AudioSafety.c" "$P/AudioDspLink.c" "$P/AudioPcm.c" "$P/AudioAmps.c" "$P/AudioSession.c" "$P/AudioPreset.c" "$P/FatVolume.c" \
  "$P/AudioLive.c" "$P/AudioSpkProt.c" "$P/AudioVa.c" "$D/package/common/PianoFirmware.cpp" "$D/package/common/PianoFirmwareCore.c" "$D/package/common/PianoFirmwareZlib.c" \
  "$O/gen/PianoAudioFirmware.c" -I "$P" -I "$D/package/common" -I "$O/gen" \
  --obj "${K27_OBJ:-k27-native-obj}"
KERNEL=${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}
# PianoPower's client functions and NativeUfs's partition reader come from those kexts (listed before PianoAudio, named as libraries
# by the descriptor): the KC link resolves them by name.
[ ! -f "$KERNEL" ] || python3 "$D/tooling/k27/check-kext-imports.py" "$O/objects.json" "$KERNEL" \
  --library-prefix _piano_adsp_ --library-prefix _piano_qrtr_ --library-prefix _native_ufs_part_ | cut -c1-200 >&2
KC_BASE=${KC_BASE:-$K/out/kc/kc27.bin}
LOG=()
[ ! -f "$D/out/k27/kc-k27-native.log" ] || LOG=(--kc-log "$D/out/k27/kc-k27-native.log")
[ ! -f "$KC_BASE" ] || python3 "$D/tooling/k27/check-audio2-family.py" "$KC_BASE" --tree "$K" ${LOG[@]+"${LOG[@]}"} >&2
