#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Mac side of RAM16G milestone M0 (private/host-checks/20261125-ram16g/ANALYSIS.md): read-only memory snapshots of the INSTALLED boot over
# the maintenance ssh. MAIN SESSION ONLY, on the Mac with the tablet USB. Takes /tmp/piano-tablet.lock (refuses when held) unless
# LOCK_HELD=1. Nothing is uploaded: ram-m0-tab.sh goes over ssh on stdin (bash -s), as the ssh user (no root needed).
#   run-ram-m0.sh NAME [PHASES]     PHASES default "idle"; e.g. "idle apps apps10" with APPS_WAIT_S (default 600) before apps10
# The apps phase is whatever the operator opened (Safari, Terminal, Photos ...): the script asks for ENTER between phases when a TTY is
# attached, else it just waits APPS_WAIT_S. Results: OUT (default MAIN/private/host-checks/<today>-runs/NAME/), SUMMARY.txt.
set -uo pipefail
NAME=${1:?NAME}; PHASES=${2:-idle}
T=$(cd "$(dirname "$0")" && pwd)
TSSH=${TSSH:-/tmp/tssh}
MAIN=${MAIN:-$(git -C "$T" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's#/\.git$##')}
OUT=${OUT:-$MAIN/private/host-checks/$(date +%Y%m%d)-runs/$NAME}
LOCK=${LOCK:-/tmp/piano-tablet.lock}
APPS_WAIT_S=${APPS_WAIT_S:-600}
[ -e "$OUT" ] && { echo "refused: $OUT exists (fresh NAME per run)"; exit 2; }
for p in $PHASES; do case $p in idle|apps|apps10) ;; *) echo "refused: phase $p (idle|apps|apps10)"; exit 2;; esac; done
if [ "${LOCK_HELD:-0}" != 1 ]; then
  mkdir "$LOCK" 2>/dev/null || { echo "refused: $LOCK held (a job or install owns the tablet)"; exit 3; }
  trap 'rmdir "$LOCK" 2>/dev/null' EXIT
fi
mkdir -p "$OUT"
note() { echo "$(date +%H:%M:%S) $*" | tee -a "$OUT/host.log"; }
$TSSH 'echo SSH_OK; /usr/sbin/sysctl -n machdep.piano_data_io' > "$OUT/pre.txt" 2>&1
grep -q SSH_OK "$OUT/pre.txt" || { note "FATAL no ssh"; cat "$OUT/pre.txt"; exit 4; }
grep -q "mode=single-gate\|mode=concurrent" "$OUT/pre.txt" || { note "FATAL not an installed (persistent-store) boot: $(tr '\n' ' ' < "$OUT/pre.txt")"; exit 4; }
prev=
for p in $PHASES; do
  if [ "$p" = apps10 ] && [ "$prev" = apps ]; then note "waiting $APPS_WAIT_S s before apps10"; sleep "$APPS_WAIT_S"
  elif [ "$p" != idle ] && [ -t 0 ]; then read -r -p "open the app set for phase $p on the tablet, then ENTER " _
  fi
  note "snapshot $p"
  $TSSH "/bin/bash -s $p" < "$T/ram-m0-tab.sh" > "$OUT/$p.txt" 2>&1
  grep -q '^M0_DONE$' "$OUT/$p.txt" || { note "FATAL snapshot $p incomplete"; exit 5; }
  prev=$p
done
files=(); for p in $PHASES; do files+=("$OUT/$p.txt"); done
python3 -I "$T/summarize-ram-m0.py" "${files[@]}" | tee "$OUT/SUMMARY.txt"
