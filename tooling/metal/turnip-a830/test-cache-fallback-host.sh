#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host check of plugin 0012 (tooling/metal/turnip-a830/plugin/0012-plugin-open-without-disk-cache.patch), on the build Mac over the
# hostsim, through the plugin's own deviceForPort (cache_fallback_host.mm):
#   tooling/metal/turnip-a830/test-cache-fallback-host.sh BUNDLE SHIM [BASELINE_BUNDLE]
# BUNDLE: the NativeAdrenoMetal.bundle with 0012 (tooling/metal/build-turnip-a830.sh); SHIM: the hostsim's macOS IOKit shim
# (check-turnip-a830-host.py's out/turnip-a830/hostsim/macos/libhostiokit.dylib); BASELINE_BUNDLE: optional, a plugin without 0012
# (the installed 0011 build ee106443) for the known-negative.
#   sandboxed:  sandbox-exec denies every file write in the user cache directory, the user temp directory and /private/var/tmp
#               (all five of the plugin's candidates, as Chrome's gpu.sb does for the GPU helper): the plugin says
#               "no writable cache directory: opening without the m2v disk cache", gets its device, makes the pipeline twice, the
#               dispatch completes, and nothing is written in its first candidate;
#   writable:   no sandbox: the plugin opens in its first candidate (<user cache>com.apple.metal/cache_fallback_host/m2v) without the
#               fallback line, and the runtime writes its entries there (the disk cache of every process that may write: unchanged);
#   baseline:   (with BASELINE_BUNDLE) the same sandbox on the plugin without 0012: "no device: " and no device (the gate 14.7 found).
# Exit 0 when all hold. Output: OUT (default /tmp/cache-fallback-host-$$).
set -euo pipefail
D=$(cd "$(dirname "$0")/../../.." && pwd)
BUNDLE=${1:?bundle}; SHIM=${2:?hostsim shim}; BASELINE=${3:-}
OUT=${OUT:-/tmp/cache-fallback-host-$$}; mkdir -p "$OUT"
S=$D/tooling/metal/turnip-a830
xcrun clang++ -target arm64e-apple-macos14.0 -fobjc-arc -O1 -std=c++17 -x objective-c++ "$S/cache_fallback_host.mm" \
  -o "$OUT/cache_fallback_host" -framework Foundation -framework Metal
real() { python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"; }
CACHE=$(real "$(getconf DARWIN_USER_CACHE_DIR)"); TEMP=$(real "$(getconf DARWIN_USER_TEMP_DIR)")
FIRST=$CACHE/com.apple.metal/cache_fallback_host/m2v
printf '(version 1)\n(allow default)\n(deny file-write* (subpath "%s") (subpath "%s") (subpath "/private/var/tmp"))\n' \
  "$CACHE" "$TEMP" > "$OUT/deny-cache.sb"
run() {   # run LABEL SANDBOXED(0|1) BUNDLE ARGS...: the plugin's device path over the hostsim, stdout OUT/LABEL.txt, stderr OUT/LABEL.err
  local label=$1 boxed=$2 bundle=$3; shift 3
  # The environment is set by env(1) inside the sandbox: dyld drops DYLD_INSERT_LIBRARIES at sandbox-exec (a platform binary), and
  # without the shim the host's own AGXAccelerator answers (0xe00002c7).
  local cmd=(/usr/bin/env -u METAL2VULKAN_SHADER_CACHE_OPTIONAL DYLD_INSERT_LIBRARIES=$SHIM HOSTSIM_GPU=a830 HOSTSIM_PAGE_SIZE=4096
             HOSTSIM_VA=16 MESA_LOG= "$OUT/cache_fallback_host" "$bundle/Contents/MacOS/NativeAdrenoMetal" "$@")
  if [ "$boxed" = 1 ]; then cmd=(sandbox-exec -f "$OUT/deny-cache.sb" "${cmd[@]}"); fi
  "${cmd[@]}" > "$OUT/$label.txt" 2> "$OUT/$label.err" && echo "$label: rc 0" || echo "$label: rc $?"
}
fail=0
want() {   # want FILE TEXT: TEXT is in FILE
  if grep -qF -- "$2" "$1"; then echo "PASS $(basename "$1"): $2"; else echo "FAIL $(basename "$1"): no '$2'"; fail=1; fi
}
wantnot() {
  if grep -qF -- "$2" "$1"; then echo "FAIL $(basename "$1"): has '$2'"; fail=1; else echo "PASS $(basename "$1"): no '$2'"; fi
}
LINE='NativeAdrenoMetal: cache_fallback_host: no writable cache directory: opening without the m2v disk cache'
rm -rf "$CACHE/com.apple.metal/cache_fallback_host"
run sandboxed 1 "$BUNDLE"
want "$OUT/sandboxed.txt" 'RESULT PASS'
want "$OUT/sandboxed.txt" 'device: Adreno'
want "$OUT/sandboxed.err" "$LINE"
want "$OUT/sandboxed.err" 'NativeAdrenoMetal: cache_fallback_host: device Adreno'
wantnot "$OUT/sandboxed.err" 'without a disk cache refused'
if [ -e "$CACHE/com.apple.metal/cache_fallback_host" ]; then echo "FAIL sandboxed: wrote $CACHE/com.apple.metal/cache_fallback_host"; fail=1
else echo "PASS sandboxed: nothing written in the first candidate"; fi
run writable 0 "$BUNDLE"
want "$OUT/writable.txt" 'RESULT PASS'
want "$OUT/writable.err" 'NativeAdrenoMetal: cache_fallback_host: device Adreno'
wantnot "$OUT/writable.err" 'no writable cache directory'
entries=$(find "$FIRST/v1" -type f 2>/dev/null | wc -l | tr -d ' ')
if [ "${entries:-0}" -gt 0 ]; then echo "PASS writable: $entries entries in $FIRST/v1 (the disk cache)"; else echo "FAIL writable: no entries in $FIRST/v1"; fail=1; fi
if [ -n "$BASELINE" ]; then
  rm -rf "$CACHE/com.apple.metal/cache_fallback_host"
  run baseline 1 "$BASELINE" --expect-no-device
  want "$OUT/baseline.txt" 'no device'
  want "$OUT/baseline.txt" 'RESULT PASS'
  want "$OUT/baseline.err" 'NativeAdrenoMetal: cache_fallback_host: no device: '
  wantnot "$OUT/baseline.err" 'no writable cache directory'
fi
rm -rf "$CACHE/com.apple.metal/cache_fallback_host"
echo "output: $OUT"
[ $fail = 0 ] && echo "RESULT PASS" || echo "RESULT FAIL"
exit $fail
