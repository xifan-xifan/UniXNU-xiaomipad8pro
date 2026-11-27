#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# pool-capture-check.sh WINDOWS_DIR: the host gates of a T-20 v2 capture (PLAN-v2.md step 2; maint-session.py window-capture wrote
# WINDOWS_DIR/CAPTURE.json, os.img, systemos.img). No root, nothing written but WINDOWS_DIR/CHECK.txt; the images are attached read-only
# and never mounted. Per window: the file's size = CAPTURE.json's; its sha256 read without the page cache = CAPTURE.json's (= the
# tablet's own); block 0 starts an APFS container (NXSB) whose nx_uuid = the one the tablet read; attached read-only, the container
# holds exactly the expected volume (OS 84581E99..., SystemOS 07AD53B3...; POOL_EXPECT_OS_UUID / POOL_EXPECT_SYSTEMOS_UUID for a test);
# the sha256 again after the detach (the attach changed nothing). Exit 0 PASS, 1 FAIL.
set -uo pipefail
DIR=${1:?the windows folder of a capture}
OUT=$DIR/CHECK.txt
fails=0
: > "$OUT" || exit 1
say() { echo "$*" | tee -a "$OUT"; }
gate() { if [ "$1" = 0 ]; then say "PASS $2"; else say "FAIL $2"; fails=$((fails + 1)); fi; }
[ -f "$DIR/CAPTURE.json" ] || { say "FAIL no $DIR/CAPTURE.json"; exit 1; }
nocache_sha() {
    python3 -c '
import fcntl, hashlib, os, sys
h = hashlib.sha256(); fd = os.open(sys.argv[1], os.O_RDONLY); fcntl.fcntl(fd, fcntl.F_NOCACHE, 1)
while (b := os.read(fd, 8 << 20)): h.update(b)
print(h.hexdigest())' "$1"
}
field() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["windows"][sys.argv[2]][sys.argv[3]])' "$DIR/CAPTURE.json" "$1" "$2"; }
for v in os systemos; do
    f=$DIR/$v.img
    case $v in os) want_uuid=${POOL_EXPECT_OS_UUID:-84581E99-85BE-4957-AC53-2947E887B3E4};;
               systemos) want_uuid=${POOL_EXPECT_SYSTEMOS_UUID:-07AD53B3-7565-4004-B574-F45FAC3753FB};; esac
    [ -f "$f" ] || { gate 1 "$v: no $f"; continue; }
    size=$(field "$v" size); sha=$(field "$v" sha256); nx=$(field "$v" nx_uuid)
    [ "$(stat -f %z "$f")" = "$size" ]; gate $? "$v: $size bytes"
    got=$(nocache_sha "$f")
    [ "$got" = "$sha" ]; gate $? "$v: sha256 without the page cache $got = CAPTURE.json's"
    hdr=$(python3 -c 'import sys; d = open(sys.argv[1], "rb").read(128); print(d[32:36].decode(errors="replace"), d[72:88].hex())' "$f")
    [ "$hdr" = "NXSB $nx" ]; gate $? "$v: APFS container superblock, nx_uuid $nx ($hdr)"
    att=$(diskutil image attach --readOnly --noMount "$f" 2>&1)
    attach_rc=$?
    phys=$(echo "$att" | grep -oE '^/dev/disk[0-9]+' | head -1)
    vols=$(echo "$att" | awk '/Apple_APFS_Volume/ {print $1}')
    uuids=''
    for d in $vols; do uuids="$uuids $(diskutil info -plist "$d" | plutil -extract VolumeUUID raw -o - - 2>/dev/null)"; done
    [ -n "$phys" ] && { hdiutil detach "$phys" > /dev/null 2>&1 || hdiutil detach -force "$phys" > /dev/null 2>&1; }
    detach_rc=$?
    [ "$attach_rc" = 0 ] && [ "$detach_rc" = 0 ] && [ "$uuids" = " $want_uuid" ]
    gate $? "$v: attached read-only and detached, its one volume is $want_uuid (found:${uuids:- none})"
    again=$(shasum -a 256 "$f" | awk '{print $1}')
    [ "$again" = "$sha" ]; gate $? "$v: sha256 after the detach unchanged"
done
say "$([ $fails = 0 ] && echo 'CAPTURE CHECK PASS' || echo "CAPTURE CHECK FAIL: $fails failed")"
exit $([ $fails = 0 ] && echo 0 || echo 1)
