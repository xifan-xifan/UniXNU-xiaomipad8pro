#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Exercise the collector on the Mac and the runner with a local fake SSH transport.
set -uo pipefail
[ "$(uname)" = Darwin ] || { echo "SKIP: needs macOS (vm_stat, memory_pressure)"; exit 0; }
T=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/ram-m0-test.XXXXXX") || exit 1
trap 'rm -rf "$W"' EXIT
pass=0 fail=0
ok() {
  local name=$1
  shift
  if "$@"; then
    echo "PASS $name"
    pass=$((pass+1))
  else
    echo "FAIL $name"
    fail=$((fail+1))
  fi
}
expect_status() {
  local expected=$1 actual=0
  shift
  "$@" || actual=$?
  [ "$actual" = "$expected" ]
}

bash "$T/ram-m0-tab.sh" idle > "$W/idle.txt" 2>&1
ok "tab: M0_DONE" grep -Fxq 'M0_DONE' "$W/idle.txt"
if memsize=$(sysctl -n hw.memsize 2>/dev/null); then
  ok "tab: hw.memsize read" grep -Fxq "hw.memsize=$memsize" "$W/idle.txt"
  memsize_mib='[0-9]+'
else
  ok "tab: hw.memsize unavailable" grep -Fxq 'MISSING sysctl hw.memsize' "$W/idle.txt"
  memsize_mib='-'
fi
ok "tab: vm_stat section" grep -q '^Pages free:' "$W/idle.txt"
ok "tab: tier telemetry attempted" grep -Eq '^(machdep\.piano_gpu_tier=|MISSING sysctl machdep\.piano_gpu_tier$)' "$W/idle.txt"
if grep -nE 'memory_pressure +-' "$T/ram-m0-tab.sh"; then
  ok "tab: never applies pressure" false
else
  ok "tab: never applies pressure" true
fi
summary_rc=0
python3 -I "$T/summarize-ram-m0.py" "$W/idle.txt" > "$W/sum.txt" || summary_rc=$?
ok "summary rc 0" test "$summary_rc" = 0
ok "summary line" grep -Eq "^phase=idle memsize_mib=$memsize_mib " "$W/sum.txt"
ok "summary hint" grep -q '^M0 HINT' "$W/sum.txt"

cat > "$W/fssh" <<'FAKE'
#!/bin/bash
case "$1" in
  *SSH_OK*)
    case "${MOCK_SSH_SCENARIO:-}" in
      missing-ssh) exit 255 ;;
      partial-pre) printf '%s\n' SSH_OK 'mode=single-gate fake'; exit 255 ;;
      invalid-boot) printf '%s\n' SSH_OK 'mode=concurrent-old'; exit 0 ;;
      missing-sentinel) printf '%s\n' NOT_SSH_OK 'mode=concurrent fake'; exit 0 ;;
      *) printf '%s\n' SSH_OK 'mode=single-gate fake' ;;
    esac ;;
  '/bin/bash -s idle'|'/bin/bash -s apps'|'/bin/bash -s apps10')
    case "${MOCK_SSH_SCENARIO:-}" in
      partial-snapshot) printf 'phase=idle\n'; exit 0 ;;
      failed-snapshot) printf 'phase=idle\nM0_DONE\n'; exit 255 ;;
      *) exec /bin/bash -s "${1##* }" ;;
    esac ;;
  *) exit 64 ;;
esac
FAKE
chmod +x "$W/fssh"
run_fake() {
  local run=$1 phases=$2
  TSSH=$W/fssh LOCK=$W/lock OUT=$W/$run APPS_WAIT_S=0 \
    bash "$T/run-ram-m0.sh" "$run" "$phases" </dev/null > "$W/runner.log" 2>&1
}

ok "run: fake SSH drives all phases" run_fake run1 $'idle apps\napps10'
for phase in idle apps apps10; do
  ok "run: $phase summarized" grep -q "^phase=$phase " "$W/run1/SUMMARY.txt"
done
ok "run: lock released" test ! -e "$W/lock"
ok "run: existing OUT refused" expect_status 2 run_fake run1 idle
ok "run: bad phase refused" expect_status 2 run_fake run2 boot
ok "run: whitespace-only phases refused" expect_status 2 run_fake run2 '   '
ok "run: duplicate phase refused" expect_status 2 run_fake duplicate 'idle idle'
mkdir "$W/lock"
ok "run: held lock refused" expect_status 3 run_fake run3 idle
ok "run: preserves held lock" test -d "$W/lock"
rmdir "$W/lock"
touch "$W/output-file"
ok "run: output creation failure" expect_status 2 run_fake output-file/child idle
ok "run: output failure releases lock" test ! -e "$W/lock"
for scenario in missing-ssh partial-pre invalid-boot missing-sentinel; do
  MOCK_SSH_SCENARIO=$scenario ok "run: $scenario exits 4" expect_status 4 run_fake "$scenario" idle
  ok "run: $scenario stops before snapshot" test ! -e "$W/$scenario/idle.txt"
done
for scenario in partial-snapshot failed-snapshot; do
  MOCK_SSH_SCENARIO=$scenario ok "run: $scenario exits 5" expect_status 5 run_fake "$scenario" idle
  ok "run: $scenario stops before summary" test ! -e "$W/$scenario/SUMMARY.txt"
done
ok "run: failure releases lock" test ! -e "$W/lock"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
