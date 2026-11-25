#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Log in to the debug account over PianoDwc3's simulated keyboard. The
# password goes only to piano-input.py on stdin.
#
#   desk-login.sh [--console FILE] [--timeout SECS] [--secret FILE]
#                 [--webcam-dir DIR] [--uid UID] [--now]
#
# Shift wakes the display. Each submission requires its own fresh, lit C920
# frame, a short settle, and a cleared password field. A new Finder process
# owned by the target UID and matching /dev/console ownership prove login.
# --now skips waiting for the SecurityAgent console line.
# DESK_LOGIN_INPUT and DESK_LOGIN_PROBE replace transports for offline tests.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd -P); cd "$D"
MAIN=$(git rev-parse --git-common-dir); MAIN=$(cd "$MAIN/.." && pwd -P)
CONSOLE= TIMEOUT=600 SECRET=$MAIN/private/login.secret NOW=0 LOGIN_UID=${DESK_LOGIN_UID:-501} WEBCAM_DIR=
while [ $# -gt 0 ]; do
  case $1 in
    --console) CONSOLE=$2; shift ;;
    --timeout) TIMEOUT=$2; shift ;;
    --secret) SECRET=$2; shift ;;
    --webcam-dir) WEBCAM_DIR=$2; shift ;;
    --uid) LOGIN_UID=$2; shift ;;
    --now) NOW=1 ;;
    *) echo 'desk-login: unknown argument' >&2; exit 2 ;;
  esac
  shift
done
case $LOGIN_UID in ''|*[!0-9]*|0*) echo 'desk-login: a positive target UID is required' >&2; exit 2;; esac
case $TIMEOUT in ''|*[!0-9]*|0*) echo 'desk-login: a positive timeout is required' >&2; exit 2;; esac
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

start=$(date +%s)
left() { echo $(( TIMEOUT - ($(date +%s) - start) )); }
since() { echo $(( $(date +%s) - start )); }

session_state() {
  local state
  if [ -n "${DESK_LOGIN_PROBE:-}" ]; then
    local probe
    read -r -a probe <<< "$DESK_LOGIN_PROBE"
    state=$("${probe[@]}" "$LOGIN_UID" </dev/null) || return 1
  else
    state=$(/bin/bash /tmp/tmo.sh 12 /bin/bash /tmp/tssh       "/usr/bin/stat -f %u /dev/console && { /usr/bin/pgrep -u $LOGIN_UID -x Finder || [ \$? -eq 1 ]; }"       </dev/null 2>/dev/null) || return 1
  fi
  state=${state//$'\n'/ }
  [[ $state =~ ^[0-9]+(\ [0-9]+)*$ ]] || return 1
  read -r console_uid finder_pids <<< "$state"
}

prove_desktop() {
  local pid
  [ "$console_uid" = "$LOGIN_UID" ] || return 1
  for pid in $finder_pids; do
    case " $baseline_finders " in *" $pid "*) continue ;; esac
    echo "desk-login: the desktop: the Finder opened (new PID and console user confirmed) $(( $(since) - typed ))s after password"
    return 0
  done
  return 1
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

if [ "$NOW" = 0 ]; then
  if [ -z "$CONSOLE" ]; then
    while [ "$(left)" -gt 0 ]; do
      CONSOLE=$(ls -t logs/usb-console/*-port0.txt 2>/dev/null | head -n 1 || true)
      [ -n "$CONSOLE" ] && [ $(( $(date +%s) - $(stat -f %m "$CONSOLE") )) -lt 120 ] && break
      CONSOLE=; sleep 3
    done
  fi
  [ -n "$CONSOLE" ] && [ -f "$CONSOLE" ] || { echo 'desk-login: no live console; nothing typed' >&2; exit 2; }
  echo "desk-login: watching $CONSOLE"
  until grep -aq 'PianoA830: SecurityAgent opens the accelerator' "$CONSOLE"; do
    [ "$(left)" -gt 0 ] || { echo 'desk-login: no SecurityAgent; nothing typed' >&2; exit 2; }
    sleep 3
  done
fi
until "${INPUT[@]}" status >/dev/null 2>&1 && session_state; do
  [ "$(left)" -gt 0 ] || { echo 'desk-login: input or session state unavailable; nothing typed' >&2; exit 2; }
  sleep 2
done
if [ "$console_uid" = "$LOGIN_UID" ] && [ -n "$finder_pids" ]; then
  echo 'desk-login: Finder runs already and console user confirmed'
  exit 0
fi
baseline_finders=$finder_pids

for attempt in 1 2; do
  [ "$(left)" -gt 0 ] || break
  if [ "$attempt" = 2 ]; then
    # A slow authentication or a lost SSH response must not cause input on a desktop.
    session_state || { echo 'desk-login: session state unavailable; retry skipped' >&2; exit 1; }
    if prove_desktop; then exit 0; fi
  fi
  [ "$console_uid" = 0 ] || { echo 'desk-login: console is no longer at the login window' >&2; exit 1; }
  "${INPUT[@]}" key shift >/dev/null
  # 11-19: the daily v12 profile has auto-brightness with no RAM floor; at 0 lux the login window sits at 6 % (code 128), too
  # dark for the C920 lit check. Brightness-up keys are a normal user action and only raise the panel (DESK_LOGIN_BRIGHTEN=0 off).
  # 11-21 slider3: not "seq 1 N": macOS's BSD seq counts down, so "seq 1 0" is "1 0" and DESK_LOGIN_BRIGHTEN=0 sent two keys.
  brighten=${DESK_LOGIN_BRIGHTEN:-6}; case $brighten in ''|*[!0-9]*) brighten=6 ;; esac
  sent=0
  while [ "$sent" -lt "$brighten" ]; do "${INPUT[@]}" key brightnessup >/dev/null 2>&1 || true; sleep 0.3; sent=$((sent + 1)); done
  echo "desk-login: $sent brightness-up key(s) sent (DESK_LOGIN_BRIGHTEN=${DESK_LOGIN_BRIGHTEN:-default 6})"
  touch "$scratch/wake"
  echo "desk-login: non-text wake sent for attempt $attempt"
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
    if session_state && prove_desktop; then exit 0; fi
    sleep 2
  done
done
echo 'desk-login: no new Finder plus console-user proof within the login window' >&2
exit 1
