#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of k27 0061/0062's decisions (tooling/k27/piano-pm/piano_pm_policy.h: the idle state choice with its domains and
# latency holds, the PSCI and LLCC verdicts, the wake sources, the Type-C address, the wake calendar) and of 0064's
# (piano_s2idle_policy.h: the LLCC island read only at an ss3 decision, a driver's cancel of the suspend in progress), with and
# without the sanitizers, and the check that patches/k27/0061 and 0064 add those headers byte for byte under osfmk/arm64/.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/piano-pm.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
S=$D/tooling/k27/piano-pm
for t in test_piano_pm_policy test_piano_s2idle_policy; do
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/$t.c" -o "$T/$t"
  "$T/$t"
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
    "$S/$t.c" -o "$T/${t}_san"
  "$T/${t}_san"
done
for pair in 0061:piano_pm_policy.h 0064:piano_s2idle_policy.h; do
  python3 "$D/tooling/k27/check-policy-header.py" "osfmk/arm64/${pair#*:}" "$S/${pair#*:}" \
    "$D"/patches/k27/"${pair%%:*}"-*.patch
done
# --kext: package/include/PianoPowerContract.h compiled as a kext (the k27 objects' recipe, -Werror), and with K27_KERNEL set
# (a 0062 kernel) its imports resolved against that kernel.
if [ "${1:-}" = --kext ]; then
  if ! python3 "$D/tooling/k27/build-native-kext.py" PianoPowerContractCheck "$S/contract_check.cpp" --out "$T/contract-check" \
      > "$T/contract-check.log" 2>&1; then
    cat "$T/contract-check.log" >&2
    exit 1
  fi
  grep -v -e CCC_OVERRIDE -e 'Adding argument' "$T/contract-check.log" || true
  [ -f "$T/contract-check/contract_check.o" ] || { echo "test-piano-pm: PianoPowerContract.h does not compile as a kext" >&2; exit 1; }
  if [ -n "${K27_KERNEL:-}" ]; then
    python3 -c 'import json, sys; print(json.dumps(sys.argv[1:]))' "$T/contract-check/contract_check.o" > "$T/contract-check/objects.json"
    python3 "$D/tooling/k27/check-kext-imports.py" "$T/contract-check/objects.json" "$K27_KERNEL"
  fi
fi
