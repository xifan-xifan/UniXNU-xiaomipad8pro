#!/bin/bash
# Portable policy, wire/contract, gauge bus and battery regression tests.
set -euo pipefail

D=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=scripts/ci/host-test-jobs.sh
source "$D/scripts/ci/host-test-jobs.sh"
P=$D/package/PianoPower
T=$(mktemp -d "${TMPDIR:-/tmp}/piano-charge-host.XXXXXXXX")
trap 'rm -rf -- "$T"' EXIT
for f in PianoMca ChargePolicy QcomQrtr; do
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -c "$P/$f.c" -o "$T/$f.o"
done

san=(-g '-fsanitize=address,undefined')

compile_and_run() {
  local test=$1
  shift
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$P" "$P/tests/${test%_san}.cpp" "$@" -o "$T/$test"
  "$T/$test"
}

run_charge_test() {
  case $1 in
    contract_link_host|pdo_firmware_host|fastcharge_host)
      compile_and_run "$1" -I "$D/package/include" "$T/ChargePolicy.o" "$T/PianoMca.o"
      ;;
    policy_host)
      compile_and_run "$1" "$T/ChargePolicy.o"
      ;;
    power_host)
      compile_and_run "$1" -I "$D/package/include" "$T/PianoMca.o" "$T/ChargePolicy.o" "$T/QcomQrtr.o"
      ;;
    cp_driver_host_san)
      compile_and_run "$1" "${san[@]}"
      ;;
    cp_session_host)
      "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I "$P" -c "$P/ChargePumpSession.c" -o "$T/ChargePumpSession.o"
      compile_and_run "$1" -I "$D/package/include" "$T/ChargePumpSession.o" "$T/ChargePolicy.o"
      ;;
    cp_session_host_san)
      "${CC:-cc}" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$P" -c "$P/ChargePumpSession.c" -o "$T/ChargePumpSession_san.o"
      "${CC:-cc}" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$P" -c "$P/ChargePolicy.c" -o "$T/ChargePolicy_san.o"
      compile_and_run "$1" "${san[@]}" -I "$D/package/include" "$T/ChargePumpSession_san.o" "$T/ChargePolicy_san.o"
      ;;
    *)
      compile_and_run "$1"
      ;;
  esac
}

run_charge_sim() {
  # T-CP fork B: 45 scenarios, 17 guard mutants, 10 integrated sensitivity cases and randomized fuzz.
  # The full qualification runs `cp_sim_host 10000`. This suite runs both fuzz loops at 1000, with ASan/UBSan.
  local f
  for f in ChargePumpSession ChargeMipps ChargePolicy PianoMca; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror "${san[@]}" -I "$P" -I "$D/package/include" -c "$P/$f.c" -o "$T/${f}_cpsim.o" || return
  done
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -O1 "${san[@]}" -I "$P" -I "$D/package/include" "$P/tests/cp_sim_host.cpp" \
    "$T/ChargePumpSession_cpsim.o" "$T/ChargeMipps_cpsim.o" "$T/ChargePolicy_cpsim.o" "$T/PianoMca_cpsim.o" -o "$T/cp_sim_host" || return
  CPSIM_REAL_FUZZ=1000 "$T/cp_sim_host" 1000
}

tests=(power_host policy_host contract_link_host pdo_firmware_host fastcharge_host contract_host contract_init_host
       cp_driver_host cp_driver_host_san cp_session_host cp_session_host_san battery_host)
jobs=$(host_job_limit)
if (( jobs == 1 )); then
  run_host_jobs "$T" run_charge_test "${tests[@]}"
  run_charge_sim
else
  # Keep a slot for the simulator for its entire run. The shorter jobs use only the other slots.
  CI_SUBJOBS=$((jobs - 1)) run_host_jobs "$T" run_charge_test "${tests[@]}" > "$T/other-tests.log" 2>&1 &
  other_tests=$!
  simulator_status=0
  run_charge_sim || simulator_status=$?
  other_status=0
  if wait "$other_tests"; then
    cat "$T/other-tests.log"
  else
    other_status=$?
    cat "$T/other-tests.log" >&2
  fi
  if (( simulator_status )); then
    printf 'cp_sim_host failed (exit %d)\n' "$simulator_status" >&2
    exit "$simulator_status"
  fi
  exit "$other_status"
fi
