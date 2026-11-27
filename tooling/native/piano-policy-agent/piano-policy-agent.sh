#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano-policy-agent: the userland half of PianoPolicy (docs/policy.md section 4), for the native macOS on piano. Three things the
# kernel cannot see or do on its own:
#   1. macOS Low Power Mode lives in userland only: powerd keeps it as the notify name com.apple.system.lowpowermode (state 0/1)
#      and sends nothing to the kernel. Each change is written to the kernel: sysctl machdep.piano_policy_lpm=<0|1>. The same for
#      High Power Mode (Energy Mode's third option, docs/policy.md section 12; offered once the AppleSMC stand-in runs,
#      piano-smc=1): com.apple.system.highpowermode -> sysctl machdep.piano_policy_hpm=<0|1>.
#   2. No kernel call puts the display to sleep (k27 has no display wrangler: WindowServer owns display power). When the cover is
#      closed and closing does not sleep (PianoPolicy: on external power, or no system sleep), IOPMrootDomain says so
#      (AppleClamshellState Yes, AppleClamshellCausesSleep No) and powerd posts com.apple.system.powermanagement.clamshellstate:
#      then "pmset displaysleepnow" here, and the session locks (System Settings > Lock Screen: require the password immediately).
#   3. The power state of background services (docs/policy.md section 4.1): Normal (on power with Low Power Mode off, or High Power
#      on battery) or Save (Low Power Mode on, or on battery outside High Power). Save is applied after DEBOUNCE s of a stable
#      wish and never before MIN_UP s of uptime; Normal at once. macOS's scheduler (dasd) already defers the background work of
#      every re-enabled service on battery and in Low Power Mode (their tasks need external power or follow Low Power Mode), so the
#      list this agent eases in Save (PAUSE) is EMPTY. The mechanism is for a label measured to cost idle CPU that dasd leaves:
#      taskpolicy -b / -B on its pid (darwin background: CPU and I/O throttled, clients still answered) or launchctl bootout +
#      bootstrap. Never a signal, never a persistent override (no launchctl disable, no mdutil): every boot starts Normal, and a
#      restart of this agent undoes its own actions first. A label whose action fails twice (an rc, a failed reload, a crash
#      report after a transition) goes to broken.tsv: restored and left alone. The file slim/off is the kill switch (all undone,
#      nothing applied). Test-only entries come from slim/test-pause, read only while that file exists (the flag).
#      State, actions and failures: /private/var/db/piano-userland/slim/ (state, log, applied, fail.tsv, broken.tsv).
# Event driven: one notifyutil process waits on the names (no polling); a delayed Save or a crash check is one sleeping child that
# posts com.unixnu.piano.slim.tick. Stock tools only (notifyutil, sysctl, ioreg, pmset, launchctl, taskpolicy, logger). Runs as
# root. Lines go to the system log under piano-policy-agent (log show --predicate 'process == "logger"') and to slim/log.
#
# Installed by the userland pipeline (configs/userland.toml, tooling/userland/stage.py, docs/userland.md) as
# /usr/local/libexec/piano-policy-agent with its LaunchDaemon com.unixnu.piano-policy-agent.plist and the Aqua agent
# com.unixnu.piano-slim-session.plist (next to this file: it posts com.unixnu.piano.slim.session at each login).
# By hand (root shell), without the pipeline:
#   launchctl submit -l com.unixnu.piano-policy-agent -- /bin/sh /path/to/piano-policy-agent.sh
# and launchctl remove com.unixnu.piano-policy-agent to stop it. Other modes (root): --status (the slim state, its actions, the
# last log lines), --restore (undo every action of this boot now; with slim/off it stays undone), --event NAME (handle one event:
# lpm, hpm, lid, source, session, tick), --start (the start step alone: restore first, then Normal).
set -u
LPM=com.apple.system.lowpowermode
HPM=com.apple.system.highpowermode
LID=com.apple.system.powermanagement.clamshellstate
SRC=com.apple.system.powersources.source
SESSION=com.unixnu.piano.slim.session
TICK=com.unixnu.piano.slim.tick
R=${PIANO_USERLAND_ROOT:-}
PARENT=$R/private/var/db/piano-userland
SLIM=$PARENT/slim
DEBOUNCE=30   # s of a stable Save wish before it is applied (PD renegotiation and fast-charge flaps)
MIN_UP=120    # s of uptime before any Save (the first minutes of a boot)
WATCH=600     # s after a transition in which a label's new crash reports count as failures
CHECK=60      # s after a transition: one crash check (a tick)
# class C (SLIM-LIST.md section 3): "MODE DOMAIN LABEL PROCESS" per line; MODE bg (taskpolicy) or unload (bootout/bootstrap);
# DOMAIN system or gui (the console user's session). EMPTY by decision: dasd defers the services of SLIM-LIST.md section 2.
PAUSE=''
# class A (SLIM-LIST.md sections 2 and 5, always off: the install's persistent disable): never touched here, also not as a test entry
NEVER='com.apple.watchdogd com.apple.security.xprotectd com.apple.mobileactivationd com.apple.PerfPowerServices com.apple.security.cryptexd com.apple.AppSSODaemon com.apple.filesystems.fskitd com.apple.passd com.apple.routined'

