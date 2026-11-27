#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0063's decisions (tooling/k27/piano-guard/piano_guard_policy.h: the watchdog's holders, the boot guard's health
# decision and tick schedule, piano-boot-guard-test, the milestone's name, machdep.piano_boot_guard), with and without the
# sanitizers, and the check that patches/k27/0063 adds that header byte for byte as osfmk/arm64/piano_guard_policy.h.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/piano-guard.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
S=$D/tooling/k27/piano-guard
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_piano_guard_policy.c" -o "$T/test_piano_guard_policy"
"$T/test_piano_guard_policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
  "$S/test_piano_guard_policy.c" -o "$T/test_piano_guard_policy_san"
"$T/test_piano_guard_policy_san"
python3 "$D/tooling/k27/check-policy-header.py" osfmk/arm64/piano_guard_policy.h "$S/piano_guard_policy.h" "$D"/patches/k27/0063-*.patch
