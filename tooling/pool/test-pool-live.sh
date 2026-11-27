#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host test of T-20 v2 R1b (PLAN-v2.md): pool-manifest --paths + pool-live-check.py. No root, no tablet: a made-up OS layer and SystemOS
# layer, their manifests (as make-volume-archive.sh writes them), and a merged tree standing in for the live union `/` (SystemOS wins
# for a name both have, directories keep the top layer's attributes), with Data content behind a firmlink source and another file system's
# content behind a mount point. EQUAL with the tolerances counted; DIFFERENT for a changed byte, an added file, a removed file, a changed
# xattr, a changed mode, a root mount that is not read-only; VOID for a truncated live run and for a list made from other inputs.
#
#   test-pool-live.sh [--keep]
set -u
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d /tmp/pool-live-test.XXXXXX)
[ "${1:-}" = --keep ] && echo "kept: $T" || trap 'chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"' EXIT
TOOL=$T/pool-manifest
bash "$D/tooling/pool/build-pool-manifest.sh" "$TOOL" > /dev/null || exit 1
CHECK=$D/tooling/pool/pool-live-check.py
fails=0
check() { if [ "$1" = 0 ]; then echo "  ok    $2"; else echo "  FAIL  $2"; fails=$((fails + 1)); fi; }
L=$T/os U=$T/systemos
mkdir -p "$L/System/Library/Frameworks/A.framework" "$L/usr/bin" "$L/usr/lib" "$L/Library/Old" "$L/sbin" "$L/System/Cryptexes"
echo a > "$L/System/Library/Frameworks/A.framework/A"; echo tool > "$L/usr/bin/tool"; echo lower-both > "$L/System/Library/both.txt"
echo h > "$L/usr/lib/h1"; ln "$L/usr/lib/h1" "$L/usr/lib/h2"; ln -s ../../Cryptexes/OS/B "$L/System/Library/Frameworks/B"
echo old > "$L/Library/Old/x"; echo s > "$L/sbin/launchd"; chmod 755 "$L/sbin/launchd"; mkdir -p "$L/dev"
mkdir -p "$L/System/Library/Shadowed"; echo below > "$L/System/Library/Shadowed/inner"
mkdir -p "$U/System/Library" "$U/System/Cryptexes/OS" "$U/Library" "$U/dev" "$U/.fseventsd" "$U/usr/lib"
echo upper-both > "$U/System/Library/both.txt"; echo crypt > "$U/System/Cryptexes/OS/B"; echo f > "$U/.fseventsd/0001"
echo shadow-file > "$U/System/Library/Shadowed"      # a file in SystemOS where OS has a directory: OS's inner is not visible
echo u > "$U/usr/lib/u1"; ln "$U/usr/lib/u1" "$U/usr/lib/u2"; xattr -w com.example.p 1 "$U/System/Cryptexes/OS/B"
touch -t 202601010101 "$U/System/Library" "$U/System" "$U"; touch -t 202501010101 "$L/System" "$L"
"$TOOL" --exclude .fseventsd "$L" > "$T/os.manifest"; "$TOOL" --exclude .fseventsd "$U" > "$T/sys.manifest"
printf '/dev/disk4s1 on / (apfs, local, read-only, journaled)\n/dev/disk5s1 on / (apfs, local, read-only, union)\ndevfs on /dev (devfs, local, nobrowse)\n/dev/disk3s1 on /System/Volumes/Data (apfs, local, journaled)\n' > "$T/mounts.txt"
merge() {   # the live union: OS, then SystemOS over it; directories with the top layer's attributes; Data behind /Library, devfs at /dev
    local M=$T/live
    chmod -R u+w "$M" 2>/dev/null; rm -rf "$M"; mkdir "$M"
    ditto "$L" "$M"; rm -rf "$M/System/Library/Shadowed"; ditto "$U" "$M"; rm -rf "$M/.fseventsd"
    rm -rf "$M/Library"; mkdir -p "$M/Library/Data-only"; echo data > "$M/Library/Data-only/f"     # the firmlink target (Data)
    echo console > "$M/dev/console"                                                                # devfs content
    python3 - "$L" "$U" "$M" <<'PYT'
import os, sys
L, U, M = sys.argv[1:]
def times(src, rel):   # mtime (and a directory's mode) of SRC/REL onto M/REL, links not followed
    st = os.lstat(os.path.join(src, rel))
    os.utime(os.path.join(M, rel), ns=(st.st_atime_ns, st.st_mtime_ns), follow_symlinks=False)
    if os.path.isdir(os.path.join(src, rel)) and not os.path.islink(os.path.join(src, rel)):
        os.chmod(os.path.join(M, rel), st.st_mode & 0o7777)
def walk(root):
    out = []
    for d, dirs, files in os.walk(root, topdown=False):
        for n in files + dirs:
            out.append(os.path.relpath(os.path.join(d, n), root))
    return out + ['.']
for rel in walk(L):
    if not os.path.lexists(os.path.join(U, rel)) and os.path.lexists(os.path.join(M, rel)):
        times(L, rel)
for rel in walk(U):
    if os.path.lexists(os.path.join(M, rel)):
        times(U, rel)
PYT
}
lst() { python3 "$CHECK" list --systemos "$T/sys.manifest" --os "$T/os.manifest" --mounts "${1:-$T/mounts.txt}" --out "$T/list.txt"; }
live() { "$TOOL" --paths "$T/list.txt" "$T/live" > "$T/live.txt" 2> "$T/live.err"; }
cmp_() { python3 "$CHECK" compare --systemos "$T/sys.manifest" --os "$T/os.manifest" --mounts "${1:-$T/mounts.txt}" --list "$T/list.txt" --live "$T/live.txt" --json "$T/verdict.json" > "$T/compare.txt" 2>&1; }
echo '== pool-manifest --paths + pool-live-check.py on a host-made union'
lst > "$T/list.out"; check $? "list: $(cat "$T/list.out")"
! grep -q $'^o\tSystem/Library/Shadowed/inner$' "$T/list.txt" && ! grep -q $'^o\tLibrary/Old/x$' "$T/list.txt" && ! grep -q $'^o\tSystem/Library/both.txt$' "$T/list.txt" && grep -q $'^s\tSystem/Library/both.txt$' "$T/list.txt"
check $? 'list: the shadowed OS name, the OS entry below a SystemOS file and the firmlink source stay out'
merge; live; cmp_; rc=$?
[ $rc = 0 ] && grep -q '^EQUAL' "$T/compare.txt"; check $? "EQUAL: $(head -2 "$T/compare.txt" | tr "\n" " ")"
grep -q '"T2"' "$T/verdict.json" && grep -q '"T3"' "$T/verdict.json" && grep -q '"T4"' "$T/verdict.json" && grep -q '"T6"' "$T/verdict.json"
check $? 'the tolerances counted (T2 firmlink, T3 mount point, T4 hidden OS entries, T6 union listing)'
neg() {   # WHAT EXPECT_RC PATTERN COMMAND...: on a fresh merged tree
    local what=$1 want=$2 pat=$3; shift 3
    merge; "$@"; live; cmp_; local rc=$?
    [ $rc = "$want" ] && grep -Eq "$pat" "$T/compare.txt"; check $? "$what -> rc $rc: $(head -1 "$T/compare.txt" | cut -c1-150)"
}
neg 'a changed byte in a SystemOS file' 1 '"data"' sh -c "printf X | dd of='$T/live/System/Library/both.txt' bs=1 seek=2 conv=notrunc 2>/dev/null"
neg 'a changed byte in an OS file' 1 '"data"' sh -c "printf X | dd of='$T/live/usr/bin/tool' bs=1 seek=1 conv=notrunc 2>/dev/null"
neg 'an added file in a SystemOS directory' 1 '"added"' sh -c "echo n > '$T/live/System/Cryptexes/OS/new'"
neg 'a removed OS file' 1 '"missing"' rm -f "$T/live/usr/bin/tool"
neg 'a changed xattr' 1 '"xattrs"' xattr -w com.example.p 2 "$T/live/System/Cryptexes/OS/B"
neg 'a changed mode' 1 '"mode"' chmod 700 "$T/live/sbin/launchd"
neg 'a broken hard link' 1 'hardlink' sh -c "cp '$T/live/usr/lib/u2' '$T/live/usr/lib/u2.t' && mv -f '$T/live/usr/lib/u2.t' '$T/live/usr/lib/u2'"
sed 's/read-only, union/union/' "$T/mounts.txt" > "$T/mounts-rw.txt"
lst "$T/mounts-rw.txt" > /dev/null; merge; live; cmp_ "$T/mounts-rw.txt"; rc=$?
[ $rc = 1 ] && grep -q 'root-not-read-only' "$T/compare.txt"; check $? 'the old SystemOS mounted read-write: DIFFERENT'
lst > /dev/null; merge; live; head -3 "$T/live.txt" > "$T/live.cut"; mv "$T/live.cut" "$T/live.txt"; cmp_; rc=$?
[ $rc = 2 ] && grep -q '^VOID' "$T/compare.txt"; check $? 'a truncated live output: VOID'
live; printf 'o\tsbin\n' >> "$T/list.txt"; cmp_; rc=$?
[ $rc = 2 ] && grep -q 'not the one' "$T/compare.txt"; check $? 'a list made from other inputs: VOID'
mv "$T/os.manifest" "$T/os.good"
tail -1 "$T/os.good" > "$T/os.manifest"
lst > "$T/compare.txt" 2>&1; rc=$?
[ $rc = 2 ] && grep -q '^VOID' "$T/compare.txt"; check $? 'a source footer with missing records: VOID'
mv "$T/os.good" "$T/os.manifest"
lst > /dev/null; live
{ head -1 "$T/live.txt"; cat "$T/live.txt"; } > "$T/live.duplicate"
mv "$T/live.duplicate" "$T/live.txt"
cmp_; rc=$?
[ $rc = 2 ] && grep -q '^VOID' "$T/compare.txt"; check $? 'a repeated live record: VOID'
echo "$([ $fails = 0 ] && echo PASS || echo FAIL): $fails failure(s)"
exit $([ $fails = 0 ] && echo 0 || echo 1)
