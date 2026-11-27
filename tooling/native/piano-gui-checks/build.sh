#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd -P)
exec xcrun clang++ -std=c++17 -arch arm64 -fobjc-arc -mmacosx-version-min=14.0 -Wall -Wextra -Werror \
    -I "$here/../../../package/PianoWifi" "$here/gui.mm" -o "${1:?native helper output path}" \
    -framework AppKit -framework ApplicationServices -framework CoreWLAN
