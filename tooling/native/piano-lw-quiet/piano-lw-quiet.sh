#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-lw-quiet (LT-D10, owner 2026-11-25 09:28: keep the password login, fix the login-window CPU loop).
# At the login window (nobody owns /dev/console) Apple's audiomxd spins in a BTAudioRoutingRequest retry loop (UpdateAudioState ->
# "No user logged in" -> "audioaccessoryd died" -> again), ~1.3 cores on piano with configd answering its console-user queries; the
# same loop is seen on a Mac mini's login window (its audiomxd log quarantined for volume). This daemon keeps audiomxd stopped
# (SIGSTOP) while no user is logged in and continues it (SIGCONT) the moment one is; a launchd restart of audiomxd (new pid) is
# handled the same way. Tablet A/B 2026-11-25 09:5x: audiomxd 73 % + configd 45 % -> 0; the login completes, audiomxd resumes,
# sound plays. Off with boot-arg piano-lw-quiet=0. Root, no python; a 2 s poll (stat + pgrep).
args=$(/usr/sbin/sysctl -n kern.piano_bootargs 2>/dev/null || /usr/sbin/sysctl -n kern.bootargs 2>/dev/null)
case " $args " in *" piano-lw-quiet=0 "*) exit 0;; esac
stopped=""
resume() { [ -n "$stopped" ] && /bin/kill -CONT "$stopped" 2>/dev/null; stopped=""; }
trap 'resume; exit 0' TERM INT HUP EXIT
while :; do
  u=$(/usr/bin/stat -f %Su /dev/console 2>/dev/null)
  p=$(/usr/bin/pgrep -x audiomxd | /usr/bin/head -1)
  if [ "$u" = root ] || [ -z "$u" ]; then
    if [ -n "$p" ] && [ "$p" != "$stopped" ]; then
      resume
      /bin/kill -STOP "$p" 2>/dev/null && stopped=$p && /usr/bin/logger -t piano-lw-quiet "audiomxd $p stopped (no console user)"
    fi
  elif [ -n "$stopped" ]; then
    /usr/bin/logger -t piano-lw-quiet "audiomxd $stopped continued (console user $u)"
    resume
  fi
  sleep 2
done
