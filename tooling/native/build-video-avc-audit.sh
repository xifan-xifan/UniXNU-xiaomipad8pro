#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host-only CoreMedia audit; never opens a decoder or contacts a device.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
O=${1:-$D/out/native-tools/video-avc-audit}
mkdir -p "$(dirname "$O")"
xcrun clang++ -std=c++17 -arch arm64 -mmacosx-version-min=15.0 -O2 -Wall -Wextra -Werror \
  -Wno-deprecated-declarations -fobjc-arc -I "$D/package/PianoVideo" "$D/tooling/native/video-avc-audit.mm" \
  -framework Foundation -framework AVFoundation -framework CoreFoundation -framework CoreMedia -framework CoreVideo -o "$O"
python3 "$D/package/PianoVideo/tests/avc_files_host.py" "$O"
