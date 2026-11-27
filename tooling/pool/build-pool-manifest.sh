#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Build tooling/pool/pool-manifest.c for the Mac and the tablet (one arm64 binary, ad-hoc signed): OUT (default out/pool/pool-manifest)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$D/out/pool/pool-manifest}
mkdir -p "$(dirname "$OUT")"
xcrun clang -O2 -Wall -Wextra -Werror -arch arm64 -mmacosx-version-min=14.0 -o "$OUT.new" "$D/tooling/pool/pool-manifest.c"
codesign -s - -f "$OUT.new" > /dev/null 2>&1
mv -f "$OUT.new" "$OUT"
shasum -a 256 "$OUT"
