#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of tooling/native/ram-m0: the tablet script runs on this Mac (same tools, read-only), the summarizer parses it, and
# run-ram-m0.sh drives a fake TSSH (local bash) with a private lock and OUT; refusals (bad phase, existing OUT, held lock, no ssh).
set -uo pipefail
[ "$(uname)" = Darwin ] || { echo "SKIP: needs macOS (vm_stat, memory_pressure)"; exit 0; }
T=$(cd "$(dirname "$0")" && pwd); W=$(mktemp -d /tmp/ram-m0-test.XXXXXX); trap 'rm -rf "$W"' EXIT
pass=0 fail=0
ok() { if eval "$2"; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
bash "$T/ram-m0-tab.sh" idle > "$W/idle.txt" 2>&1
ok "tab: M0_DONE" "grep -q '^M0_DONE$' '$W/idle.txt'"
ok "tab: hw.memsize read" "grep -q '^hw.memsize=[0-9]' '$W/idle.txt'"
ok "tab: vm_stat section" "grep -q '^Pages free:' '$W/idle.txt'"
ok "tab: never applies pressure" "! grep -nE 'memory_pressure +-' '$T/ram-m0-tab.sh'"
python3 -I "$T/summarize-ram-m0.py" "$W/idle.txt" > "$W/sum.txt"; rc=$?
ok "summary rc 0" "[ $rc = 0 ]"
ok "summary line" "grep -q '^phase=idle memsize_mib=[0-9]' '$W/sum.txt'"
ok "summary hint" "grep -q '^M0 HINT' '$W/sum.txt'"
printf 'phase=apps\nhw.memsize=13956546560\nhw.pagesize=16384\nvm.swapusage=total = 1024.00M  used = 300.00M  free = 724.00M\nkern.memorystatus_vm_pressure_level=2\nPages free: 100.\nPages speculative: 10.\nPages occupied by compressor: 90000.\nM0_DONE\n' > "$W/apps.txt"
python3 -I "$T/summarize-ram-m0.py" "$W/apps.txt" > "$W/sum2.txt"
ok "fixture: 13310 MiB window, pressure hint" "grep -q 'memsize_mib=13310' '$W/sum2.txt' && grep -q 'RAM pressure seen (apps)' '$W/sum2.txt'"
printf 'phase=idle\n' > "$W/bad.txt"
python3 -I "$T/summarize-ram-m0.py" "$W/bad.txt" > /dev/null; ok "incomplete snapshot rc 1" "[ $? = 1 ]"
# fake ssh: a local bash that answers the pre read with an installed-boot line
cat > "$W/fssh" <<'S'
#!/bin/bash
case "$1" in *SSH_OK*) echo SSH_OK; echo "mode=single-gate fake";; *) eval "$1";; esac
S
chmod +x "$W/fssh"
TSSH=$W/fssh LOCK=$W/lock OUT=$W/run1 bash "$T/run-ram-m0.sh" t1 idle > "$W/run1.log" 2>&1; rc=$?
ok "run: rc 0 with a fake ssh" "[ $rc = 0 ] && [ -s '$W/run1/idle.txt' ] && grep -q '^phase=idle' '$W/run1/SUMMARY.txt'"
ok "run: lock released" "[ ! -e '$W/lock' ]"
TSSH=$W/fssh LOCK=$W/lock OUT=$W/run1 bash "$T/run-ram-m0.sh" t1 idle > /dev/null 2>&1; ok "run: existing OUT refused" "[ $? = 2 ]"
TSSH=$W/fssh LOCK=$W/lock OUT=$W/run2 bash "$T/run-ram-m0.sh" t2 boot > /dev/null 2>&1; ok "run: bad phase refused" "[ $? = 2 ]"
mkdir "$W/lock"; TSSH=$W/fssh LOCK=$W/lock OUT=$W/run3 bash "$T/run-ram-m0.sh" t3 idle > /dev/null 2>&1; ok "run: held lock refused" "[ $? = 3 ] && [ -d '$W/lock' ]"; rmdir "$W/lock"
printf '#!/bin/bash\nexit 255\n' > "$W/nossh"; chmod +x "$W/nossh"
TSSH=$W/nossh LOCK=$W/lock OUT=$W/run4 bash "$T/run-ram-m0.sh" t4 idle > /dev/null 2>&1; ok "run: no ssh rc 4" "[ $? = 4 ]"
echo "$pass passed, $fail failed"; [ $fail = 0 ]
