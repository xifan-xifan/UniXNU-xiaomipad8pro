#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
# PowerLog's SMC metrics agent has no SMC here: piano publishes an AppleSMC stand-in only with piano-smc=1/2, and by default there is
# no such service at all, yet PerfPowerServices (launchd's adaptive job, keepalive) polls SMCGetKeyFromIndex for a Mac model's key
# table over and over. Measured on the tablet, 2026-11-28: that one process held 45-68 % of every core, `top` never showed an idle
# tick, and everything else queued behind it (the boot-time sshd waiting minutes for a host key, the 802.1X client logging its first
# line at boot+471 s, WindowServer at six frames a second). With the job disabled and booted out: 58 % idle, load average 700 -> 17,
# `networksetup` back from tens of seconds to 0.08 s (docs/results-2026-11-28-powerlog-holds-the-machine.md).
# It is power telemetry, not power management: assertions and sleep stay with powerd, the battery readout comes from our own
# IOPMPowerSource, and the Energy Mode switch comes from the 'MSHP' stand-in (package/PianoPolicy/AppleSmc.cpp), so nothing the owner
# sees depends on this job. Nothing of Apple's is edited: the job is disabled through launchctl's own override and booted out, the
# override that was in place before us is recorded, and --remove puts it back.
# Modes: --apply (idempotent), --check, --remove. Host tests set PIANO_LAUNCHCTL to a stub and PIANO_USERLAND_ROOT for the state
# directory, and PIANO_POWERLOG_POLICY_NOCHOWN=1 to skip the root-only steps. Lines "piano-powerlog-policy: ..." on stdout.
set -u
umask 077
R=${PIANO_USERLAND_ROOT:-}
LAUNCHCTL=${PIANO_LAUNCHCTL:-launchctl}
STATE=$R/private/var/db/piano-userland/powerlog-policy
TARGET=system/com.apple.PerfPowerServices
MODE=${1:---apply}
say() { echo "piano-powerlog-policy: $*"; logger -t piano-powerlog-policy "$*" 2>/dev/null || true; }
status() {
  say "$*"
  mkdir -p "$STATE" 2> /dev/null
  printf '%s\n' "$*" > "$STATE/status.new" 2> /dev/null && mv -f "$STATE/status.new" "$STATE/status"
}
fail() { status "FAIL $*"; exit 1; }
loaded() { $LAUNCHCTL print "$TARGET" > /dev/null 2>&1; }
override() {
  # A failed print is an error, never an absent/default override, and the label is matched whole rather than as a regexp.
  values=$($LAUNCHCTL print-disabled system 2> /dev/null) || return 1
  printf '%s\n' "$values" | awk '$1 == "\"com.apple.PerfPowerServices\"" && $2 == "=>" {print $3; found=1} END {if (!found) print "default"}'
}
case "$MODE" in --install|--apply|--check|--remove) ;; *) say 'invalid mode'; exit 2 ;; esac
if [ "$MODE" = --install ]; then
  [ "$(id -u)" = 0 ] || [ "${PIANO_POWERLOG_POLICY_NOCHOWN:-0}" = 1 ] || { say 'FAIL install requires root'; exit 1; }
  [ ! -L "$STATE" ] || { say 'FAIL state directory is a symlink'; exit 1; }
  mkdir -p "$STATE" && chmod 700 "$STATE" || exit 1
  exec "$0" --apply
fi
case "$MODE" in --apply|--check) ;; esac
if [ "$MODE" = --remove ] && [ ! -d "$STATE" ]; then
  say 'RESTORED nothing was recorded'; exit 0
fi
if [ "$MODE" = --remove ]; then
  prior=$(cat "$STATE/original" 2> /dev/null) || prior=default
  case "$prior" in true|false|default) ;; *) fail 'invalid original state; receipt kept' ;; esac
  if [ "$prior" = true ]; then
    $LAUNCHCTL disable "$TARGET" || fail 'restoring the original disable failed'
  else
    $LAUNCHCTL enable "$TARGET" || fail 'enable failed; receipt kept'
  fi
  current=$(override) || fail 'cannot read back the restored override'
  case "$prior:$current" in
    true:true|true:disabled) ;;
    *:false|*:enabled) [ "$prior" = true ] && fail 'the original disable was not restored' ;;
    default:default) ;;
    *) fail "restored override is $current, expected $prior" ;;
  esac
  loaded && $LAUNCHCTL kickstart -p "$TARGET" > /dev/null 2>&1
  status "RESTORED override=$prior policy=powerlog1"
  exit 0
fi
mkdir -p "$STATE" 2> /dev/null
i=0
until mkdir "$STATE/lock" 2> /dev/null; do
  i=$((i + 1)); [ "$i" -lt 25 ] || { say 'FAIL state is locked; retry without deleting its lock'; exit 1; }
  sleep 0.2
done
trap 'rmdir "$STATE/lock" 2> /dev/null || true' EXIT HUP INT TERM
current=$(override) || fail 'cannot read the disabled state'
if [ "$MODE" = --check ]; then
  [ "$current" = true ] || [ "$current" = disabled ] || { say 'FAIL the job is not disabled'; exit 1; }
  loaded && { say 'FAIL the job is still loaded'; exit 1; }
  say 'PASS disabled=true loaded=false policy=powerlog1'
  exit 0
fi
[ -f "$STATE/original" ] || { mkdir -p "$STATE" && printf '%s\n' "$current" > "$STATE/original"; }
[ "$current" = true ] || [ "$current" = disabled ] || {
  $LAUNCHCTL disable "$TARGET" || fail 'disable failed'
  current=$(override) || fail 'cannot read back the override'
  [ "$current" = true ] || [ "$current" = disabled ] || fail 'the override did not take'
}
if loaded; then
  $LAUNCHCTL bootout "$TARGET" > /dev/null 2>&1 || fail 'the job remains loaded'
fi
loaded && fail 'the job came back after bootout'
status "PASS disabled=true loaded=false policy=powerlog1"
exit 0