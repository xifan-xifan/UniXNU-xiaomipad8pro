#!/bin/bash
# Portable policy, wire/contract, gauge bus and battery regression tests.
set -euo pipefail
D=$(cd "$(dirname "$0")/../../.." && pwd)
P=$D/package/PianoPower
T=$D/out/chg-host
mkdir -p "$T"
for f in PianoMca ChargePolicy QcomQrtr; do
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/$f.c" -o "$T/$f.o"
done
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/contract_host.cpp" -o "$T/contract_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/contract_link_host.cpp" \
  "$T/ChargePolicy.o" "$T/PianoMca.o" -o "$T/contract_link_host"
"$T/contract_link_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/pdo_firmware_host.cpp" \
  "$T/ChargePolicy.o" "$T/PianoMca.o" -o "$T/pdo_firmware_host"
"$T/pdo_firmware_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/fastcharge_host.cpp" \
  "$T/ChargePolicy.o" "$T/PianoMca.o" -o "$T/fastcharge_host"   # charge/pd1: the buck fast-charge set (charging-policy.md §21)
"$T/fastcharge_host"
"$T/contract_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/contract_init_host.cpp" -o "$T/contract_init_host"
"$T/contract_init_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/policy_host.cpp" "$T/ChargePolicy.o" -o "$T/policy_host"
"$T/policy_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/power_host.cpp" \
  "$T/PianoMca.o" "$T/ChargePolicy.o" "$T/QcomQrtr.o" -o "$T/power_host"
"$T/power_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/cp_driver_host.cpp" -o "$T/cp_driver_host"   # the SC8541 driver (ChargePump.h)
"$T/cp_driver_host"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I "$P" "$P/tests/cp_driver_host.cpp" -o "$T/cp_driver_host_san"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$P" -c "$P/ChargePumpSession.c" -o "$T/ChargePumpSession.o"   # the pump session
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" -I "$D/package/include" "$P/tests/cp_session_host.cpp" "$T/ChargePumpSession.o" \
  "$T/ChargePolicy.o" -o "$T/cp_session_host"
"$T/cp_session_host"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I "$P" -c "$P/ChargePumpSession.c" -o "$T/ChargePumpSession_san.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I "$P" -c "$P/ChargePolicy.c" -o "$T/ChargePolicy_san.o"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I "$P" -I "$D/package/include" \
  "$P/tests/cp_session_host.cpp" "$T/ChargePumpSession_san.o" "$T/ChargePolicy_san.o" -o "$T/cp_session_host_san"
"$T/cp_session_host_san"
"$T/cp_driver_host_san"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/battery_host.cpp" -o "$T/battery_host"
"$T/battery_host"
# T-CP fork B: the charge pump simulator (cp_sim.h plant + checker, cp_ref.h reference, cp_real.h the integrated driver/session/MiPPS):
# 45 scenarios, 17 guard mutants, the integrated code under 10 sensitivity cases, and randomized fuzz (1000 episodes here; the full
# qualification runs `cp_sim_host 10000`). ASan/UBSan.
for f in ChargePumpSession ChargeMipps ChargePolicy PianoMca; do
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I "$P" -I "$D/package/include" -c "$P/$f.c" -o "$T/${f}_cpsim.o"
done
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I "$P" -I "$D/package/include" "$P/tests/cp_sim_host.cpp" \
  "$T/ChargePumpSession_cpsim.o" "$T/ChargeMipps_cpsim.o" "$T/ChargePolicy_cpsim.o" "$T/PianoMca_cpsim.o" -o "$T/cp_sim_host"
CPSIM_REAL_FUZZ=1000 "$T/cp_sim_host" 1000
