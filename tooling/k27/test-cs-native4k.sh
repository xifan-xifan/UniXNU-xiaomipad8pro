#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0067's work split (tooling/k27/cs-native4k/cs_native4k_policy.h: chunks, claiming, the first error, the 4 KiB table
# equal to the one-thread pass with 1-16 threads), with and without the sanitizers, and the check that patches/k27/0067 adds that
# header byte for byte as bsd/kern/cs_native4k_policy.h.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/cs-native4k.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
S=$D/tooling/k27/cs-native4k
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_cs_native4k_policy.c" -o "$T/test_cs_native4k_policy" -lpthread
"$T/test_cs_native4k_policy"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
  "$S/test_cs_native4k_policy.c" -o "$T/test_cs_native4k_policy_san" -lpthread
"$T/test_cs_native4k_policy_san"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=thread -I "$S" \
  "$S/test_cs_native4k_policy.c" -o "$T/test_cs_native4k_policy_tsan" -lpthread
"$T/test_cs_native4k_policy_tsan"
python3 "$D/tooling/k27/check-policy-header.py" bsd/kern/cs_native4k_policy.h "$S/cs_native4k_policy.h" "$D"/patches/k27/0067-*.patch
