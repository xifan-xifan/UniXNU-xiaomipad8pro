#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026, Leo Zhang. All rights reserved.
# watchdogd cannot run here. It looks the watchdog kext up with IOServiceMatching("IOWatchdog") (watchdogd+0x2af8, IOServiceOpen
# at +0x2b2c); piano publishes no such service, so it takes the crash branch: __os_crash("connect_to_kext() unexpectedly failed -
# unable to lookup watchdog KEXT service") and brk #1, which is the EXC_BREAKPOINT/SIGTRAP at watchdogd+0xc948 in
# /Library/Logs/DiagnosticReports/watchdogd-2026-11-24-220316.ips (slice_uuid 3258367c-791c-38d8-89c8-b5de978b556e, the same
# bytes as the installed /usr/libexec/watchdogd sha256 78bdcbdf831850a49ff9d50859b1f608482f6ad8c57a4c078fad134a99f52565).
# The only quiet exit it has is for a guest: the branch that logs "detected virtual machine environment and no watchdog KEXT
# found, exiting..." is gated on kern.hv_vmm_present, which a native machine does not set. So on a native boot the daemon
# crashes before it can check in, launchd respawns it, and ReportCrash/coresymbolicationd chew through the boot window.
# Nothing is lost by taking it out: a daemon that dies at connect_to_kext never arms anything. The boot guard stays where it
# always was (piano-boot-guard and the Gunyah watchdog PianoPower arms, docs/power.md), and the real fix is a native
# IOWatchdog service that answers the scalar methods watchdogd calls, which is the ticket this file points at.
# Nothing of Apple's is edited: the job is disabled through launchctl's own override and booted out, the override that was in
# place before us is recorded, and --remove puts it back.
# Modes: --apply (idempotent), --check, --remove. Host tests set PIANO_LAUNCHCTL to a stub and PIANO_USERLAND_ROOT for the state
# directory, and PIANO_WATCHDOG_POLICY_NOCHOWN=1 to skip the root-only steps. Lines "piano-watchdog-policy: ..." on stdout.
set -u
umask 077
R=${PIANO_USERLAND_ROOT:-}
LAUNCHCTL=${PIANO_LAUNCHCTL:-launchctl}
STATE=$R/private/var/db/piano-userland/watchdog-policy
TARGET=system/com.apple.watchdogd
MODE=${1:---apply}
say() { echo "piano-watchdog-policy: $*"; logger -t piano-watchdog-policy "$*" 2>/dev/null || true; }
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
  printf '%s\n' "$values" | awk '$1 == "\"com.apple.watchdogd\"" && $2 == "=>" {print $3; found=1} END {if (!found) print "default"}'
}
case "$MODE" in --install|--apply|--check|--remove) ;; *) say 'invalid mode'; exit 2 ;; esac
if [ "$MODE" = --install ]; then
  [ "$(id -u)" = 0 ] || [ "${PIANO_WATCHDOG_POLICY_NOCHOWN:-0}" = 1 ] || { say 'FAIL install requires root'; exit 1; }
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
  status "RESTORED override=$prior policy=watchdog1"
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
  say 'PASS disabled=true loaded=false policy=watchdog1'
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
status "PASS disabled=true loaded=false policy=watchdog1"
exit 0