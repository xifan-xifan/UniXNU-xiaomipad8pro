#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang.
# Build package/PianoMicNS ("Mic (noise reduced)", private/host-checks MIC-NS.md section 8): the host tests first (the core against
# PianoAudio's tap ABI, package/PianoAudio/AudioTap.h, in simulated time: passthrough integrity under +-200 ppm drift, RNNoise on
# white noise and silence, the tap's events), then the AudioServerPlugIn bundle out/piano-micns/PianoMicNS.driver: a thin arm64e
# Mach-O (the HAL's driver service, com.apple.audio.Core-Audio-Driver-Service.helper, is arm64e and on the tablet runs with pointer
# authentication on, so dyld accepts only arm64e slices: "incompatible architecture (have 'arm64', need 'arm64e')"; thin because the
# trust cache takes one CDHash; as NativeAdrenoMetal.bundle), macOS 14, -Wall -Wextra -Werror for this repository's files, RNNoise
# (third_party/rnnoise, BSD-3-Clause, unmodified) with its own warnings off, signed ad hoc as a bundle (codesign: the Info.plist and
# the executable sealed; the driver service refuses an invalid signature), loaded the way the driver service loads it, in an arm64e
# host process (tests/plugin_host.c built arm64e: signature check, CFBundle load, CFPlugIn factory, the device's properties; and the
# negative control: the same sources built arm64, out/piano-micns/control-arm64, must be refused with "need 'arm64e'"; an arm64 host
# would accept both slices and could not see the helper's grading). Prints the executable's CDHash (it goes into the trust cache with
# the userland layer) and writes out/piano-micns/build.json.
#   environment: CC (default xcrun clang), PIANO_MICNS_WAV (optional: a recording, mono 16-bit 24/48 kHz, through micns_host --wav)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
P=$D/package/PianoMicNS; R=$P/third_party/rnnoise; A=$D/package/PianoAudio
O=$D/out/piano-micns; T=$O/host-tests; B=$O/PianoMicNS.driver
CC=${CC:-xcrun clang}
rm -rf "$O"; mkdir -p "$T" "$O/obj"
RN=(denoise rnn rnn_data pitch kiss_fft celt_lpc)
RFLAGS=(-std=c11 -O2 -w -DNDEBUG -I "$R/include" -I "$R/src")
OURS=(-std=c11 -O2 -Wall -Wextra -Werror -I "$P" -I "$A" -I "$R/include")
# 1 host tests (this Mac's arch)
for f in "${RN[@]}"; do $CC "${RFLAGS[@]}" -c "$R/src/$f.c" -o "$T/rn-$f.o"; done
$CC "${OURS[@]}" "$P/tests/micns_host.c" "$P/MicNsCore.c" "$T"/rn-*.o -lm -o "$T/micns_host"
"$T/micns_host" || { echo "Error: PianoMicNS host tests failed" >&2; exit 1; }
$CC -std=c11 -Wall -Wextra -Werror -I "$A" "$A/tests/tap_host.c" -o "$T/tap_host" && "$T/tap_host"
$CC "${OURS[@]}" -DPIANO_MICNS_TEST_TAP "$P/tests/plugin_io_host.c" "$P/PianoMicNS.c" "$P/MicNsCore.c" "$T"/rn-*.o -framework CoreAudio \
  -framework CoreFoundation -framework IOKit -lm -o "$T/plugin_io_host"
"$T/plugin_io_host" || { echo "Error: PianoMicNS plugin_io_host failed" >&2; exit 1; }
if [ -n "${PIANO_MICNS_WAV:-}" ]; then "$T/micns_host" --wav "$PIANO_MICNS_WAV" "$O/micns-host-out.wav"; fi
# 2 the bundle: thin arm64e, macOS 14; the same sources thin arm64 as step 3's negative control (never staged)
bundle() {   # ARCH BUNDLE OBJDIR
  local a=$1 b=$2 o=$3; local ARCH=(-arch "$1" -mmacosx-version-min=14.0)
  mkdir -p "$o" "$b/Contents/MacOS"
  for f in "${RN[@]}"; do $CC "${ARCH[@]}" "${RFLAGS[@]}" -c "$R/src/$f.c" -o "$o/rn-$f.o"; done
  $CC "${ARCH[@]}" "${OURS[@]}" -c "$P/MicNsCore.c" -o "$o/MicNsCore.o"
  $CC "${ARCH[@]}" "${OURS[@]}" -c "$P/PianoMicNS.c" -o "$o/PianoMicNS.o"
  cp "$P/Info.plist" "$b/Contents/Info.plist"
  $CC "${ARCH[@]}" -bundle "$o/PianoMicNS.o" "$o/MicNsCore.o" "$o"/rn-*.o -framework CoreAudio -framework CoreFoundation \
    -framework IOKit -lm -Wl,-exported_symbol,_PianoMicNS_Create -o "$b/Contents/MacOS/PianoMicNS"
  [ "$(lipo -archs "$b/Contents/MacOS/PianoMicNS")" = "$a" ] || { echo "Error: $b is not a thin $a Mach-O" >&2; exit 1; }
  plutil -lint "$b/Contents/Info.plist" > /dev/null
  nm -u "$b/Contents/MacOS/PianoMicNS" | grep -q '_IOServiceOpen' || { echo "Error: the plug-in does not open the tap" >&2; exit 1; }
  nm -g "$b/Contents/MacOS/PianoMicNS" | grep -q ' T _PianoMicNS_Create$' || { echo "Error: no factory symbol" >&2; exit 1; }
  codesign --force --sign - --timestamp=none "$b"
  codesign --verify --strict --verbose=1 "$b" 2>&1 | tail -1
}
bundle arm64e "$B" "$O/obj"
C=$O/control-arm64/PianoMicNS.driver
bundle arm64 "$C" "$O/obj-arm64"
# 3 the signed bundle as the driver service loads it, in an arm64e host process (keys on, as the tablet's helper): the arm64e bundle
# loads and passes (signature check, CFBundle load, CFPlugIn factory, the device's properties); the arm64 control is refused
$CC -arch arm64e "${OURS[@]}" "$P/tests/plugin_host.c" -framework CoreAudio -framework CoreFoundation -framework Security -o "$T/plugin_host"
[ "$(lipo -archs "$T/plugin_host")" = arm64e ] || { echo "Error: plugin_host is not an arm64e process" >&2; exit 1; }
"$T/plugin_host" "$B" || { echo "Error: PianoMicNS plugin_host failed" >&2; exit 1; }
"$T/plugin_host" --refused "$C" ||
  { echo "Error: the arm64e host did not refuse the arm64 control with need 'arm64e': it does not grade slices as the tablet's helper" >&2; exit 1; }
CDHASH=$(codesign -dvvv "$B" 2>&1 | sed -n 's/^CDHash=\([0-9a-f]*\)$/\1/p' | head -1)
[ -n "$CDHASH" ] || { echo "Error: no CDHash" >&2; exit 1; }
python3 - "$B" "$CDHASH" "$O/build.json" "$D" <<'PY'
import hashlib, json, pathlib, subprocess, sys
b, cdhash, out, d = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
files = {str(p.relative_to(b)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(b.rglob('*')) if p.is_file()}
commit = subprocess.run(['git', '-C', d, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
json.dump({'bundle': b.name, 'cdhash': cdhash, 'files': files, 'commit': commit, 'arch': 'arm64e', 'min_os': '14.0'},
          open(out, 'w'), indent=1)
PY
echo "PianoMicNS: $B (CDHash $CDHASH, $(stat -f %z "$B/Contents/MacOS/PianoMicNS") bytes)"
