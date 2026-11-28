#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-sshd (configs/userland.toml): sshd on 2222 started by launchd at boot, so a root shell no longer depends on the COM6
# user pipe coming up. Why this shape and not Remote Login: docs/results-2026-11-28-usb-net-ssh-and-the-arg-budget.md - Apple's
# /System/Library/LaunchDaemons/ssh.plist cannot be bootstrapped here (Remote Login is switched off, "Bootstrap failed: 5"),
# and on port 22 the key is accepted and the session then dies in pam_restriction, so -o UsePAM=no is load-bearing.
# Gate: it serves only when /var/root/.ssh/authorized_keys is non-empty, so an image without a key is not a listening daemon.
# Lines "piano-sshd: ..." go to the console. A host test runs it with PIANO_SSHD_PATH (stubs first), PIANO_SSHD_OUT,
# PIANO_SSHD_KEYS, PIANO_SSHD_PORT and PIANO_SSHD_NOW=1 (check and exit instead of exec'ing sshd).
set -u
PATH=${PIANO_SSHD_PATH:-/usr/sbin:/usr/bin:/sbin:/bin}
out=${PIANO_SSHD_OUT:-/dev/console}
keys=${PIANO_SSHD_KEYS:-/var/root/.ssh/authorized_keys}
port=${PIANO_SSHD_PORT:-2222}
say() { printf 'piano-sshd: %.230s\n' "$1" >> "$out" 2> /dev/null; }

[ -s "$keys" ] || { say "no key in $keys: not starting"; exit 0; }
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