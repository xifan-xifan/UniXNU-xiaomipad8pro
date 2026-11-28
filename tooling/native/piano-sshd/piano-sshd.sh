#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-sshd (configs/userland.toml): sshd on 2222 started by launchd at boot, so a root shell no longer depends on the COM6
# user pipe coming up. Why this shape and not Remote Login: docs/results-2026-11-28-usb-net-ssh-and-the-arg-budget.md - Apple's
# /System/Library/LaunchDaemons/ssh.plist cannot be bootstrapped here (Remote Login is switched off, "Bootstrap failed: 5"),
# and on port 22 the key is accepted and the session then dies in pam_restriction, so -o UsePAM=no is load-bearing.
# Two things this boot learned the hard way (docs/results-2026-11-28-ul11-roundtrip-1-macos-android-macos.md):
#   - root's home is on Data, and after an Android stint the mount came later than the job's start, so it waits (PIANO_SSHD_WAIT,
#     default 240 s) instead of exiting and leaving the boot without a listener;
#   - sshd with no host key exits, and launchd then restarts it every 10 s (the boot of 12:47 shows eleven sshd starts and
#     Apple's sshd-keygen-wrap with it). So the fast ed25519 host key is made here and named with -h, and the slow full set
#     (ssh-keygen -A, an RSA-3072 draw from a thin RNG on this platform) goes to the background instead of gating the listener.
# Lines "piano-sshd: ..." go to the console and to launchd's StandardErrorPath. A host test runs it with PIANO_SSHD_PATH
# (stubs first), PIANO_SSHD_OUT, PIANO_SSHD_KEYS, PIANO_SSHD_PORT, PIANO_SSHD_WAIT, PIANO_SSHD_HOSTKEY and PIANO_SSHD_NOW=1.
set -u
PATH=${PIANO_SSHD_PATH:-/usr/sbin:/usr/bin:/sbin:/bin}
out=${PIANO_SSHD_OUT:-/dev/console}
keys=${PIANO_SSHD_KEYS:-/var/root/.ssh/authorized_keys}
port=${PIANO_SSHD_PORT:-2222}
wait_s=${PIANO_SSHD_WAIT:-240}
hostkey=${PIANO_SSHD_HOSTKEY:-/etc/ssh/ssh_host_ed25519_key}
# Both ends, deliberately: /dev/console lines are unreliable this early (0-byte logs seen twice), and launchd's StandardErrorPath
# only sees the job's stderr, so the second printf is redirected to fd 2 rather than left on stdout (where it went to /dev/null).
say() { printf 'piano-sshd: %.230s\n' "$1" >> "$out" 2> /dev/null; printf 'piano-sshd: %.230s\n' "$1" >&2; }

w=0
while [ ! -s "$keys" ]; do
  [ "$w" -lt "$wait_s" ] || { say "no key in $keys after ${w} s: not starting"; exit 0; }
  [ $((w % 30)) = 0 ] && say "$keys is not there yet (after ${w} s): waiting up to ${wait_s} s"
  sleep 5
  w=$((w + 5))
done
[ "$w" -gt 0 ] && say "$keys appeared after ${w} s"
[ -x /usr/sbin/sshd ] || { say "/usr/sbin/sshd is missing: not starting"; exit 0; }

if [ ! -s "$hostkey" ]; then
  t0=$(date +%s)
  rm -f "$hostkey" "$hostkey.pub" 2> /dev/null
  /usr/bin/ssh-keygen -q -t ed25519 -f "$hostkey" -N '' -C "piano-sshd" > /dev/null 2>&1
  say "ed25519 host key made (rc $?) after $(( $(date +%s) - t0 )) s"
fi
[ -s "$hostkey" ] || { say "no host key in $hostkey: not starting"; exit 0; }
[ -s /etc/ssh/ssh_host_rsa_key ] || ( /usr/sbin/ssh-keygen -A > /dev/null 2>&1 ) &

[ "${PIANO_SSHD_NOW:-0}" = 1 ] && { say "check only (PIANO_SSHD_NOW=1): key present, sshd present, host key $hostkey present"; exit 0; }
say "exec /usr/sbin/sshd -D -p $port -h $hostkey -o UsePAM=no -o PermitRootLogin=yes"
exec /usr/sbin/sshd -D -p "$port" -h "$hostkey" -o UsePAM=no -o PermitRootLogin=yes -o PidFile=/var/run/piano-sshd.pid