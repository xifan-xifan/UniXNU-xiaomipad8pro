#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Exercise the installer's detached-step receipt handling with no device connection.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/pool-host.XXXXXX")
trap 'rm -rf "$T"' EXIT
source "$D/pool-host.sh"

PR=$T/remote
ROUND=$T
POOL_DATA_UUID=fixture
nap() { :; }
say() { printf '%s\n' "$1"; }
rsudo() {
    case "$1" in
        'cat '*".rc'"*) [ "$reply" != missing ] && printf '%s\n' "$reply" ;;
        'cat '*".out'"*) echo 'detached step output' ;;
        *) [ "$reply" != disconnected ] ;;
    esac
}

reply=0
dev_step extract os 0
reply=17
if dev_step extract os 0; then exit 1; fi
test ! -e "$ROUND/phase"
for reply in missing disconnected 'incomplete 0' 256; do
    rc=0
    ( dev_step extract os 0 || touch "$T/rollback" ) || rc=$?
    test "$rc" = 3
    test "$(cat "$ROUND/phase")" = recovery
    test ! -e "$T/rollback"
    grep -qx 'detached step output' "$ROUND/device-extract-os.log"
done
