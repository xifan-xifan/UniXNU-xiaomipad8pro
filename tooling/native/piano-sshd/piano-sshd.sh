#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-sshd (configs/userland.toml): sshd on 2222 started by launchd at boot, so a root shell no longer depends on the COM6
# user pipe coming up. Why this shape and not Remote Login: docs/results-2026-11-28-usb-net-ssh-and-the-arg-budget.md - Apple's
# /System/Library/LaunchDaemons/ssh.plist cannot be bootstrapped here (Remote Login is switched off, "Bootstrap failed: 5"),
# and on port 22 the key is accepted and the session then dies in pam_restriction, so -o UsePAM=no is load-bearing.
# Gate: it serves only when /var/root/.ssh/authorized_keys is non-empty, so an image without a key is not a listening daemon.
# The wait matters: root's home is on Data, and after an Android stint the mount came later than this job's start (the boot of
# 2026-11-28 12:20), so exiting at once would have meant no listener for the rest of the boot. It waits (PIANO_SSHD_WAIT,
# default 240 s) and says so on the console every 30 s rather than giving up quietly.
# Lines "piano-sshd: ..." go to the console. A host test runs it with PIANO_SSHD_PATH (stubs first), PIANO_SSHD_OUT,
# PIANO_SSHD_KEYS, PIANO_SSHD_PORT, PIANO_SSHD_WAIT and PIANO_SSHD_NOW=1 (check and exit instead of exec'ing sshd).
set -u
PATH=${PIANO_SSHD_PATH:-/usr/sbin:/usr/bin:/sbin:/bin}
out=${PIANO_SSHD_OUT:-/dev/console}
keys=${PIANO_SSHD_KEYS:-/var/root/.ssh/authorized_keys}
port=${PIANO_SSHD_PORT:-2222}
wait_s=${PIANO_SSHD_WAIT:-240}
say() { printf 'piano-sshd: %.230s\n' "$1" >> "$out" 2> /dev/null; }

w=0
while [ ! -s "$keys" ]; do
  [ "$w" -lt "$wait_s" ] || { say "no key in $keys after ${w} s: not starting"; exit 0; }
  if [ $((w % 30)) = 0 ]; then say "$keys is not there yet (after ${w} s): waiting up to ${wait_s} s"; fi
  sleep 5
  w=$((w + 5))
done
[ "$w" -gt 0 ] && say "$keys appeared after ${w} s"
[ -x /usr/sbin/sshd ] || { say "/usr/sbin/sshd is missing: not starting"; exit 0; }

missing=
for k in /etc/ssh/ssh_host_rsa_key /etc/ssh/ssh_host_ecdsa_key /etc/ssh/ssh_host_ed25519_key; do
  [ -s "$k" ] || missing="$missing $(basename "$k")"
done
if [ -n "$missing" ]; then
  t0=$(date +%s)
  /usr/sbin/ssh-keygen -A > /dev/null 2>&1
  say "host keys for$missing generated (rc $?) after $(( $(date +%s) - t0 )) s"
fi

[ "${PIANO_SSHD_NOW:-0}" = 1 ] && { say "check only (PIANO_SSHD_NOW=1): key present, sshd present, host keys present"; exit 0; }
say "exec /usr/sbin/sshd -D -p $port -o UsePAM=no -o PermitRootLogin=yes"
exec /usr/sbin/sshd -D -p "$port" -o UsePAM=no -o PermitRootLogin=yes -o PidFile=/var/run/piano-sshd.pid