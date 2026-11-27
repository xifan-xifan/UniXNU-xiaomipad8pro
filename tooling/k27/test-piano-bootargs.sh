#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0066's string handling (tooling/k27/piano-bootargs/piano_bootargs_policy.h: the set a selector names, the merge
# of DarwinBoot's line and the ADT's tail, kern.bootargs' clip), with and without the sanitizers; the check that patches/k27/0066
# adds that header byte for byte as pexpert/arm/piano_bootargs_policy.h; and the host side (tooling/adt/boot_args.py: the split, the
# ADT property, the kernel's merge redone) against the same numbers.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/piano-bootargs.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
S=$D/tooling/k27/piano-bootargs
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_piano_bootargs_policy.c" -o "$T/test_piano_bootargs_policy"
"$T/test_piano_bootargs_policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
  "$S/test_piano_bootargs_policy.c" -o "$T/test_piano_bootargs_policy_san"
"$T/test_piano_bootargs_policy_san"
python3 "$D/tooling/k27/check-policy-header.py" pexpert/arm/piano_bootargs_policy.h "$S/piano_bootargs_policy.h" "$D"/patches/k27/0066-*.patch
python3 "$D/tooling/adt/boot_args.py" --self-test
