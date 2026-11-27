# Source from the installed-system runner, then call metal_sustain_installed after the desktop gate.
# Uses tabletrun.sh helpers and its live R, C and MARK variables. Does not boot or change policy.

metal_sustain_snapshot() {
  rsudo 'printf "BOOT="; /usr/sbin/sysctl -n kern.bootsessionuuid; /usr/sbin/sysctl -n machdep.piano_gpu_state; /usr/sbin/sysctl -n machdep.piano_gpu_recovery'
}

# Run under the proven console login. rssh/rsudo can return 0 for a failed remote command.
metal_sustain_smoke() {
  local dir=$1
  SSH_T=30 rsudo "u=\$(/usr/bin/stat -f %u /dev/console); test \"\$u\" -ge 501 || exit 1
    /bin/launchctl asuser \"\$u\" /usr/bin/sudo -n -u \"#\$u\" -- $dir/piano-metal-sustain --onscreen --seconds 3 --width 1920 --height 1280 --draws 256 > $dir/smoke.csv 2> $dir/smoke.log
    rc=\$?; printf \"METAL_SUSTAIN_REMOTE_RC=%s\\n\" \"\$rc\"; exit \"\$rc\"" </dev/null
}

metal_sustain_detach() {
  local dir=$1
  # The text-mode rsudo wrapper single-quotes the remote command. The here-doc
  # passes the worker to Bash without expanding its variables in the outer shell.
  # shellcheck disable=SC2016
  SSH_T=4 rsudo "dir=$dir
  "'
    u=$(/usr/bin/stat -f %u /dev/console)
    test "$u" -ge 501 || exit 1
    worker=$(/bin/cat <<"METAL_SUSTAIN_WORKER"
dir=$1
[ ! -e "$dir/frames.stop" ] || exit 1
"$dir/piano-metal-sustain" --onscreen --minutes 10 --width 1920 --height 1280 --draws 256 \
  </dev/null >"$dir/frames.csv" 2>"$dir/frames.log" &
pid=$!
if ! { printf "%s\n" "$pid" >"$dir/frames.pid.tmp" && /bin/mv "$dir/frames.pid.tmp" "$dir/frames.pid"; }; then
  kill -TERM "$pid" 2>/dev/null
  wait "$pid"
  exit 1
fi
# A stop request can race with publication of the benchmark PID.
if [ -e "$dir/frames.stop" ]; then kill -TERM "$pid" 2>/dev/null; fi
rc=0
wait "$pid" || rc=$?
printf "%s\n" "$rc" >"$dir/frames.rc.tmp" && /bin/mv "$dir/frames.rc.tmp" "$dir/frames.rc"
METAL_SUSTAIN_WORKER
    ) || exit 1
    /usr/bin/nohup /bin/launchctl asuser "$u" /usr/bin/sudo -n -u "#$u" -- \
      /bin/bash -c "$worker" bash "$dir" </dev/null >/dev/null 2>"$dir/frames.launch.log" &
    echo METAL_SUSTAIN_LAUNCH_QUEUED
  ' </dev/null
}

metal_sustain_probe() {
  local dir=$1 action=$2
  # The SSH user owns the console session and can signal its benchmark directly.
  # shellcheck disable=SC2016
  SSH_T=4 rssh "dir=$dir action=$action
  "'
    report_rc() {
      [ -r "$dir/frames.rc" ] || return 1
      rc=$(/bin/cat "$dir/frames.rc") || return 1
      case "$rc" in ""|*[!0-9]*) return 1 ;; esac
      [ "$rc" -le 255 ] || return 1
      printf "METAL_SUSTAIN_REMOTE_RC=%s\n" "$rc"
    }
    if report_rc; then exit 0; fi
    if [ "$action" = stop ]; then : >"$dir/frames.stop" || exit 1; fi
    if [ ! -r "$dir/frames.pid" ]; then
      echo METAL_SUSTAIN_STARTING
      exit 0
    fi
    pid=$(/bin/cat "$dir/frames.pid") || exit 1
    case "$pid" in ""|0*|1|*[!0-9]*) exit 1 ;; esac
    if ! kill -0 "$pid" 2>/dev/null; then
      report_rc || echo METAL_SUSTAIN_EXIT_PENDING
    elif [ "$action" = poll ]; then
      echo METAL_SUSTAIN_RUNNING
    else
      # A stale PID can belong to another process if the receipt is missing.
      case "$(/bin/ps -ww -p "$pid" -o command=)" in
        "$dir/piano-metal-sustain "*)
          if ! report_rc; then kill -TERM "$pid" && echo METAL_SUSTAIN_TERM_SENT; fi ;;
        *) report_rc ;;
      esac
    fi
  ' </dev/null | tr -d '\r' |
    grep -xE 'METAL_SUSTAIN_(REMOTE_RC=[0-9]{1,3}|STARTING|EXIT_PENDING|RUNNING|TERM_SENT)'
}