say() { logger -t piano-policy-agent "$*" 2>/dev/null; slog "$*"; }

mode_value() {
  local value
  value=$(notifyutil -g "$1") || return 1
  value=$(printf '%s\n' "$value" | awk -v key="$1" '$1 == key && NF == 2 {print $2}')
  [ -n "$value" ] || return 1
  case "$value" in 1) echo 1 ;; *) echo 0 ;; esac
}

read_modes() {
  lpmv=$(mode_value "$LPM") || lpmv=unknown
  hpmv=$(mode_value "$HPM") || hpmv=unknown
  [ "$lpmv" != unknown ] && [ "$hpmv" != unknown ]
}

write_mode() {   # suffix, label, value
  if sysctl -w "machdep.piano_policy_$1=$3" > /dev/null 2>&1; then
    say "$2 $3: machdep.piano_policy_$1=$3"
  else
    say "$2 $3: machdep.piano_policy_$1 unavailable"
  fi
}

energy_modes() {
  local lpmv hpmv
  read_modes || { say "Energy Mode read failed: skipping sysctl writes"; return; }
  # Either notification may arrive after both values changed. Low Power takes precedence during the two writes.
  if [ "$lpmv" = 1 ]; then
    write_mode lpm "Low Power Mode" "$lpmv"
    write_mode hpm "High Power Mode" "$hpmv"
  else
    write_mode hpm "High Power Mode" "$hpmv"
    write_mode lpm "Low Power Mode" "$lpmv"
  fi
}

lid() {
  local r
  r=$(ioreg -r -c IOPMrootDomain -d 1) || return 0
  printf '%s\n' "$r" | grep -Eq '^[[:space:]]*"AppleClamshellState" = Yes[[:space:]]*$' || return 0
  if printf '%s\n' "$r" | grep -Eq '^[[:space:]]*"AppleClamshellCausesSleep" = Yes[[:space:]]*$'; then
    say "cover closed: the system sleeps"
    return
  fi
  printf '%s\n' "$r" | grep -Eq '^[[:space:]]*"AppleClamshellCausesSleep" = No[[:space:]]*$' || return 0
  pmset displaysleepnow && say "cover closed without sleep: display sleep (the screen locks)"
}

