#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host check of frontend 0011 (MTLGPUFamilyMac2 for the browsers' GPU images alone), on the build Mac over the hostsim:
#   tooling/metal/turnip-a830/test-family-host.sh BUNDLE SHIM [METALLIB]
# BUNDLE: a NativeAdrenoMetal.bundle (tooling/metal/build-turnip-a830.sh); SHIM: the hostsim's IOKit shim, the macOS kernel build
# (check-turnip-a830-host.py's out/turnip-a830/hostsim/macos/libhostiokit.dylib); METALLIB: optional, a .metallib to make as ANGLE makes
# its internal shaders (carved from a browser on this Mac; nothing of a browser is kept in this repository).
# Three runs of family_host on the bundle's own device:
#   as-angle:  asked through family_asker.m built as .../libGLESv2.dylib (ANGLE's image name): Mac2 = 1, ANGLE's gates PASS;
#   as-other:  the same asker built as .../libFamilyOther.dylib: Mac2 = 0 (every other client keeps the device's answer);
#   self:      family_host's own calls: Mac2 = 0 in both runs (checked by --expect-*).
# The Apple families stay as the device answers them for every caller (Apple1-3 = 1, Apple4+ = 0), checked here as well.
# Exit 0 when all hold. Output: OUT (default /tmp/family-host-$$).
set -euo pipefail
D=$(cd "$(dirname "$0")/../../.." && pwd)
BUNDLE=${1:?bundle}; SHIM=${2:?hostsim shim}; METALLIB=${3:-}
OUT=${OUT:-/tmp/family-host-$$}; mkdir -p "$OUT/as-angle" "$OUT/as-other" "$OUT/cache"
S=$D/tooling/metal/turnip-a830
CC=(xcrun clang++ -target arm64e-apple-macos14.0 -fobjc-arc -O1)
"${CC[@]}" -std=c++17 -x objective-c++ "$S/family_host.mm" -o "$OUT/family_host" -framework Foundation -framework Metal
for n in as-angle/libGLESv2.dylib as-other/libFamilyOther.dylib; do
  xcrun clang -target arm64e-apple-macos14.0 -fobjc-arc -O1 -dynamiclib -x objective-c "$S/family_asker.m" -o "$OUT/$n" -framework Metal
done
M=$BUNDLE/Contents/MacOS
run() {   # run LABEL ARGS...: family_host over the hostsim, its output in OUT/LABEL.txt
  local label=$1; shift
  DYLD_INSERT_LIBRARIES=$SHIM HOSTSIM_GPU=a830 HOSTSIM_PAGE_SIZE=4096 HOSTSIM_VA=16 MESA_LOG= \
    "$OUT/family_host" "$@" "$M/NativeAdrenoMetal" "$M/libvulkan_freedreno.dylib" "$OUT/cache" ${METALLIB:+"$METALLIB"} \
    > "$OUT/$label.txt" 2> "$OUT/$label.err" && echo "$label: rc 0" || echo "$label: rc $?"
}
fail=0
run as-angle --as "$OUT/as-angle/libGLESv2.dylib" --expect-mac2 || true
run as-other --as "$OUT/as-other/libFamilyOther.dylib" --expect-no-mac2 || true
# The executable itself under an image's path (no library: the pad's way): ANGLE's name gets Mac2, WebGPU.framework's Apple4 (the
# mechanism's known-positive, frontend familyForCaller before 0011).
mkdir -p "$OUT/exe/as-angle" "$OUT/exe/WebGPU.framework"
cp "$OUT/family_host" "$OUT/exe/as-angle/libGLESv2.dylib"; cp "$OUT/family_host" "$OUT/exe/WebGPU.framework/WebGPU"
runexe() {   # runexe LABEL EXE ARGS...
  local label=$1 exe=$2; shift 2
  DYLD_INSERT_LIBRARIES=$SHIM HOSTSIM_GPU=a830 HOSTSIM_PAGE_SIZE=4096 HOSTSIM_VA=16 MESA_LOG= \
    "$exe" "$@" "$M/NativeAdrenoMetal" "$M/libvulkan_freedreno.dylib" "$OUT/cache" > "$OUT/$label.txt" 2> "$OUT/$label.err" \
    && echo "$label: rc 0" || echo "$label: rc $?"
}
runexe exe-angle "$OUT/exe/as-angle/libGLESv2.dylib" --expect-mac2
runexe exe-webgpu "$OUT/exe/WebGPU.framework/WebGPU"
runexe exe-self "$OUT/family_host" --expect-no-mac2
want() {   # want FILE LINE: the exact line is in FILE
  if grep -qxF "$2" "$1"; then echo "PASS $(basename "$1" .txt): $2"; else echo "FAIL $(basename "$1" .txt): no '$2'"; fail=1; fi
}
for f in as-angle as-other; do
  want "$OUT/$f.txt" 'RESULT PASS'
  for fam in Apple1 Apple2 Apple3; do want "$OUT/$f.txt" "family $fam = 1"; want "$OUT/$f.txt" "self family $fam = 1"; done
  for fam in Apple4 Apple7 Metal3; do want "$OUT/$f.txt" "family $fam = 0"; want "$OUT/$f.txt" "self family $fam = 0"; done
  want "$OUT/$f.txt" 'self family Mac2 = 0'
done
want "$OUT/as-angle.txt" 'family Mac2 = 1'
want "$OUT/as-angle.txt" 'gate angle-metal-renderer-available (default device in Mac2) = PASS'
want "$OUT/as-angle.txt" 'gate angle-require-gpu-family2 (Apple1 or Mac2) = PASS'
want "$OUT/as-angle.txt" 'angle es version = 3.0 (WebGL2)'
want "$OUT/as-angle.txt" 'dawn limits row = Mac2'
want "$OUT/exe-angle.txt" 'family Mac2 = 1'
want "$OUT/exe-angle.txt" 'family Apple4 = 0'
want "$OUT/exe-angle.txt" 'RESULT PASS'
want "$OUT/exe-webgpu.txt" 'family Apple4 = 1'
want "$OUT/exe-webgpu.txt" 'family Mac2 = 0'
want "$OUT/exe-self.txt" 'family Mac2 = 0'
want "$OUT/exe-self.txt" 'RESULT PASS'
want "$OUT/as-other.txt" 'family Mac2 = 0'
want "$OUT/as-other.txt" 'gate angle-metal-renderer-available (default device in Mac2) = FAIL'
want "$OUT/as-other.txt" 'dawn limits row = Apple3'
[ -n "$METALLIB" ] && want "$OUT/as-angle.txt" 'gate angle-internal-shaders = PASS'
echo "output: $OUT"
[ $fail = 0 ] && echo "RESULT PASS" || { echo "RESULT FAIL"; exit 1; }
