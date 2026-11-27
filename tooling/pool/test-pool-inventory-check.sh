#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of tooling/pool/pool-inventory-check.sh on made-up inventory folders: a pass, each refusal gate (R1-R4), VOID inventories.
set -u
D=$(cd "$(dirname "$0")/../.." && pwd)
C=$D/tooling/pool/pool-inventory-check.sh
export POOL_DATA_UUID=DA7A0000-0000-4000-8000-000000000001
T=$(mktemp -d /tmp/pool-inv-test.XXXXXX); trap 'rm -rf "$T"' EXIT
fails=0
check() { if [ "$1" = 0 ]; then echo "  ok    $2"; else echo "  FAIL  $2"; fails=$((fails + 1)); fi; }
mkdir -p "$T/a-os" "$T/a-sys"
echo "source_sha256=$(printf a | shasum -a 256 | cut -c1-64)" > "$T/a-os/archive.env"
echo "source_sha256=$(printf b | shasum -a 256 | cut -c1-64)" > "$T/a-sys/archive.env"
mk() {   # DIR: a passing inventory
    mkdir -p "$1"
    printf 'x\nUSED os y\nINVENTORY DONE 2026-11-23 20:00:00: x\n' > "$1/RESULT.txt"
    printf 'store=disk4\n%s  -\nseconds=60\n' "$(printf a | shasum -a 256 | cut -c1-64)" > "$1/window-sha-os.txt"
    printf 'store=disk5\n%s  -\nseconds=20\n' "$(printf b | shasum -a 256 | cut -c1-64)" > "$1/window-sha-systemos.txt"
    echo 'System Integrity Protection status: disabled.' > "$1/csr.txt"
    echo '   Container Free Space:      190.0 GB (190000000000 Bytes)' > "$1/info-data.txt"
    printf '    APFS Container Reference:     disk3\n        Volume UUID:               %s\n' "$POOL_DATA_UUID" > "$1/apfs-list.txt"
    echo x > "$1/adt-chosen.txt"; echo x > "$1/preboot.txt"
}
run() { bash "$C" "$1" "$T/a-os" "$T/a-sys" > "$T/out" 2>&1; }
echo '== pool-inventory-check.sh'
mk "$T/pass"; run "$T/pass"; [ $? = 0 ] && grep -q 'INVENTORY CHECK PASS' "$T/out" && [ "$(grep -c '^PASS R' "$T/out")" = 5 ]; check $? 'a complete inventory: PASS (R1 os, R1 systemos, R2, R3, R4)'
mk "$T/no-uuid"; POOL_DATA_UUID= run "$T/no-uuid"; [ $? = 2 ] && grep -q '^VOID R3' "$T/out"; check $? 'R3: missing expected Data UUID is VOID'
mk "$T/r1"; printf 'store=disk4\n%s  -\n' "$(printf z | shasum -a 256 | cut -c1-64)" > "$T/r1/window-sha-os.txt"; run "$T/r1"; [ $? = 1 ] && grep -q '^FAIL R1 os' "$T/out"; check $? 'R1: the OS window is not the archive source'
mk "$T/r2"; echo 'System Integrity Protection status: enabled.' > "$T/r2/csr.txt"; run "$T/r2"; [ $? = 1 ] && grep -q '^FAIL R2' "$T/out"; check $? 'R2: SIP enabled'
POOL_SIP_ENABLED_OK=1 bash "$C" "$T/r2" "$T/a-os" "$T/a-sys" > "$T/out" 2>&1; [ $? = 0 ]; check $? 'R2: SIP enabled with POOL_SIP_ENABLED_OK=1 (the extraction probe decides)'
mk "$T/r2b"; printf 'System Integrity Protection status: unknown (Custom Configuration).\n\tFilesystem Protections: enabled\n' > "$T/r2b/csr.txt"; run "$T/r2b"; [ $? = 1 ] && grep -q '^FAIL R2' "$T/out"; check $? 'R2: a custom configuration with filesystem protections on'
mk "$T/r3"; printf '    APFS Container Reference:     disk3\n        Volume UUID:               AAAAAAAA-0000-0000-0000-000000000000\n' > "$T/r3/apfs-list.txt"; run "$T/r3"; [ $? = 1 ] && grep -q '^FAIL R3' "$T/out"; check $? 'R3: the Data volume not in the APFS list'
mk "$T/r3b"; printf '    APFS Container Reference:     disk3\n        Volume UUID:               %s\n        FileVault:                 Yes (Unlocked)\n' "$POOL_DATA_UUID" > "$T/r3b/apfs-list.txt"; run "$T/r3b"; [ $? = 1 ] && grep -q '^FAIL R3' "$T/out"; check $? 'R3: FileVault on the Data container'
mk "$T/r4"; echo '   Container Free Space:      29.9 GB (29900000000 Bytes)' > "$T/r4/info-data.txt"; run "$T/r4"; [ $? = 1 ] && grep -q '^FAIL R4' "$T/out"; check $? 'R4: 29.9 GB free, below 30 GB'
mk "$T/v1"; rm "$T/v1/window-sha-systemos.txt"; run "$T/v1"; [ $? = 2 ] && grep -q 'INVENTORY CHECK VOID' "$T/out"; check $? 'VOID: a file missing'
mk "$T/v2"; printf 'x\nINVENTORY FATAL at step probe: ssh\n' > "$T/v2/RESULT.txt"; run "$T/v2"; [ $? = 2 ]; check $? 'VOID: a stopped inventory (FATAL, no DONE)'
grep -q '^RECORDED extraction speed' "$T/pass/pool-gates.txt" && grep -q '^RECORDED boot-manifest' "$T/pass/pool-gates.txt"; check $? 'the recorded items written to pool-gates.txt'
echo "$([ $fails = 0 ] && echo PASS || echo FAIL): $fails failure(s)"
exit $([ $fails = 0 ] && echo 0 || echo 1)
