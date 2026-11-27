#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0069's decisions (tooling/k27/piano-halt/piano_halt_policy.h: PE_halt_restart's PSCI call per PEHaltRestart
# type, the restart word that stops the boot guard's restart-reason write once a restart has begun), with and without the
# sanitizers (thread sanitizer for the race), and the check that patches/k27/0069 adds that header byte for byte as
# osfmk/arm64/piano_halt_policy.h.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/piano-halt.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
S=$D/tooling/k27/piano-halt
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_piano_halt_policy.c" -o "$T/test_piano_halt_policy" -lpthread
"$T/test_piano_halt_policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
  "$S/test_piano_halt_policy.c" -o "$T/test_piano_halt_policy_san" -lpthread
"$T/test_piano_halt_policy_san"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=thread -I "$S" \
  "$S/test_piano_halt_policy.c" -o "$T/test_piano_halt_policy_tsan" -lpthread
"$T/test_piano_halt_policy_tsan"
python3 "$D/tooling/k27/check-policy-header.py" osfmk/arm64/piano_halt_policy.h "$S/piano_halt_policy.h" "$D"/patches/k27/0069-*.patch