metal_sustain_run() {
  local dir=$1 rc=0 saved_traps
  saved_traps=$(trap -p HUP INT TERM)
  trap 'metal_sustain_interrupted "$dir" 129' HUP
  trap 'metal_sustain_interrupted "$dir" 130' INT
  trap 'metal_sustain_interrupted "$dir" 143' TERM
  metal_sustain_poll "$dir" || rc=$?
  trap - HUP INT TERM
  eval "$saved_traps"
  return "$rc"
}

metal_sustain_interrupted() {
  trap '' HUP INT TERM
  metal_sustain_probe "$1" stop >&2 || true
  exit "$2"
}

metal_sustain_now() {
  REPLY=$SECONDS
}

metal_sustain_poll() {
  local dir=$1 launch status progress start next delay elapsed action REPLY
  launch=$(metal_sustain_detach "$dir" | tr -d '\r') || true
  if ! grep -qx METAL_SUSTAIN_LAUNCH_QUEUED <<< "$launch"; then
    printf 'metal-sustain: detached launch receipt missing\n' >&2
    metal_sustain_probe "$dir" stop >&2 || true
    return 1
  fi

  metal_sustain_now
  start=$REPLY
  next=$start
  while :; do
    metal_sustain_now
    delay=$((next - REPLY))
    if ((delay > 0)); then sleep "$delay"; fi
    metal_sustain_now
    elapsed=$((REPLY - start))
    action=poll
    # The final probe checks for completion first, then requests cancellation.
    if ((elapsed >= 720)); then action=stop; fi
    status=$(metal_sustain_probe "$dir" "$action") || true
    if [[ "$status" =~ ^METAL_SUSTAIN_REMOTE_RC=[0-9]{1,3}$ ]]; then
      printf '%s\n' "$status"
      return 0
    fi
    if [ "$action" = stop ]; then
      if [ "$status" = METAL_SUSTAIN_TERM_SENT ]; then
        printf 'metal-sustain: timed out after %ss, sent SIGTERM\n' "$elapsed" >&2
      else
        printf 'metal-sustain: timed out after %ss, SIGTERM unconfirmed\n' "$elapsed" >&2
      fi
      return 1
    fi
    case "$status" in
      METAL_SUSTAIN_STARTING) progress='waiting for benchmark PID' ;;
      METAL_SUSTAIN_RUNNING) progress='benchmark running' ;;
      METAL_SUSTAIN_EXIT_PENDING) progress='benchmark exited, waiting for exit status' ;;
      *) progress='status check failed' ;;
    esac
    printf 'metal-sustain: %ss elapsed, %s\n' "$elapsed" "$progress" >&2
    next=$((start + (elapsed / 20 + 1) * 20))
  done
}

