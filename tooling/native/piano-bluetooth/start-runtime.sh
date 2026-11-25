#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang. All rights reserved.
set -eu
# The kernel registers this OID only for parsed piano-bt=5. kern.bootargs is
# clipped at 1024 bytes and can omit the ADT tail the driver actually parsed.
# -N checks the name without invoking the pending boot-result handler.
/usr/sbin/sysctl -N machdep.piano_bt_boot >/dev/null 2>&1 || exit 0
runtime_note() {
    if uptime=$(/usr/sbin/sysctl -n machdep.piano_bt_uptime_ms 2>/dev/null); then
        printf '%s [uptime_ms=%s]\n' "$*" "$uptime"
    else
        printf '%s\n' "$*"
    fi
}
runtime_note 'piano-bt-path3 runtime: kernel level5 endpoint present'
# A stock process may already have mapped the lower executable while firmware was
# loading. Stop just these jobs before making the new views visible: otherwise
# the old daemon can issue Broadcom commands in the worker's ready/start interval.
for service in com.apple.bluetoothd com.apple.BlueTool; do
    if /bin/launchctl print "system/$service" >/dev/null 2>&1; then
        /bin/launchctl bootout "system/$service" || {
            runtime_note "piano-bt-path1 runtime: service=$service bootout_failed=1"
            exit 1
        }
    fi
done
/usr/local/libexec/piano-bt-runtime-mount
# Hardware setup includes a firmware-partition wait; do not spend the daemon's
# serial-open timeout before the transport can answer. This read has no power
# side effect. Each kernel wait is bounded at 30 seconds.
i=0
ready=0
while [ "$i" -lt 4 ]; do
    boot=$(/usr/sbin/sysctl -n machdep.piano_bt_boot 2>/dev/null || printf unavailable)
    if [ "$boot" = 0 ]; then ready=1; break; fi
    i=$((i + 1))
    /bin/sleep 1
done
[ "$ready" = 1 ] || { runtime_note 'piano-bt-path1 runtime: transport_not_ready=1'; exit 1; }
# The stock job definitions retain Apple's mach services and sandbox policy.
for service in com.apple.BlueTool com.apple.bluetoothd; do
    /bin/launchctl bootstrap system "/System/Library/LaunchDaemons/$service.plist" || {
        runtime_note "piano-bt-path1 runtime: service=$service bootstrap_failed=1"
        exit 1
    }
done
if ! /bin/launchctl kickstart -k system/com.apple.BlueTool; then
    runtime_note 'piano-bt-path1 runtime: service=BlueTool kickstart_failed=1'
    exit 1
fi
if ! /bin/launchctl kickstart -k system/com.apple.bluetoothd; then
    runtime_note 'piano-bt-path1 runtime: service=bluetoothd kickstart_failed=1'
    exit 1
fi
runtime_note 'piano-bt-path1 runtime: stock services restarted on verified views'
