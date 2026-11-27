#!/bin/bash
# Run PianoPower's portable IPC, firmware, telemetry and contract tests.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/piano-power-host.XXXXXXXX")
trap 'rm -rf "$T"' EXIT
flags=(-Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$P" -I "$P/../include")
cc=("${CC:-cc}" -std=c11 "${flags[@]}")
cxx=("${CXX:-c++}" -std=c++20 "${flags[@]}")

# Compile C sources as C, including the dependencies of the mixed C++ suites.
for source in ChargePolicy FatVolume PianoMca QcomMdt QcomQmp QcomQrtr QcomRpmh QcomSmem QcomSmp2p QrtrPorts Sm8750Scm UsbcRelay; do
    "${cc[@]}" -c "$P/$source.c" -o "$T/$source.o"
done
"${cc[@]}" -Wshorten-64-to-32 -c "$P/QcomGlink.c" -o "$T/QcomGlink.o"
"${cc[@]}" -DPIANO_PORT_EVENTS_TEST -c "$P/PortEvents.c" -o "$T/PortEvents.o"

run() {
    local source=$1
    shift
    local name=${source%.*} obj
    local -a objects=()
    for obj in "$@"; do
        objects+=("$T/$obj.o")
    done
    printf 'Running %s\n' "$name"
    case "$source" in
        *.c) "${cc[@]}" "$P/tests/$source" ${objects[@]+"${objects[@]}"} -o "$T/$name" ;;
        *.cpp) "${cxx[@]}" "$P/tests/$source" ${objects[@]+"${objects[@]}"} -o "$T/$name" ;;
    esac
    "$T/$name"
}

run battery_host.cpp
run contract_host.cpp
run contract_init_host.cpp
run contract_link_host.cpp ChargePolicy PianoMca
run glink_return_host.c QcomGlink
run observe_host.cpp
run pdo_firmware_host.cpp ChargePolicy PianoMca
run platform_host.c FatVolume QcomMdt QcomQmp QcomRpmh QcomSmem QcomSmp2p Sm8750Scm
run pon_history_host.cpp
run port_events_host.cpp PortEvents PianoMca
run ports_host.cpp QrtrPorts QcomQrtr PianoMca QcomGlink
run power_host.cpp PianoMca ChargePolicy QcomQrtr
run trace_host.cpp
run usbc_host.cpp UsbcRelay PianoMca
