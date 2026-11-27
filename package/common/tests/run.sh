#!/bin/bash
# Host tests of package/common/PianoIrq.h (the INTR_CFG encoding, the PDC map, the adaptive-poll and guard state machines). The
# drivers' build scripts run it before their own tests.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
flags=(-Wno-unused-parameter)
if [[ ${1:-} == --sanitize ]]; then
  flags=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all)
fi
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror "${flags[@]}" "$P/tests/irq_host.cpp" -o "$T/irq_host"
"$T/irq_host"
