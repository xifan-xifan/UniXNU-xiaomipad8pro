#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Log in to the debug account over PianoDwc3's simulated keyboard, or unlock its locked screen. The
# password goes only to piano-input.py on stdin.
#
#   desk-login.sh [--console FILE] [--timeout SECS] [--secret FILE]
#                 [--webcam-dir DIR] [--uid UID] [--now] [--state]
#
# One ssh read gives the session state: the console user (stat /dev/console), the lock flag of the console
# session (ioreg IOConsoleUsers: "CGSSessionScreenIsLocked"=Yes, confirmed on the tablet 11-24 gpu-hz120-y12c2),
# the Finder and SecurityAgent processes. Three states are acted on:
#   loginwindow  console root: shift wakes, then the password; a NEW Finder of the target UID with the console
#                user proves the login.
#   locked       console user = the target UID with the lock flag set (a session after sleep or a display
#                lock): shift wakes, then the password; the lock flag cleared with the same console user and a
#                Finder proves the unlock (no new Finder: the session was already there).
#   desktop      console user = the target UID, lock flag clear, Finder running: nothing typed.
# Each submission requires its own fresh, lit C920 frame, a short settle, and a cleared password field.
# Readiness before typing: the console line "PianoA830: SecurityAgent opens the accelerator" (RAM boots) OR a
# state read showing SecurityAgent at the login window, a locked session or the desktop (installed boots, where
# the capture can start after the line or the KC no longer prints it). --now skips the readiness wait.
# --state prints the state line and exits (0 desktop, 3 locked, 4 loginwindow, 1 other/unknown); nothing typed.
# DESK_LOGIN_INPUT and DESK_LOGIN_PROBE replace transports for offline tests (the probe prints the state line
# "C=<uid> U=<0|1> L=<0|1> S=<n> F=<pid,...>").
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd -P); cd "$D"
MAIN=$(git rev-parse --git-common-dir); MAIN=$(cd "$MAIN/.." && pwd -P)
CONSOLE= TIMEOUT=600 SECRET=$MAIN/private/login.secret NOW=0 STATE_ONLY=0 LOGIN_UID=${DESK_LOGIN_UID:-501} WEBCAM_DIR=
while [ $# -gt 0 ]; do
  case $1 in
    --console) CONSOLE=$2; shift ;;
    --timeout) TIMEOUT=$2; shift ;;
    --secret) SECRET=$2; shift ;;
    --webcam-dir) WEBCAM_DIR=$2; shift ;;
    --uid) LOGIN_UID=$2; shift ;;
    --now) NOW=1 ;;
    --state) STATE_ONLY=1 ;;
    *) echo 'desk-login: unknown argument' >&2; exit 2 ;;
  esac
  shift
done
case $LOGIN_UID in ''|*[!0-9]*|0*) echo 'desk-login: a positive target UID is required' >&2; exit 2;; esac
case $TIMEOUT in ''|*[!0-9]*|0*) echo 'desk-login: a positive timeout is required' >&2; exit 2;; esac

start=$(date +%s)
left() { echo $(( TIMEOUT - ($(date +%s) - start) )); }
since() { echo $(( $(date +%s) - start )); }

