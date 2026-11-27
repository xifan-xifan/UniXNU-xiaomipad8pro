#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# pool-inventory-check.sh INVENTORY_DIR [ARCHIVE_OS ARCHIVE_SYSTEMOS]: T-20 plan A's preflight over the read-only inventory of the
# installed system (the inventory-YYYYmmdd-HHMMSS/ folder from INVENTORY-READS.sh). Host only:
# it reads files, never the tablet. Exit 0 when every refusal gate passes, 1 when one fails, 2 when the folder is not a complete
# inventory (VOID: never a pass). Prints one line per gate (PASS / FAIL / VOID / RECORDED) and writes them to INVENTORY_DIR/pool-gates.txt.
#   R1 the OS and SystemOS windows' raw sha256 (window-sha-os/-systemos) = the sources of the archives the install pushes; with
#      POOL_R1_CAPTURE=1 (T-20 v2) only recorded: R1a (archive = capture) and R1b (live content) in pool-install.sh decide
#   R2 SIP does not block the extraction: csrutil read; "enabled" is a FAIL only together with a Filesystem Protections line saying
#      enabled AND no SIP-less evidence; the extraction itself probes SF_RESTRICTED on the target before writing (pool-device.sh)
#   R3 addVolume usable: the expected Data volume (POOL_DATA_UUID) listed in diskutil's APFS list, no FileVault /
#      encryption or sealed-container line on it, the container not "Fusion"
#   R4 free space in the Data container at least POOL_MIN_FREE_GB (30) GB (diskutil info /System/Volumes/Data, Container Free Space)
#   recorded (never a refusal): the boot-manifest hash readers (adt-chosen, preboot), the volumes' used space, the windows' read time
#   (the extraction speed stays unknown until the session times it)
set -uo pipefail
INV=${1:?inventory folder}
ARCH_OS=${2:-$(cd "$(dirname "$0")/../.." && pwd)/out/pool/os-40d8282e}
ARCH_SYS=${3:-$(cd "$(dirname "$0")/../.." && pwd)/out/pool/systemos-a5346509}
MIN_GB=${POOL_MIN_FREE_GB:-30}
DATA_UUID=${POOL_DATA_UUID:-}
OUTF=$INV/pool-gates.txt
fails=0 voids=0
: > "$OUTF" 2>/dev/null || { echo "VOID: cannot write $OUTF"; exit 2; }
line() { echo "$*" | tee -a "$OUTF"; }
gate() {   # STATE TEXT
    case "$1" in FAIL) fails=$((fails + 1));; VOID) voids=$((voids + 1));; esac
    line "$1 $2"
}
NEED_FILES="RESULT.txt csr.txt info-data.txt apfs-list.txt"
[ "${POOL_R1_CAPTURE:-0}" = 1 ] || NEED_FILES="$NEED_FILES window-sha-os.txt window-sha-systemos.txt"
for f in $NEED_FILES; do
    [ -s "$INV/$f" ] || gate VOID "the inventory has no $f"
done
grep -q '^INVENTORY DONE ' "$INV/RESULT.txt" 2>/dev/null || gate VOID 'the inventory did not end (no INVENTORY DONE line in RESULT.txt)'
grep -q '^INVENTORY FATAL' "$INV/RESULT.txt" 2>/dev/null && gate VOID "the inventory stopped: $(grep -m1 '^INVENTORY FATAL' "$INV/RESULT.txt")"
# R1
for v in os systemos; do
    a=$ARCH_OS; [ "$v" = systemos ] && a=$ARCH_SYS
    want=$(sed -n 's/^source_sha256=//p' "$a/archive.env" 2>/dev/null)
    got=$(grep -a -E -o '^[0-9a-f]{64}' "$INV/window-sha-$v.txt" 2>/dev/null | head -1)
    secs=$(sed -n 's/^seconds=//p' "$INV/window-sha-$v.txt" 2>/dev/null | head -1)
    if [ "${POOL_R1_CAPTURE:-0}" = 1 ]; then   # T-20 v2: R1a against the quiesced capture (pool-install.sh), the live read only recorded
        line "RECORDED R1 $v: the inventory's live raw read ${got:-none} (a running system's window moves: PLAN-v2.md); R1a/R1b decide"
    elif [ -z "$want" ]; then gate VOID "R1 $v: no archive.env in $a"
    elif [ -z "$got" ]; then gate VOID "R1 $v: no sha256 in window-sha-$v.txt"
    elif [ "$got" = "$want" ]; then gate PASS "R1 $v window = the archive source ${want:0:16}... (read in ${secs:-?} s)"
    else gate FAIL "R1 $v window ${got:0:16}... is not the archive source ${want:0:16}...: make the $v archive from the pad's own window"
    fi
