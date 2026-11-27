#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Every host test of the T-16 UEFI OS switch: the selector build and its codec/decision tests, the packer (with the real stock
# + macos-set images, read only, when they exist on this host), the shell flag tool, and the offline SW-B safety model.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 -IB tooling/switch/test-switch-safety.py
make -C firmware/selector test OUT="$out"
python3 -I tooling/switch/test-pack-selector.py "$out/selector.bin" \
  --real "${ANDROID_BOOT:-unset}" "${UEFI_BOOT:-unset}"   # ANDROID_BOOT = stock boot.img, UEFI_BOOT = a macos-set boot.img
bash tooling/switch/test-bootflag-sh.sh
bash tooling/switch/test-sign-props.sh
python3 -I tooling/switch/check-fd-patch.py
c++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I package/PianoDwc3 package/PianoDwc3/tests/bootflag_sdam_host.cpp -o "$out/bootflag_sdam_host"
"$out/bootflag_sdam_host"
echo "ALL SWITCH HOST TESTS PASS"
