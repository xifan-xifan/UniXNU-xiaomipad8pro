#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-data-concurrent (configs/userland.toml; docs/data-store.md "Concurrent mode at boot"): switches the direct Data store to concurrent
# admission ONCE per boot, after the mount, the path qualified on the tablet (dab-r2/dab-w1, the power cut m5dur-m4cc-cut4/verify4: serial
# at mount with data-store-concurrent=2, then machdep.piano_data_concurrent=1 at run time). The boot line keeps data-store-concurrent=2.
#   enabled only with PIANO_DATA_CONCURRENT_AT_BOOT=1 in the plist's EnvironmentVariables (remove the key, or the plist, to disable it);
#   nothing when machdep.piano_data_concurrent is absent (no data-store-concurrent opt-in) or already 1;
#   waits (180 s) for the Data volume and reads the live store state from machdep.piano_data_io, then
#   switches only for state 2 (resumed) on the direct store (machdep.piano_data_io mode=single-gate); anything else: nothing switched;
#   never switches back on its own. Rollback: sudo sysctl -w machdep.piano_data_concurrent=0 (this boot), or remove the daemon.
# One "piano-data-concurrent: ..." console line with the result, the same text in /private/var/run/piano-data-concurrent.status.
# A test runs it with PIANO_DC_PATH (stub commands first), PIANO_DC_OUT, PIANO_DC_STATUS, PIANO_DC_WAIT, PIANO_DC_PERIOD.
set -u
PATH=${PIANO_DC_PATH:-/usr/sbin:/usr/bin:/sbin:/bin}
out=${PIANO_DC_OUT:-/dev/console} status=${PIANO_DC_STATUS:-/private/var/run/piano-data-concurrent.status}
wait_s=${PIANO_DC_WAIT:-180} period=${PIANO_DC_PERIOD:-2}
say() {
  # Readers see the last complete result even if this process dies while recording it.
  (
    temp=$(mktemp "${status}.XXXXXX") || exit 1
    trap 'rm -f "$temp"' EXIT
    trap 'exit 1' HUP INT TERM
    printf '%s\n' "$1" > "$temp" && chmod 0644 "$temp" && [ ! -d "$status" ] && mv -f "$temp" "$status"
  ) 2> /dev/null || :
  printf 'piano-data-concurrent: %.230s\n' "$1" 2> /dev/null >> "$out" || :
}

[ "${PIANO_DATA_CONCURRENT_AT_BOOT:-0}" = 1 ] || { say "off (PIANO_DATA_CONCURRENT_AT_BOOT is not 1): nothing switched"; exit 0; }
data_mounted() { mount 2> /dev/null | grep -q ' on /System/Volumes/Data (apfs'; }
w=0
until data_mounted; do
  [ "$w" -lt "$wait_s" ] || { say "the Data volume not mounted after ${w} s: nothing switched"; exit 0; }
  sleep "$period"; w=$((w + period))
done
# NativeRamBlock registers the sysctls before publishing the disk to APFS.
cur=$(sysctl -n machdep.piano_data_concurrent 2> /dev/null) || { say "absent (no machdep.piano_data_concurrent: no data-store-concurrent opt-in): nothing switched"; exit 0; }
[ "$cur" = 1 ] && { say "already 1: nothing switched"; exit 0; }
[ "$cur" = 0 ] || { say "unexpected concurrency mode: nothing switched"; exit 0; }
io=$(sysctl -n machdep.piano_data_io 2> /dev/null) || io=
state=$(printf '%s\n' "$io" | sed -n 's/.* store_state=\([0-9][0-9]*\)$/\1/p')
[ "$state" = 2 ] || { say "store state ${state:-unread} (not 2, resumed) after ${w} s: nothing switched"; exit 0; }
case "$io" in
  'mode=single-gate '*) ;;
  *) say "machdep.piano_data_io is not the direct store in single-gate mode (${io%% inflight*}): nothing switched"; exit 0 ;;
esac
data_mounted || { say "the Data volume disappeared before the switch: nothing switched"; exit 0; }
written=0
sysctl -w machdep.piano_data_concurrent=1 > /dev/null 2>&1 && written=1
cur=$(sysctl -n machdep.piano_data_concurrent 2> /dev/null) || cur=
if [ "$written" = 1 ] && [ "$cur" = 1 ]; then
  io=$(sysctl -n machdep.piano_data_io 2> /dev/null)
  say "switched: machdep.piano_data_concurrent=1 (store state 2, ${io%% inflight*}) after ${w} s; rollback: sudo sysctl -w machdep.piano_data_concurrent=0"
else
  say "switch not confirmed: machdep.piano_data_concurrent=${cur:-unreadable}"
fi
exit 0