# session_state: sets console_uid users locked agents finder_pids and state (desktop|locked|loginwindow|starting|other)
session_state() {
  local raw k v
  if [ -n "${DESK_LOGIN_PROBE:-}" ]; then
    local probe
    read -r -a probe <<< "$DESK_LOGIN_PROBE"
    raw=$("${probe[@]}" "$LOGIN_UID" </dev/null) || return 1
  else
    raw=$(/bin/bash /tmp/tmo.sh 12 /bin/bash /tmp/tssh "printf 'C=%s ' \"\$(/usr/bin/stat -f %u /dev/console)\"; r=\$(/usr/sbin/ioreg -n Root -d1 -w0); case \"\$r\" in *IOConsoleUsers*) printf 'U=1 ';; *) printf 'U=0 ';; esac; case \"\$r\" in *'\"CGSSessionScreenIsLocked\"=Yes'*) printf 'L=1 ';; *) printf 'L=0 ';; esac; printf 'S=%s ' \"\$(/usr/bin/pgrep -x SecurityAgent | /usr/bin/wc -l | /usr/bin/tr -d ' ')\"; printf 'F='; /usr/bin/pgrep -u $LOGIN_UID -x Finder | /usr/bin/tr '\n' ','; echo" </dev/null 2>/dev/null) || return 1
  fi
  raw=${raw//$'\n'/ }
  local format='^C=[0-9]+ U=[01] L=[01] S=[0-9]+ F=([1-9][0-9]*(,[1-9][0-9]*)*,?)? *$'
  [[ $raw =~ $format ]] || return 1
  console_uid= users= locked= agents= finder_pids=
  for kv in $raw; do
    k=${kv%%=*} v=${kv#*=}
    case $k in C) console_uid=$v ;; U) users=$v ;; L) locked=$v ;; S) agents=$v ;; F) finder_pids=${v//,/ } ;; esac
  done
  finder_pids=${finder_pids% }
  if [ "$console_uid" = 0 ]; then state=loginwindow
  elif [ "$console_uid" != "$LOGIN_UID" ]; then state=other
  elif [ "$users" != 1 ]; then return 1          # the lock flag lives in IOConsoleUsers: without it the read is not a read
  elif [ "$locked" = 1 ]; then state=locked
  elif [ -n "$finder_pids" ]; then state=desktop
  else state=starting
  fi
}
state_line() { echo "desk-login: state $state (console uid $console_uid, lock flag $locked, SecurityAgent $agents, Finder ${finder_pids:-none})"; }

if [ "$STATE_ONLY" = 1 ]; then
  session_state || { echo 'desk-login: state unknown (no answer or no IOConsoleUsers)'; exit 1; }
  state_line
  case $state in desktop) exit 0 ;; locked) exit 3 ;; loginwindow) exit 4 ;; *) exit 1 ;; esac
fi

[ -s "$SECRET" ] || { echo 'desk-login: secret is missing or empty' >&2; exit 2; }
secret_dir=$(cd "$(dirname "$SECRET")" && pwd -P)
case $secret_dir/ in "$D/private/"*|"$D/out/"*|"$MAIN/private/"*) ;; *)
  echo 'desk-login: secret is not under this repository’s private/ or out/' >&2; exit 2 ;; esac
