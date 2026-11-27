#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
dir=$(cd "$(dirname "$0")" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

compiler=${CXX:-c++}
flags=(-std=c++17 -Wall -Wextra -Werror)
"$compiler" "${flags[@]}" "$dir/diagnostics_test.cpp" -o "$scratch/plain"
"$scratch/plain"
echo "m120 diagnostics C++ plain passed"

"$compiler" "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer "$dir/diagnostics_test.cpp" -o "$scratch/sanitized"
"$scratch/sanitized"
echo "m120 diagnostics C++ ASan/UBSan passed"
