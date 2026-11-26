#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Run COMMAND with the 27.3 line's KC base present, then delete it, whatever COMMAND does. The ONLY IPSW member fetched is the
# kernelcache (kernelcache.release.mac17j, 31 MiB, by HTTP range requests from Apple's CDN: tooling/k273/ipsw-members.py); it is
# decompressed and checked against the profile's pin (tooling/k273/decompress-kernelcache.py), placed where the build reads it, and
# removed with its download on exit. CI must never cache or upload it (docs/ci.md): keep it out of every actions/cache and
# actions/upload-artifact path (this script puts it under $KC_DIR, default $RUNNER_TEMP or /tmp, never in the workspace).
#
#   tooling/ci/with-kernelcache.sh COMMAND [ARGS...]      COMMAND sees K273_KC=<the KC base> and K273_INPUTS=<its inputs folder>
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || { echo "usage: with-kernelcache.sh COMMAND [ARGS...]" >&2; exit 2; }
BASE=${KC_DIR:-${RUNNER_TEMP:-/tmp}}
W=$(mktemp -d "$BASE/piano-kc.XXXXXX")
cleanup() {
  rm -rf "$W"
  [ ! -e "$W" ] && echo "with-kernelcache: removed the kernelcache and its download ($W)"
}
trap cleanup EXIT INT TERM
python3 "$D/tooling/k273/ipsw-members.py" fetch --out "$W/ipsw" KernelCache
IM4P=$(find "$W/ipsw" -name 'kernelcache.release.mac17j' -type f | head -1)
[ -n "$IM4P" ] || { echo "with-kernelcache: no kernelcache.release.mac17j fetched" >&2; exit 1; }
mkdir -p "$W/inputs/kc"
python3 "$D/tooling/k273/decompress-kernelcache.py" "$IM4P" "$W/inputs/kc/kc273-mac17j.bin"
rm -rf "$W/ipsw"
export K273_INPUTS=$W/inputs K273_KC=$W/inputs/kc/kc273-mac17j.bin
"$@"
