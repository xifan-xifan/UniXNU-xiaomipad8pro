#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
native=$(cd "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
flags=(-Wall -Wextra -Werror '-fsanitize=address,undefined' -fno-sanitize-recover=all)

make -C "$native/gop-shot" OUT="$scratch/gop" CFLAGS="-std=c11 ${flags[*]}" test
make -C "$native/piano-brightness-control" OUT="$scratch/brightness" CXXFLAGS="-std=c++20 ${flags[*]}" test

xcrun clang "${flags[@]}" "$native/piano-wifi-controls/tests/desktop_gate_host.c" -o "$scratch/desktop-gate"
"$scratch/desktop-gate"
xcrun clang "${flags[@]}" "$native/piano-wifi-controls/tests/country_probe_host.c" \
    -framework CoreFoundation -framework SystemConfiguration -o "$scratch/country-probe"
"$scratch/country-probe"
xcrun clang "${flags[@]}" "$native/piano-wifi-supplicant/piano-wifi-supplicant.c" -o "$scratch/supplicant"
"$scratch/supplicant" --self-test
xcrun clang "${flags[@]}" "$native/piano-charge-config/tests/config_host.c" \
    -framework CoreFoundation -framework IOKit -o "$scratch/charge-config"
"$scratch/charge-config" "$scratch/saved-config"
