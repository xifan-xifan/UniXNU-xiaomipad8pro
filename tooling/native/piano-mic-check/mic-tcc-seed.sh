#!/bin/bash
# SPDX-License-Identifier: MIT
# Source for the boot-arg test, or run a hash-verified copy as root on the tablet.

mic_check_bootargs() {
  local args quote
  if args=$(LC_ALL=C /usr/sbin/sysctl -n kern.piano_bootargs 2>&1); then
    printf "%s\n" "$args"
    return 0
  fi
  # This function is also sent through the text-mode SSH wrapper.
  quote=$(printf "\\047")
  if [ "$args" = "sysctl: unknown oid ${quote}kern.piano_bootargs${quote}" ]; then
    /usr/sbin/sysctl -n kern.bootargs 2>/dev/null
  else
    return 1
  fi
}

mic_check_boot_enabled() {
  /usr/bin/awk '
    {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^piano-mic-unattended(=|$)/) count++
        if ($i == "piano-mic-unattended=1") enabled++
      }
    }
    END { exit !(count == 1 && enabled == 1) }
  ' <<< "$1"
}

mic_tcc_seed_error() {
  printf 'TCC_SEED_ERROR %s\n' "$*" >&2
  return 1
}

mic_tcc_seed_apply() (
  local boot=$1 stage=$2 cdhash=$3 args uid db user_db='' tccd now receipt
  local bundle=com.unixnu.piano-mic-check requirement scratch hex
  [[ "$boot" =~ ^[0-9A-Fa-f-]{36}$ &&
     "$stage" =~ ^/private/var/tmp/piano-mic-check\.[A-Za-z0-9]{8}$ &&
     "$cdhash" =~ ^[0-9a-f]{40}$ ]] || { mic_tcc_seed_error 'invalid boot, stage or code hash'; return 1; }
  [ "$EUID" -eq 0 ] || { mic_tcc_seed_error 'root required'; return 1; }
  args=$(mic_check_bootargs) || return 1
  mic_check_boot_enabled "$args" || { mic_tcc_seed_error 'piano-mic-unattended=1 required'; return 1; }
  [ "$(/usr/sbin/sysctl -n kern.bootsessionuuid)" = "$boot" ] ||
    { mic_tcc_seed_error 'boot identity changed'; return 1; }

  uid=$(/usr/bin/stat -f %u /dev/console) || return 1
  if ! { [[ "$uid" =~ ^[0-9]+$ ]] && [ "$uid" -ge 501 ] &&
    [ "$uid" = "$(/usr/bin/stat -f %u "$stage")" ] &&
    /usr/bin/pgrep -u "$uid" -x Finder >/dev/null &&
    /bin/launchctl print "gui/$uid" >/dev/null; }; then
    mic_tcc_seed_error 'staging user must own a live desktop'
    return 1
  fi
  tccd=$(/usr/bin/pgrep -u "$uid" -x tccd) || return 1
  [[ "$tccd" =~ ^[0-9]+$ ]] || { mic_tcc_seed_error 'expected one console user tccd'; return 1; }
  for db in /private/var/containers/Data/ProtectedSystem/*/Data/Library/Application\ Support/com.apple.TCC/TCC.db; do
    [ -f "$db" ] && [ ! -L "$db" ] && [ "$(/usr/bin/stat -f %u "$db")" = "$uid" ] || continue
    [ -z "$user_db" ] || { mic_tcc_seed_error 'multiple console user TCC databases'; return 1; }
    user_db=$db
  done
  [ -n "$user_db" ] || { mic_tcc_seed_error 'console user ProtectedSystem TCC database missing'; return 1; }

  # The LaunchServices probe is responsible for its own TCC request. Bind the
  # row to the exact ad-hoc signature staged by this spec.
  requirement="identifier \"$bundle\" and cdhash H\"$cdhash\""
  /usr/bin/codesign --verify --strict -R="$requirement" "$stage/PianoMicCheck.app" ||
    { mic_tcc_seed_error 'app does not match the staged signing requirement'; return 1; }
  scratch=$(/usr/bin/mktemp -d /private/var/run/piano-mic-csreq.XXXXXXXX) || return 1
  trap '/bin/rm -f -- "$scratch/app.csreq"; /bin/rmdir "$scratch"' EXIT
  printf '%s\n' "$requirement" | /usr/bin/csreq -r- -b "$scratch/app.csreq" || return 1
  hex=$(/usr/bin/xxd -p "$scratch/app.csreq" | /usr/bin/tr -d '\n') || return 1
  [[ "$hex" =~ ^([0-9a-f]{2})+$ ]] || { mic_tcc_seed_error 'invalid requirement blob'; return 1; }

  now=$(/bin/date +%s) || return 1
  receipt=$(/usr/bin/sqlite3 -batch -bail -cmd '.timeout 5000' "$user_db" "
    BEGIN IMMEDIATE;
    INSERT OR REPLACE INTO access
      (service, client, client_type, auth_value, auth_reason, auth_version, csreq, flags, last_modified)
      VALUES ('kTCCServiceMicrophone', '$bundle', 0, 2, 4, 1, X'$hex', 0, $now);
    COMMIT;
    SELECT 'TCC_MIC_APP_OK' FROM access
      WHERE service='kTCCServiceMicrophone' AND client='$bundle' AND client_type=0
        AND auth_value=2 AND auth_reason=4 AND csreq=X'$hex';
  ") || { mic_tcc_seed_error 'could not write or read back the app grant'; return 1; }
  [ "$receipt" = TCC_MIC_APP_OK ] || { mic_tcc_seed_error 'app grant readback mismatch'; return 1; }
  printf '%s\n' "$receipt"
  [ "$(/usr/bin/stat -f %u /dev/console)" = "$uid" ] &&
    [ "$(/usr/bin/pgrep -u "$uid" -x tccd)" = "$tccd" ] ||
    { mic_tcc_seed_error 'console user or tccd changed during the write'; return 1; }
  /bin/kill -KILL "$tccd" || return 1
  printf 'TCC_USER_TCCD_SIGNALLED=%s\nTCC_MIC_SEED_COMPLETE\n' "$tccd"
)

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  [ "$#" -eq 4 ] && [ "$1" = --apply ] || exit 2
  umask 077
  mic_tcc_seed_apply "$2" "$3" "$4"
fi