# ---------------------------------------------------------------- the power state of background services (slim)
up() {   # uptime in s (boot time moves with a clock set, so the difference stays the uptime)
  local b n
  b=$(sysctl -n kern.boottime 2>/dev/null | sed -n 's/^{ sec = \([0-9][0-9]*\),.*/\1/p')
  n=$(date +%s)
  case "$b" in ''|*[!0-9]*) echo 0; return ;; esac
  case "$n" in ''|*[!0-9]*) echo 0; return ;; esac
  echo $((n - b))
}
bootid() {
  local u
  u=$(sysctl -n kern.bootsessionuuid 2>/dev/null) || return 1
  case "$u" in ''|*[!A-Fa-f0-9-]*) return 1 ;; esac
  printf '%s\n' "$u"
}
console() { local u; u=$(stat -f %u /dev/console 2>/dev/null); case "$u" in ''|*[!0-9]*) u=0 ;; esac; echo "$u"; }
slog() {
  local n
  [ -d "$SLIM" ] || return 0
  printf '%s up=%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$(up)" "$*" >> "$SLIM/log" 2>/dev/null || return 0
  n=$(wc -l < "$SLIM/log" | tr -dc 0-9)
  if [ "${n:-0}" -gt 2000 ]; then tail -n 1000 "$SLIM/log" > "$SLIM/log.new" && mv -f "$SLIM/log.new" "$SLIM/log"; fi
}
sdir() {   # the state folder: root only; its parent (shared with the account's passd policy) traversable
  if [ ! -d "$PARENT" ]; then mkdir -p "$R/private/var/db" && mkdir "$PARENT" && chmod 711 "$PARENT" || return 1; fi
  [ ! -L "$SLIM" ] || return 1
  [ -d "$SLIM" ] || mkdir "$SLIM" || return 1
  chmod 700 "$SLIM"
}
lock() {
  local i=0 p
  until mkdir "$SLIM/lock" 2>/dev/null; do
    p=$(cat "$SLIM/lock/pid" 2>/dev/null)
    if [ -n "$p" ] && ! ps -p "$p" > /dev/null 2>&1; then rm -rf "$SLIM/lock"; continue; fi
    i=$((i + 1)); [ "$i" -lt 100 ] || return 1
    sleep 0.1
  done
  echo $$ > "$SLIM/lock/pid"
}
unlock() { rm -rf "$SLIM/lock" && return "${1:-0}"; }
num() {
  local v
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  v=${1#"${1%%[!0]*}"}   # shell arithmetic treats a leading zero as octal
  [ "${#v}" -le 18 ] || return 1   # leave room for arithmetic on 64-bit counters and times
  printf '%s\n' "${v:-0}"
}
load() {
  local kv
  st=normal gen=0 since= seq=0 due=0
  if [ -f "$SLIM/state" ]; then
    for kv in $(head -1 "$SLIM/state"); do
      case "$kv" in
        state=*) st=${kv#state=} ;;
        gen=*) gen=$(num "${kv#gen=}") || gen=0 ;;
        since=*) since=$(num "${kv#since=}") ;;
        seq=*) seq=$(num "${kv#seq=}") || seq=0 ;;
        due=*) due=$(num "${kv#due=}") || due=0 ;;
      esac
    done
  fi
  case "$st" in normal|pending|save|off) ;; *) st=normal ;; esac
  if [ -z "$since" ]; then
    if [ "$st" = pending ]; then st=normal; due=0; fi   # a pending Save needs a valid start time
    since=0
  fi
}
store() {   # EVENT
  local na nb
  na=0; [ -f "$SLIM/applied" ] && na=$(grep -c . "$SLIM/applied")
  nb=0; [ -f "$SLIM/broken.tsv" ] && nb=$(grep -c . "$SLIM/broken.tsv")
  if [ ! -d "$SLIM/state" ] && printf 'state=%s gen=%s since=%s seq=%s due=%s want=%s event=%s lpm=%s hpm=%s source=%s console=%s applied=%s broken=%s test=%s boot=%s up=%s\n' \
    "$st" "$gen" "$since" "$seq" "$due" "${want:-normal}" "$1" "${lpmv:-0}" "${hpmv:-0}" "${srcv:-unknown}" "$(console)" "$na" "$nb" \
    "$([ -f "$SLIM/test-pause" ] && echo on || echo off)" "$(bootid || echo unknown)" "$(up)" > "$SLIM/state.new" &&
    mv -f "$SLIM/state.new" "$SLIM/state"; then return 0; fi
  say "slim: could not update $SLIM/state for event $1"
  return 1
}
readpower() {   # lpmv, hpmv, srcv and want (normal, save, off)
  local p
  read_modes || slog "slim: Energy Mode read failed"
  p=$(pmset -g ps 2>/dev/null | head -1)
  case "$p" in
    *"'Battery Power'"*) srcv=battery ;; *"'AC Power'"*) srcv=ac ;; *"'UPS Power'"*) srcv=ups ;; *) srcv=unknown ;;
  esac
  if [ -e "$SLIM/off" ]; then want=off
  elif [ "$lpmv" = unknown ] || [ "$hpmv" = unknown ]; then want=unknown; return 1
  elif [ "$lpmv" = 1 ] || { [ "$srcv" = battery ] && [ "$hpmv" = 0 ]; }; then want=save
  else want=normal; fi
}
plist() { if [ "$1" = system ]; then echo "$R/System/Library/LaunchDaemons/$2.plist"; else echo "$R/System/Library/LaunchAgents/$2.plist"; fi; }
loaded() { launchctl print "$1" < /dev/null > /dev/null 2>&1; }
jobpid() { launchctl print "$1" < /dev/null 2>/dev/null | sed -n 's/^[[:space:]]*pid = \([0-9][0-9]*\)[[:space:]]*$/\1/p' | head -1; }
inlist() { [ -f "$2" ] && awk -v l="$1" '$1 == l {f = 1} END {exit !f}' "$2"; }
entries() {   # VERBOSE: the class-C list and the test list (behind its flag file), validated: MODE DOMAIN LABEL PROCESS TAG
  printf '%s\n' "$PAUSE" > "$SLIM/entries.src"
  addents "$SLIM/entries.src" base "$1"
  if [ -f "$SLIM/test-pause" ]; then addents "$SLIM/test-pause" test "$1"; fi
}
addents() {   # FILE TAG VERBOSE
  local m d l p x why
  while read -r m d l p x || [ -n "$m$d$l$p$x" ]; do
    case "$m" in ''|'#'*) continue ;; esac
    why=''
    case "$m" in bg|unload) ;; *) why="mode $m" ;; esac
    case "$d" in system|gui) ;; *) why="domain $d" ;; esac
    case "$l" in ''|*[!A-Za-z0-9._-]*) why="label" ;; esac
    case "$p" in ''|*[!A-Za-z0-9._-]*) why="process name" ;; esac
    [ -z "$x" ] || why='extra fields'
    case " $NEVER " in *" $l "*) why='class A (always off: never touched)' ;; esac
    if [ -z "$why" ] && [ "$m" = unload ] && [ ! -f "$(plist "$d" "$l")" ]; then why="no $(plist "$d" "$l")"; fi
    if [ -n "$why" ]; then [ "$3" = verbose ] && slog "entry refused ($2: $m $d $l): $why"; continue; fi
    if inlist "$l" "$SLIM/broken.tsv"; then [ "$3" = verbose ] && slog "entry skipped ($2: $l): in broken.tsv"; continue; fi
    echo "$m $d $l $p $2"
  done < "$1"
}
watch() {   # LABEL PROCESS BOOT: its crash reports count for WATCH s from now
  mkdir -p "$SLIM/watch" && echo "$3 $(up) $2" > "$SLIM/watch/$1"
}
crashcheck() {
  local f l b t p n bid now
  [ -d "$SLIM/watch" ] || return 0
  bid=$(bootid) || return 0
  now=$(up)
  for f in "$SLIM"/watch/*; do
    [ -f "$f" ] || continue
    l=${f##*/}
    read -r b t p < "$f" || { rm -f "$f"; continue; }
    t=$(num "$t") || { rm -f "$f"; continue; }
    if [ "$b" != "$bid" ] || [ $((now - t)) -gt "$WATCH" ]; then rm -f "$f"; continue; fi
    n=$(find "$R/Library/Logs/DiagnosticReports" "$R"/Users/*/Library/Logs/DiagnosticReports -maxdepth 1 -type f \
          -name "${p}[-_]*.ips" -newer "$f" 2>/dev/null | wc -l | tr -dc 0-9)
    if [ "${n:-0}" -gt 0 ]; then touch "$f"; failed "$l" "$n new crash report(s) of $p"; fi
  done
}
failed() {   # LABEL REASON: one failure; the second marks the label broken
  local n
  n=$(awk -v l="$1" '$1 == l {print $2}' "$SLIM/fail.tsv" 2>/dev/null | tail -1)
  n=$(num "$n") || n=0
  n=$((n + 1))
  { [ -f "$SLIM/fail.tsv" ] && awk -v l="$1" '$1 != l' "$SLIM/fail.tsv"; printf '%s %s\n' "$1" "$n"; } > "$SLIM/fail.tsv.new" &&
    mv -f "$SLIM/fail.tsv.new" "$SLIM/fail.tsv"
  say "FAIL $1: $2 (failure $n of 2)"
  [ "$n" -lt 2 ] || broken "$1" "$2"
}
broken() {   # LABEL REASON
  inlist "$1" "$SLIM/broken.tsv" || printf '%s\t%s\n' "$1" "$2" >> "$SLIM/broken.tsv"
  say "BROKEN $1: $2: restored and left alone from now on (remove its line from $SLIM/broken.tsv to retry)"
  [ "${IN_RESTORE:-0}" = 1 ] || restore "$1"
}
apply() {   # VERBOSE: Save for every entry (idempotent: a bg entry follows its job's new pid)
  local bid cu m d l p tag dom pid rc
  bid=$(bootid) || { slog "slim: Save deferred: boot session unavailable"; return 0; }
  cu=$(console)
  entries "$1" > "$SLIM/entries" || return 1
  if ! printf '' >> "$SLIM/applied" || ! prune; then
    say "slim: Save deferred: could not update $SLIM/applied"
    return 1
  fi
  while read -r m d l p tag; do
    if [ "$d" = gui ]; then
      if [ "$cu" -lt 501 ]; then [ "$1" = verbose ] && slog "save $l: no console session (gui entries wait for a login)"; continue; fi
      dom=gui/$cu
    else dom=system; fi
    case "$m" in
      bg)
        pid=$(jobpid "$dom/$l")
        if [ -z "$pid" ]; then [ "$1" = verbose ] && slog "save $l: not running (on demand): nothing to ease"; continue; fi
        awk -v d="$dom" -v l="$l" '$2 == "bg" && $3 == d && $4 == l {f = 1} END {exit !f}' "$SLIM/applied" && continue
        taskpolicy -b -p "$pid" < /dev/null > /dev/null 2>&1; rc=$?
        if [ "$rc" = 0 ]; then
          echo "$bid bg $dom $l $pid $p $tag" >> "$SLIM/applied"
          watch "$l" "$p" "$bid"; say "save $l ($tag): taskpolicy -b -p $pid (darwin background)"
        else failed "$l" "taskpolicy -b -p $pid rc $rc"; fi ;;
      unload)
        awk -v d="$dom" -v l="$l" '$2 == "unload" && $3 == d && $4 == l {f = 1} END {exit !f}' "$SLIM/applied" && continue
        if ! loaded "$dom/$l"; then [ "$1" = verbose ] && slog "save $l: not loaded: nothing to unload"; continue; fi
        launchctl bootout "$dom/$l" < /dev/null > /dev/null 2>&1; rc=$?
        if [ "$rc" = 0 ] || ! loaded "$dom/$l"; then
          echo "$bid unload $dom $l - $p $tag" >> "$SLIM/applied"
          watch "$l" "$p" "$bid"; say "save $l ($tag): launchctl bootout $dom/$l (rc $rc)"
        else failed "$l" "launchctl bootout $dom/$l rc $rc"; fi ;;
    esac
  done < "$SLIM/entries"
}
restore() {   # [LABEL]: undo this boot's actions (all, or one label's); an undo that can be retried stays recorded
  local bid b m dom l pid p tag rc rc2 IN_RESTORE=1
  [ -s "$SLIM/applied" ] || return 0
  [ -f "$SLIM/applied" ] || return 1
  bid=$(bootid) || { say "slim: restore deferred: boot session unavailable"; return 1; }
  printf '' > "$SLIM/applied.keep" || return 1
  while read -r b m dom l pid p tag; do
    [ -n "$l" ] || continue
    if [ -n "${1:-}" ] && [ "$l" != "$1" ]; then
      echo "$b $m $dom $l $pid $p $tag" >> "$SLIM/applied.keep" || return 1
      continue
    fi
    if [ "$b" != "$bid" ]; then slog "normal $l: recorded on another boot: dropped (every boot starts Normal)"; continue; fi
    case "$m" in
      bg)
        if [ "$(jobpid "$dom/$l")" = "$pid" ]; then
          taskpolicy -B -p "$pid" < /dev/null > /dev/null 2>&1; rc=$?
          if [ "$rc" = 0 ]; then watch "$l" "$p" "$bid"; say "normal $l: taskpolicy -B -p $pid"
          else
            failed "$l" "taskpolicy -B -p $pid rc $rc"
            inlist "$l" "$SLIM/broken.tsv" || echo "$b $m $dom $l $pid $p $tag" >> "$SLIM/applied.keep" || return 1
          fi
        else slog "normal $l: pid $pid gone (its successor runs unthrottled): nothing to undo"; fi ;;
      unload)
        case "$dom" in gui/*) if ! loaded "$dom"; then slog "normal $l: session $dom ended: the next login loads it"; continue; fi ;; esac
        if loaded "$dom/$l"; then slog "normal $l: loaded again already"; continue; fi
        launchctl bootstrap "$dom" "$(plist "${dom%%/*}" "$l")" < /dev/null > /dev/null 2>&1; rc=$?; rc2=-
        if ! loaded "$dom/$l"; then
          sleep 2
          launchctl bootstrap "$dom" "$(plist "${dom%%/*}" "$l")" < /dev/null > /dev/null 2>&1; rc2=$?
        fi
        if loaded "$dom/$l"; then watch "$l" "$p" "$bid"; say "normal $l: launchctl bootstrap $dom (rc $rc/$rc2)"
        else broken "$l" "launchctl bootstrap $dom failed twice (rc $rc, $rc2): it loads at the next login or boot"; fi ;;
    esac
  done < "$SLIM/applied" || return 1
  mv -f "$SLIM/applied.keep" "$SLIM/applied"
}
prune() {   # records whose action no longer holds are dropped: the job loaded again (a new session, by hand), the process gone
  local b m dom l pid p tag
  [ -s "$SLIM/applied" ] || return 0
  printf '' > "$SLIM/applied.new" || return 1
  while read -r b m dom l pid p tag; do
    [ -n "$l" ] || continue
    case "$m" in
      bg) if [ "$(jobpid "$dom/$l")" != "$pid" ]; then slog "save $l: pid $pid gone: record dropped"; continue; fi ;;
      unload) if loaded "$dom/$l"; then slog "save $l: loaded again (a new session, or by hand): record dropped"; continue; fi ;;
    esac
    echo "$b $m $dom $l $pid $p $tag" >> "$SLIM/applied.new" || return 1
  done < "$SLIM/applied" || return 1
  mv -f "$SLIM/applied.new" "$SLIM/applied"
}
schedule() {   # UPTIME: one tick then (a sleeping child posts it)
  local d
  [ "$due" = "$1" ] && return 0
  d=$(( $1 - $(up) )); [ "$d" -ge 1 ] || d=1
  due=$1
  ( sleep "$d"; notifyutil -p "$TICK" ) < /dev/null > /dev/null 2>&1 &
}
max() { if [ "$1" -ge "$2" ]; then echo "$1"; else echo "$2"; fi; }
slim() {   # EVENT
  local now
  sdir || { say "slim: no state folder $SLIM: event $1 ignored"; return 0; }
  lock || { say "slim: $SLIM/lock held: event $1 ignored"; return 0; }
  load; seq=$((seq + 1))
  readpower || { unlock; return; }
  now=$(up)
  crashcheck
  case "$want" in
    off)
      if [ "$st" != off ] || [ -s "$SLIM/applied" ]; then
        restore || { unlock; return 1; }
        gen=$((gen + 1))
        [ "$st" = off ] || say "slim: kill switch $SLIM/off: every action undone, nothing applied (gen $gen)"
        st=off
      fi ;;
    normal)
      if [ "$st" = pending ]; then gen=$((gen + 1)); say "slim: Save cancelled after $((now - since)) s (event $1: lpm=$lpmv hpm=$hpmv source=$srcv; gen $gen)"; st=normal
      elif [ "$st" != normal ]; then
        restore || { unlock; return 1; }
        gen=$((gen + 1))
        say "slim: Normal (event $1: lpm=$lpmv hpm=$hpmv source=$srcv; was $st; gen $gen)"; st=normal
        schedule $((now + CHECK))
      elif [ -s "$SLIM/applied" ]; then restore || { unlock; return 1; }
      fi ;;
    save)
      case "$st" in
        save) apply quiet || { unlock; return 1; } ;;
        normal|off)
          st=pending; since=$now; gen=$((gen + 1))
          say "slim: Save wanted (event $1: lpm=$lpmv hpm=$hpmv source=$srcv): applied once stable for $DEBOUNCE s at uptime >= $MIN_UP (gen $gen)"
          schedule "$(max $((now + DEBOUNCE)) "$MIN_UP")" ;;
        pending)
          if [ $((now - since)) -ge "$DEBOUNCE" ] && [ "$now" -ge "$MIN_UP" ]; then
            st=save; gen=$((gen + 1))
            say "slim: Save (stable $((now - since)) s; lpm=$lpmv hpm=$hpmv source=$srcv; gen $gen)"
            apply verbose || { unlock; return 1; }
            schedule $((now + CHECK))
          else schedule "$(max $((since + DEBOUNCE)) "$MIN_UP")"; fi ;;
      esac ;;
  esac
  store "$1"; unlock "$?"
}
slim_start() {   # every boot starts Normal; after a restart of this agent its own actions are undone first
  local n
  sdir || { say "slim: no state folder $SLIM"; return 1; }
  lock || { say "slim: $SLIM/lock held at start"; return 1; }
  load; readpower || :
  if [ -s "$SLIM/applied" ]; then
    n=$(awk -v b="$(bootid)" '$1 == b' "$SLIM/applied" | grep -c .)
    restore || { unlock; return 1; }
    say "slim: start: restore first ($n action(s) of this boot undone; other boots' records dropped)"
  fi
  st=normal; since=0; due=0; gen=$((gen + 1))
  store start; unlock "$?" || return 1
  say "slim: start: Normal (gen $gen; class-C entries: $(printf '%s\n' "$PAUSE" | grep -c '[^[:space:]]'), test list $([ -f "$SLIM/test-pause" ] && echo on || echo off))"
}

case "${1:-}" in
  '') ;;
  --status)
    for f in state applied broken.tsv fail.tsv; do echo "== $f"; cat "$SLIM/$f" 2>/dev/null; done
    echo "== log"; tail -n "${2:-40}" "$SLIM/log" 2>/dev/null; exit 0 ;;
  --restore)
    sdir && lock || exit 1
    load; readpower || :
    restore || { unlock; exit 1; }
    st=normal; since=0; gen=$((gen + 1)); store restore; unlock "$?" || exit 1
    say "slim: restore by hand: every action of this boot undone (Normal; $SLIM/off keeps it so)"; exit 0 ;;
  --start) slim_start; exit $? ;;
  --event)
    case "${2:-}" in
      lpm|hpm) energy_modes; slim "$2" ;; lid) lid; exit 0 ;; source|session|tick) slim "$2" ;;
      *) echo 'usage: piano-policy-agent --event lpm|hpm|lid|source|session|tick' >&2; exit 2 ;;
    esac
    exit $? ;;
  *) echo 'usage: piano-policy-agent [--status [N] | --restore | --start | --event NAME]' >&2; exit 2 ;;
esac

say "started (pid $$): Low Power Mode -> machdep.piano_policy_lpm; High Power Mode -> machdep.piano_policy_hpm; a cover closed without sleep -> display sleep; the power state of background services (slim)"
energy_modes
slim_start || exit 1
slim start
notifyutil -w "$LPM" -w "$HPM" -w "$LID" -w "$SRC" -w "$SESSION" -w "$TICK" | while read -r key _; do
  case "$key" in
    "$LPM") energy_modes; slim lpm ;;
    "$HPM") energy_modes; slim hpm ;;
    "$LID") lid ;;
    "$SRC") slim source ;;
    "$SESSION") slim session ;;
    "$TICK") slim tick ;;
  esac
done
say "notifyutil ended: exiting (launchd starts the agent again)"