metal_sustain_installed() {
  local hostdir=tooling/native/piano-metal-sustain
  local bin=${METAL_SUSTAIN_BIN:-out/piano-metal-sustain/piano-metal-sustain}
  local name=metal-sustain d want got tag lo hi old_mark
  local result="$R/$name"
  if [ "${DRY:-0}" = 1 ]; then verdict 'INFO metal-sustain dry run'; return 0; fi
  if [ ! -x "$bin" ] || [ ! -s "$C" ]; then
    verdict 'FAIL HARNESS metal-sustain: staged host binary or active recorder missing'; return 1
  fi
  mkdir -p "$result" || return 1
  # shellcheck disable=SC2016
  rssh 'u=$(/usr/bin/stat -f %u /dev/console); test "$u" -ge 501 && test "$u" = "$(/usr/bin/id -u)" && /usr/bin/pgrep -u "$u" -x Finder >/dev/null && echo METAL_SUSTAIN_GUI_OK' > "$result/gui.txt"
  if ! grep -qx METAL_SUSTAIN_GUI_OK "$result/gui.txt"; then
    verdict 'FAIL HARNESS metal-sustain: SSH user does not own a live Finder session'; return 1
  fi
  rssh '/usr/bin/pmset -g; /usr/bin/sw_vers' > "$result/environment.txt"
  d=$(rssh 'umask 077; /usr/bin/mktemp -d /private/var/tmp/piano-metal-sustain.XXXXXXXX' |
      tr -d '\r' | grep -xE '/private/var/tmp/piano-metal-sustain\.[A-Za-z0-9]{8}')
  if ! [[ "$d" =~ ^/private/var/tmp/piano-metal-sustain\.[A-Za-z0-9]{8}$ ]]; then
    verdict 'FAIL HARNESS metal-sustain: remote temporary directory missing'; return 1
  fi
  want=$(shasum -a 256 "$bin" | cut -d ' ' -f1)
  SSH_T=60 rssh "umask 077; /usr/bin/base64 -D > $d/piano-metal-sustain && /bin/chmod 755 $d/piano-metal-sustain && /usr/bin/shasum -a 256 $d/piano-metal-sustain" \
    < <(/usr/bin/base64 -i "$bin") > "$result/staging.txt"
  got=$(awk -v p="$d/piano-metal-sustain" '$2 == p {print $1}' "$result/staging.txt")
  if [ -z "$want" ] || [ "$want" != "$got" ]; then
    verdict 'FAIL HARNESS metal-sustain: staged executable SHA-256 differs'; return 1
  fi
  metal_sustain_smoke "$d" > "$result/smoke-run.txt" || true
  if ! grep -qx METAL_SUSTAIN_REMOTE_RC=0 "$result/smoke-run.txt" ||
     ! pull "$name/smoke.log" "$d/smoke.log" ||
     ! grep -aqE '^WINDOW .*gpu_timed=[1-9][0-9]* ' "$result/smoke.log"; then
    verdict "FAIL metal-sustain: three-second warmup check failed ($d/smoke.log)"; return 1
  fi

  # Allow the short smoke's demand vote to settle before the pre-run snapshot.
  rssh '/bin/sleep 5; echo METAL_SUSTAIN_SETTLED' > "$result/settle.txt"
  if ! grep -qx METAL_SUSTAIN_SETTLED "$result/settle.txt"; then
    verdict 'FAIL HARNESS metal-sustain: settle check did not return'; return 1
  fi
  metal_sustain_snapshot > "$result/before.txt"
  if ! grep -aqE '^at_us=[0-9]+ lvl=[0-9]+ mhz=[0-9]+' "$result/before.txt" ||
     ! grep -aqE '^replay [01] recoveries [0-9]+' "$result/before.txt"; then
    verdict 'FAIL HARNESS metal-sustain: installed GPU state or recovery sysctl unavailable'; return 1
  fi

  tag=${d##*.}
  old_mark=$MARK
  mark
  lo=$MARK
  rsudo "printf \"PIANO_METAL_SUSTAIN %s BEGIN\\n\" $tag > /dev/console" > "$result/mark-begin.txt"
  if ! waitfor "PIANO_METAL_SUSTAIN $tag BEGIN" 10; then
    MARK=$old_mark
    verdict 'FAIL HARNESS metal-sustain: console BEGIN marker missing'; return 1
  fi
  local run_failed=0
  metal_sustain_run "$d" > "$result/run.txt" || run_failed=1
  rsudo "printf \"PIANO_METAL_SUSTAIN %s END\\n\" $tag > /dev/console" > "$result/mark-end.txt"
  waitfor "PIANO_METAL_SUSTAIN $tag END" 10 || true
  hi=$(lines)
  MARK=$old_mark
  awk -v lo="$lo" -v hi="$hi" -v tag="$tag" '
    NR > hi { exit }
    NR > lo && index($0, "PIANO_METAL_SUSTAIN " tag " BEGIN") { capture=1 }
    capture { print }
    capture && index($0, "PIANO_METAL_SUSTAIN " tag " END") { exit }
  ' "$C" | tr -d '\r' > "$result/console.txt"
  metal_sustain_snapshot > "$result/after.txt"
  local pull_failed=0
  pull "$name/summary.log" "$d/frames.log" || pull_failed=1
  pull "$name/frames.csv" "$d/frames.csv" || pull_failed=1
  if [ "$run_failed" -ne 0 ]; then
    verdict "FAIL HARNESS metal-sustain: benchmark exit status unavailable ($d)"; return 1
  fi
  if [ "$pull_failed" -ne 0 ]; then
    verdict "FAIL HARNESS metal-sustain: evidence transfer failed ($d)"; return 1
  fi
  local grade_rc
  if python3 "$hostdir/grade.py" "$result" > "$result/grade.txt" 2>&1; then
    verdict 'PASS metal-sustain: 600-second installed Metal 3D benchmark (metal-sustain/grade.txt)'
    return 0
  else
    grade_rc=$?
  fi
  if [ "$grade_rc" -eq 2 ] && grep -q '^VOID installed Metal sustain (CONFOUND):' "$result/grade.txt"; then
    verdict 'VOID metal-sustain: external interaction in bounded console capture (metal-sustain/grade.txt)'
    return 2
  fi
  verdict 'FAIL metal-sustain: see metal-sustain/grade.txt'
  return 1
}
