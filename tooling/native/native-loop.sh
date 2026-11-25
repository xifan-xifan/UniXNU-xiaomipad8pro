#!/bin/bash
# One native test without Ubuntu (2026-11-11). The tablet waits in ABL fastboot:
#   fastboot boot IMAGE -> XNU runs until piano-reset-after resets it (k27 0057: ABL stays in fastboot)
#   -> fastboot boot DUMP (out/ram-dump: the same with piano-reset-after=1) -> while its UEFI runs, the test's XNU log is pulled from
#   the cvp@9cb00000 mailbox (fastboot -s piano-uefi oem dump: read-only, our in-RAM UEFI, never ABL) -> DUMP's XNU resets at once
#   -> the tablet is parked in ABL fastboot again. RAM boots only.
#   tooling/native/native-loop.sh IMAGE [DUMP_IMAGE]
# Logs: logs/native-runs/<ts>-loop/{loop.log,dump.bin,xnu-console.txt,reader.txt}.
set -euo pipefail
IMG=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
D=$(cd "$(dirname "$0")/../.." && pwd)
DUMP=${2:-$D/out/ram-dump/$(basename "$1")}
[ -f "$HOME/.config/piano/env" ] && . "$HOME/.config/piano/env"
SERIAL=${PIANO_SERIAL:?set PIANO_SERIAL in ~/.config/piano/env}
export PATH=/opt/homebrew/share/android-commandlinetools/platform-tools:$PATH
L=$D/logs/native-runs/$(date +%Y%m%d-%H%M%S)-loop; mkdir -p "$L"
log() { echo "$(date +%T) $*" | tee -a "$L/loop.log"; }
abl() { /tmp/tmo.sh 5 fastboot devices | grep -q "^${SERIAL}[[:space:]]"; }
uefi() { /tmp/tmo.sh 5 fastboot devices | grep -q "^piano-uefi[[:space:]]"; }
wait_for() { local what=$1 deadline=$(( $(date +%s) + $2 ));
  while :; do $what && return 0; [ "$(date +%s)" -lt "$deadline" ] || return 1; sleep 0.5; done; }
abl_answers() { /tmp/tmo.sh 10 fastboot -s "$SERIAL" getvar product 2>&1 | grep -q '^product:'; }
# Never apply a host timeout to an OUT download: killing it can strand ABL.
boot_ram() { local rc=0 marker=${LOOP_STATUS_FILE:-$L/status}.boot-inflight
  if [ -n "${PRR_RUN_RESULTS:-}" ]; then
    local epoch elapsed
    epoch=$(date +%s)
    if [ -n "${PRR_READER_ABL_EPOCH:-}" ]; then
      elapsed=$(( epoch - PRR_READER_ABL_EPOCH ))
      if [ "$elapsed" -lt 0 ] || [ "$elapsed" -ge 60 ]; then
        : > "$PRR_RUN_RESULTS/prr-boot-refused"
        echo "PRR reader expired before starting the next boot"
        return 2
      fi
    fi
    printf '%s\n' "$epoch" > "$PRR_RUN_RESULTS/prr-boot-epoch"
  fi
  : > "$marker"
  fastboot -s "$SERIAL" boot "$1" || rc=$?
  rm -f "$marker"; return "$rc"; }
[ ! -e /tmp/piano-tablet.recovery-required ] || { log "FATAL recovery required; no boot attempted"; exit 3; }
RESET=$(strings "$IMG" | grep -o 'piano-reset-after=[0-9][0-9]*' | head -1 | cut -d= -f2 || true); RESET=${RESET:-45}
log "image $(shasum -a 256 "$IMG" | cut -c1-16), reset after ${RESET} s; dump image $(shasum -a 256 "$DUMP" | cut -c1-16)"
wait_for abl 240 || { log "no ABL fastboot (parked kernel? power + volume-down 15-20 s)"; exit 1; }
t0=$(date +%s)
boot_ram "$IMG" > "$L/fastboot.log" 2>&1 || { cat "$L/fastboot.log"; exit 1; }
wait_for uefi 40 || { log "our UEFI did not appear"; exit 1; }
wait_for "eval ! uefi" 40 || true
log "XNU started after $(( $(date +%s) - t0 )) s"
wait_for abl $((RESET + 150)) || { log "no ABL fastboot within $((RESET + 150)) s (parked? power + volume-down 15-20 s)"; exit 2; }
if [ -n "${PRR_RUN_RESULTS:-}" ]; then
  date +%s > "$PRR_RUN_RESULTS/prr-abl-epoch"