SECRET=$secret_dir/${SECRET##*/}
read -r -a INPUT <<< "${DESK_LOGIN_INPUT:-python3 tooling/native/piano-input.py}"
for tool in imagesnap ffmpeg; do
  command -v "$tool" >/dev/null || { echo "desk-login: $tool is required" >&2; exit 2; }
done
umask 077
scratch=$(mktemp -d "${TMPDIR:-/tmp}/desk-login.XXXXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
WEBCAM_DIR=${WEBCAM_DIR:-$scratch}
mkdir -p -- "$WEBCAM_DIR"

prove_login() {
  local pid
  [ "$state" = desktop ] || return 1
  for pid in $finder_pids; do
    case " $baseline_finders " in *" $pid "*) continue ;; esac
    echo "desk-login: the desktop: the Finder opened (new PID and console user confirmed) $(( $(since) - typed ))s after password"
    return 0
  done
  return 1
}
prove_unlock() {
  [ "$state" = desktop ] || return 1
  # The wording keeps the callers' accepted line ("Finder runs already and console user confirmed").
  echo "desk-login: Finder runs already and console user confirmed; the locked screen unlocked (lock flag cleared) $(( $(since) - typed ))s after password"
}

fresh_panel() {
  local photo=$1 proof age
  rm -f -- "$photo" || return 1
  if [ -n "${DESK_LOGIN_RECORDING_FRAME:-}" ]; then
    # A recorder owns the C920 (a second camera client reconfigures it and corrupts the recording): take a frame the recorder
    # wrote after this request, complete (JPEG SOI..EOI), copied atomically.
    local req n=0 tmp="$photo.part"
    req=$(date +%s)
    while [ "$n" -lt 30 ]; do
      if [ -s "$DESK_LOGIN_RECORDING_FRAME" ] && [ "$(stat -f %m "$DESK_LOGIN_RECORDING_FRAME")" -gt "$req" ] &&
         cp "$DESK_LOGIN_RECORDING_FRAME" "$tmp" 2>/dev/null &&
         [ "$(head -c 2 "$tmp" | xxd -p)" = ffd8 ] && [ "$(tail -c 2 "$tmp" | xxd -p)" = ffd9 ]; then
        mv -f "$tmp" "$photo"; break
      fi
      n=$((n + 1)); sleep 0.1
    done
    rm -f -- "$tmp"; [ -s "$photo" ] || return 1
  else
    /bin/bash /tmp/tmo.sh 8 imagesnap -q -w 1.5 -d 'HD Pro Webcam C920' "$photo" >/dev/null 2>&1 || return 1
  fi
  [ -s "$photo" ] && [ "$photo" -nt "$scratch/wake" ] || return 1
  proof=$(/bin/bash tooling/native/panel-lit.sh --grade "$photo") || return 1
  age=$(( $(date +%s) - $(stat -f %m "$photo") ))
  [ "$age" -ge 0 ] && [ "$age" -lt 4 ] || return 1
  printf '%s\n' "$proof"
}

# Readiness: the console line when a live capture has it, else the state read (SecurityAgent up at the login window, or the target
# user's session already on the console: locked or desktop). Neither: wait. The console is optional now (installed boots).
ready_by_console() {
  [ -n "$CONSOLE" ] && [ -f "$CONSOLE" ] && grep -aq 'PianoA830: SecurityAgent opens the accelerator' "$CONSOLE"
}
if [ "$NOW" = 0 ]; then
  if [ -z "$CONSOLE" ]; then
    CONSOLE=$(ls -t logs/usb-console/*-port0.txt 2>/dev/null | head -n 1 || true)
    if [ -n "$CONSOLE" ] && [ $(( $(date +%s) - $(stat -f %m "$CONSOLE") )) -ge 120 ]; then CONSOLE=; fi
  fi
  if [ -n "$CONSOLE" ]; then echo "desk-login: watching $CONSOLE (and the session state)"; else echo 'desk-login: no live console; readiness from the session state'; fi
  while :; do
    if ready_by_console; then echo 'desk-login: ready: the SecurityAgent console line'; break; fi
    if "${INPUT[@]}" status >/dev/null 2>&1 && session_state; then
      case $state in
        loginwindow) if [ "${agents:-0}" -gt 0 ]; then echo 'desk-login: ready: SecurityAgent runs at the login window'; break; fi ;;
        locked|desktop|starting|other) echo "desk-login: ready: the console belongs to uid $console_uid ($state)"; break ;;
      esac
    fi
    [ "$(left)" -gt 0 ] || { echo 'desk-login: no SecurityAgent line or login-window state; nothing typed' >&2; exit 2; }
    sleep 3
  done
fi
until "${INPUT[@]}" status >/dev/null 2>&1 && session_state; do
  [ "$(left)" -gt 0 ] || { echo 'desk-login: input or session state unavailable; nothing typed' >&2; exit 2; }
  sleep 2
done
state_line
while [ "$state" = starting ]; do   # the target user's session without a Finder yet: wait for it (no input)
  [ "$(left)" -gt 0 ] || { echo 'desk-login: the session never showed a Finder; nothing typed' >&2; exit 1; }
  sleep 2; session_state || true
done
case $state in
  desktop) echo 'desk-login: Finder runs already and console user confirmed (screen not locked)'; exit 0 ;;
  other) echo "desk-login: the console belongs to uid $console_uid, not $LOGIN_UID; nothing typed" >&2; exit 1 ;;
esac
mode=$state      # loginwindow or locked
baseline_finders=$finder_pids

for attempt in 1 2; do
  [ "$(left)" -gt 0 ] || break
  if [ "$attempt" = 2 ]; then
    # A slow authentication or a lost SSH response must not cause input on a desktop.
    session_state || { echo 'desk-login: session state unavailable; retry skipped' >&2; exit 1; }
    if [ "$mode" = loginwindow ] && prove_login; then exit 0; fi
    if [ "$mode" = locked ] && prove_unlock; then exit 0; fi
  fi
  if [ "$mode" = loginwindow ]; then
    [ "$state" = loginwindow ] || { echo "desk-login: console is no longer at the login window ($state)" >&2; exit 1; }
  else
    [ "$state" = locked ] || { echo "desk-login: the session is no longer locked ($state)" >&2; exit 1; }
  fi
  "${INPUT[@]}" key shift >/dev/null
  # 11-19: the daily v12 profile has auto-brightness with no RAM floor; at 0 lux the login window sits at 6 % (code 128), too
  # dark for the C920 lit check. Brightness-up keys are a normal user action and only raise the panel (DESK_LOGIN_BRIGHTEN=0 off).
  # 11-21 slider3: not "seq 1 N": macOS's BSD seq counts down, so "seq 1 0" is "1 0" and DESK_LOGIN_BRIGHTEN=0 sent two keys.
  brighten=${DESK_LOGIN_BRIGHTEN:-6}; case $brighten in ''|*[!0-9]*) brighten=6 ;; esac
  sent=0
  while [ "$sent" -lt "$brighten" ]; do "${INPUT[@]}" key brightnessup >/dev/null 2>&1 || true; sleep 0.3; sent=$((sent + 1)); done
  echo "desk-login: $sent brightness-up key(s) sent (DESK_LOGIN_BRIGHTEN=${DESK_LOGIN_BRIGHTEN:-default 6})"
  touch "$scratch/wake"
  echo "desk-login: non-text wake sent for attempt $attempt ($mode)"
  frame_deadline=$(( $(date +%s) + 20 ))
  until fresh_panel "$WEBCAM_DIR/after-wake-$attempt.jpg"; do
    if [ "$(left)" -le 0 ] || [ "$(date +%s)" -ge "$frame_deadline" ]; then
      echo "desk-login: no fresh lit C920 frame for attempt $attempt; input skipped" >&2
      exit 2
    fi
    sleep 1
  done
  sleep 2
  [ "$(left)" -gt 0 ] || break
  "${INPUT[@]}" key cmd+a >/dev/null
  "${INPUT[@]}" key backspace >/dev/null
  echo "desk-login: fresh lit frame and HID field clear complete for attempt $attempt"
  typed=$(since)
  if "${INPUT[@]}" type --stdin --enter < "$SECRET" >/dev/null; then
    echo "desk-login: password and Return sent for attempt $attempt"
  else
    echo "desk-login: HID submission unconfirmed for attempt $attempt; checking desktop" >&2
  fi

  proof_deadline=$(( $(date +%s) + 90 ))
  while [ "$(left)" -gt 0 ] && [ "$(date +%s)" -lt "$proof_deadline" ]; do
    if session_state; then
      if [ "$mode" = loginwindow ] && prove_login; then exit 0; fi
      if [ "$mode" = locked ] && prove_unlock; then exit 0; fi
    fi
    sleep 2
  done
done
if [ "$mode" = locked ]; then echo 'desk-login: the lock flag did not clear within the unlock window' >&2
else echo 'desk-login: no new Finder plus console-user proof within the login window' >&2; fi
exit 1
