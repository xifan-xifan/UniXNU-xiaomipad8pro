#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

P=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$#" -ne 0 ] && { [ "$#" -ne 2 ] || [ "$1" != --out ]; }; then
  echo "usage: $0 [--out DIR]" >&2
  exit 2
fi
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/piano-audio-host.XXXXXX")
trap 'rm -r -- "$temp_dir"' EXIT
build_dir=${2:-$temp_dir}
mkdir -p "$build_dir"

checks=(-O1 -g -Wall -Wextra -Werror '-fsanitize=address,undefined' -fno-sanitize-recover=all)
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}halt_on_error=1"

"${CXX:-c++}" -std=c++20 "${checks[@]}" -I "$P" "$P/tests/audio_host.cpp" -o "$build_dir/audio_host"
"$build_dir/audio_host"
if [ "$(uname -s)" = Darwin ]; then
  "${CXX:-c++}" -std=c++20 "${checks[@]}" "$P/tests/level_dictionary_host.cpp" \
    -framework CoreFoundation -o "$build_dir/level_dictionary_host"
  "$build_dir/level_dictionary_host"
else
  echo "SKIP level_dictionary_host (requires macOS CoreFoundation)"
fi

C=("${CC:-cc}" -std=c11 "${checks[@]}" -I "$P")
run_test() {
  local test=$1
  shift
  "${C[@]}" "$P/tests/$test.c" "$@" -lm -o "$build_dir/$test"
  "$build_dir/$test"
}

reach=("$P/PianoAudioReach.c" "$P/PianoGpr.c")
sequence=("$P/AudioSequence.c" "${reach[@]}")
amp_drivers=("$P/Fs19xx.c" "$P/AudioSha256.c")
amps=("$P/AudioAmps.c" "${amp_drivers[@]}")
session=("$P/AudioSession.c" "${amps[@]}" "${sequence[@]}" "$P/AudioSafety.c" "$P/AudioDspLink.c")
live=("$P/AudioSpkProt.c" "${session[@]}")

run_test gpr_host "$P/PianoGpr.c"
run_test audioreach_host "${reach[@]}"
run_test sequence_host "${sequence[@]}"
run_test spkprot_host "$P/AudioSpkProt.c" "${sequence[@]}"
run_test fs19xx_host "${amp_drivers[@]}"
run_test amps_host "${amps[@]}"
run_test pcm_host "$P/AudioSafety.c"
run_test control_host
run_test timing_host
run_test dsplink_host "$P/AudioDspLink.c" "$P/PianoGpr.c"
run_test session_host "${session[@]}"
run_test live_host "$P/AudioLive.c" "${live[@]}"
# These three fixtures include AudioLive.c directly.
run_test prime_host "${live[@]}"
run_test mic_fifo_host "${live[@]}"
run_test duplex_schedule_host "${live[@]}"
run_test va_host "$P/AudioVa.c"
run_test tap_host

# Match build-esp.py's FAT32 image with 4096-byte sectors, one per cluster.
"${C[@]}" "$P/tests/preset_host.c" "$P/AudioPreset.c" "${amps[@]}" "$P/FatVolume.c" -o "$build_dir/preset_host"
if command -v mformat >/dev/null && command -v mmd >/dev/null && command -v mcopy >/dev/null; then
  mkdir "$temp_dir/esp"
  "$build_dir/preset_host" --write "$temp_dir/esp"
  image="$temp_dir/esp.img"
  mformat -C -F -S 5 -M 4096 -T 70000 -h 1 -s 1 -c 1 -i "$image" ::
  mmd -i "$image" ::/EFI ::/EFI/UniXNU
  for f in "$temp_dir/esp"/*; do
    mcopy -i "$image" "$f" "::/EFI/UniXNU/$(basename "$f")"
  done
  "$build_dir/preset_host" "$image"
else
  "$build_dir/preset_host"
  echo "SKIP preset_host FAT image (requires mtools)" >&2
fi
