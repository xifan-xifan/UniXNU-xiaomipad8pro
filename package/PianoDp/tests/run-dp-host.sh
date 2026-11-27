#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

CDPATH='' cd -- "$(dirname -- "$0")/../../.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/piano-dp-host.XXXXXX")
trap 'rm -r -- "$build_dir"' EXIT

python3 tooling/dp/dp-phy-tables.py header data/dp "$build_dir/DpQmpTables.h"
checks=(-std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
includes=(-I package/PianoDp -I package/PianoDpu -I package/PianoUsbHost -I package/PianoDwc3)
for test in dp_host dp_d0_host; do
  "${CXX:-c++}" "${checks[@]}" "${includes[@]}" -I "$build_dir" \
    "package/PianoDp/tests/$test.cpp" -o "$build_dir/$test"
  "$build_dir/$test"
done
