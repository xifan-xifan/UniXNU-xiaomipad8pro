#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 -fobjc-arc -Wall -Wextra -Werror \
    "$here/gui.m" "$here/movie.m" -o "${1:?native probe output path}" \
    -framework AppKit -framework ApplicationServices -framework AVFoundation -framework CoreMediaIO \
    -framework CoreMedia -framework CoreVideo -framework IOKit