done
# R2
if [ -s "$INV/csr.txt" ]; then
    csr=$(grep -a -m1 'System Integrity Protection status' "$INV/csr.txt")
    fsp=$(grep -a -i -m1 'Filesystem Protections' "$INV/csr.txt")
    if [ -z "$csr" ]; then gate VOID 'R2 SIP: csrutil printed no status line'
    elif printf '%s %s' "$csr" "$fsp" | grep -qi 'Filesystem Protections: *enabled' ||
         { printf '%s' "$csr" | grep -qi 'status: *enabled\.' && [ -z "$fsp" ] && [ "${POOL_SIP_ENABLED_OK:-0}" != 1 ]; }; then
        gate FAIL "R2 SIP filesystem protection on ($csr${fsp:+; $fsp}): SF_RESTRICTED may be refused; POOL_SIP_ENABLED_OK=1 lets the extraction's own probe decide (the host proof set it on a non-boot volume with SIP on)"
    else
        gate PASS "R2 SIP: $csr${fsp:+; $fsp} (the extraction probes SF_RESTRICTED on the target before writing)"
    fi
fi
# R3
if [ -s "$INV/apfs-list.txt" ]; then
    cont=$(awk -v u="$DATA_UUID" '/APFS Container Reference:/ { c = $NF } /Volume UUID:/ && $NF == u { print c; exit }' "$INV/apfs-list.txt")
    if ! [[ "$DATA_UUID" =~ ^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$ ]]; then
        gate VOID 'R3 set POOL_DATA_UUID to the expected Data volume UUID'
    elif [ -z "$cont" ]; then gate FAIL "R3 the Data volume $DATA_UUID is not in diskutil's APFS list: addVolume has no container to use"
    elif ! [[ "$cont" =~ ^disk[0-9]+$ ]]; then gate FAIL "R3 invalid APFS container target: $cont"
    elif awk -v c="$cont" '/APFS Container Reference:/ { in_c = ($NF == c) } in_c && /(Fusion|FileVault:[[:space:]]*Yes|Sealed:[[:space:]]*Yes|Encrypted:[[:space:]]*Yes)/ { found = 1 } END { exit !found }' "$INV/apfs-list.txt"; then
        gate FAIL "R3 container $cont: Fusion, FileVault, encrypted or sealed lines: addVolume not taken as usable"
    else
        gate PASS "R3 the Data volume in APFS container $cont as diskutil lists it (addVolume target)"
    fi
fi
# R4
if [ -s "$INV/info-data.txt" ]; then
    free=$(sed -nE 's/.*Container Free Space:.*\(([0-9]+) Bytes\).*/\1/p' "$INV/info-data.txt" | head -1)
    if [ -z "$free" ]; then gate VOID 'R4 no "Container Free Space: ... (N Bytes)" line in info-data.txt'
    elif [ "$free" -ge $((MIN_GB * 1000000000)) ]; then gate PASS "R4 Data container free $((free / 1000000000)) GB (minimum $MIN_GB GB)"
    else gate FAIL "R4 Data container free $((free / 1000000000)) GB, below $MIN_GB GB"
    fi
fi
# recorded
line "RECORDED boot-manifest hash readers: $(grep -a -c -E 'manifest|hash' "$INV/adt-chosen.txt" 2>/dev/null || echo 0) adt-chosen lines name a manifest/hash; preboot listing $(wc -l < "$INV/preboot.txt" 2>/dev/null | tr -d ' ' || echo 0) lines (the reader of the changed SHA-384 stays unknown: C12)"
line "RECORDED used space: $(grep -a '^USED' "$INV/RESULT.txt" 2>/dev/null | tr '\n' ' ')"
line "RECORDED extraction speed: unknown until the session's extract step times it (host: OS 400 s, SystemOS 111 s on the mini)"
if [ "$voids" != 0 ]; then line "INVENTORY CHECK VOID: $voids void, $fails failed"; exit 2; fi
if [ "$fails" != 0 ]; then line "INVENTORY CHECK FAIL: $fails failed"; exit 1; fi
line 'INVENTORY CHECK PASS'
exit 0