fi
log "back in ABL fastboot after $(( $(date +%s) - t0 )) s; checking dump eligibility"
abl_answers || { log "FATAL ABL enumerates but does not answer after the test; no dump attempted"; exit 3; }
if [ -n "${PRR_RUN_RESULTS:-}" ]; then
  [ -z "${DUMP_GUARD_RECORD:-}" ] || { log "FATAL PRR qualification forbids a dump boot"; exit 2; }
  log "PRR pair: no dump boot, ABL verified"
  exit 0
fi
# Optional mailbox export must never trade away the only working recovery path.
# 3750 mV is a conservative scheduling policy, NOT a measured PMIC cutoff.
voltage=$(/tmp/tmo.sh 10 fastboot -s "$SERIAL" getvar battery-voltage 2>&1 || true)
mv=$(printf '%s\n' "$voltage" | sed -nE 's/^(\(bootloader\) )?battery-voltage: ([0-9]+)[[:space:]]*$/\2/p' | head -1)
case ${DUMP_MIN_MV:-3750} in ''|*[!0-9]*) log "FATAL invalid DUMP_MIN_MV"; exit 2;; esac
case "$mv" in ''|*[!0-9]*) log "dump skipped: battery voltage unknown; ABL retained"; exit 0;; esac
if [ "$mv" -lt "${DUMP_MIN_MV:-3750}" ]; then
  log "dump skipped: battery ${mv} mV below ${DUMP_MIN_MV:-3750} mV policy; ABL retained"; exit 0
fi
# No current dump image has this qualification. Default: keep the live console,
# skip the second boot. Main may supply a record ONLY after an independent reset
# path (including the ABL-to-USB gap) has been forced to fail and returned to ABL.
# A kernel piano-reset-after or an EFI timer that starts later does not qualify.
if [ -z "${DUMP_GUARD_RECORD:-}" ]; then
  log "dump skipped: no qualified pre-USB reset path; ABL retained"; exit 0
fi
if ! DUMP_RETURN_BOUND=$(python3 - "$DUMP" "$DUMP_GUARD_RECORD" <<'GUARD'
import hashlib, json, pathlib, sys
try:
    image, record = map(pathlib.Path, sys.argv[1:])
    p = json.loads(record.read_text())
    digest = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
    assert p['schema'] == 1 and p['image_sha256'] == digest(image)
    assert p['pre_usb_reset_qualified'] is True and p['covers_abl_handoff'] is True
    assert p['returns_to'] == 'abl-fastboot'
    bound = p['return_bound_s']
    assert type(bound) is int and 1 <= bound <= 120
    evidence = pathlib.Path(p['evidence_path'])
    assert evidence.is_absolute() and evidence.is_file()
    assert p['evidence_sha256'] == digest(evidence)
    print(bound)
except Exception:
    sys.exit(1)
GUARD
); then log "dump skipped: invalid/reset qualification does not match image; ABL retained"; exit 0; fi
# A host wait cannot reset a device with no USB. This wait observes the qualified
# independent reset path; failure is fatal and must interlock the queue.
dump_t0=$(date +%s)
dump_return() { local left=$(( DUMP_RETURN_BOUND - $(date +%s) + dump_t0 ));
  [ "$left" -ge 0 ] || left=0
  if wait_for abl "$left" && abl_answers; then log "dump: ABL answers again"; return 0; fi
  log "FATAL dump left no verified ABL reset path; recovery required"; return 3; }
log "dump: qualified boot, battery ${mv} mV; return bound ${DUMP_RETURN_BOUND} s"
boot_ram "$DUMP" >> "$L/fastboot.log" 2>&1 || { cat "$L/fastboot.log"; dump_return || exit 3; exit 2; }
wait_for uefi 40 || { log "dump: our UEFI did not appear"; dump_return || exit 3; exit 2; }
dump_rc=0
if /tmp/tmo.sh 15 fastboot -s piano-uefi oem dump 9cb00000 700000 > /dev/null 2>&1 && (cd "$L" && /tmp/tmo.sh 15 fastboot -s piano-uefi get_staged dump.bin > /dev/null 2>&1); then
  if python3 "$D/tooling/native/xnu-log.py" decode "$L/dump.bin" > "$L/xnu-console.txt" 2> "$L/reader.txt"; then
    log "log: $(cat "$L/reader.txt")"
  else log "dump decode failed; see reader.txt"; dump_rc=2; fi
else log "dump failed or timed out; waiting only for the qualified reset path"; dump_rc=2; fi
dump_return || exit 3
log "parked in ABL fastboot after $(( $(date +%s) - t0 )) s"
exit "$dump_rc"
