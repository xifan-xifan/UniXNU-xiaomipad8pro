#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host tests of bootflag.sh: its codec equals bootflag.h's over all 256 values x 6 requests (a C table), the nvmem backend
# round trip on a fake SDAM file (only byte 0x49 changes), refusals, and the sysctl backend with a fake sysctl.
set -u
H=$(cd "$(dirname "$0")" && pwd); S=$H/bootflag.sh; R=$(cd "$H/../.." && pwd)
SH=${TEST_SH:-/bin/sh}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
unset PIANO_BOOTFLAG_BACKEND
fails=0; n=0
ok() { n=$((n+1)); if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }

cat > "$T/t.c" <<'C'
#include <stdio.h>
#include "bootflag.h"
int main(void){ const char *nm[]={"","once-android","once-macos","once-clear","default-android","default-macos","reset"};
 for(unsigned v=0;v<256;v++) for(int r=1;r<=6;r++){unsigned o; if(piano_bootflag_apply(v,r,&o)) printf("%u %s R\n",v,nm[r]); else printf("%u %s %u\n",v,nm[r],o);}
 return 0;}
C
cc -std=c99 -I"$R/firmware/selector" "$T/t.c" -o "$T/t" && "$T/t" > "$T/c.txt"
for v in $(seq 0 255); do for r in once-android once-macos once-clear default-android default-macos reset; do
  o=$($SH "$S" __apply "$v" "$r") && echo "$v $r $o" || echo "$v $r R"; done; done > "$T/sh.txt"
cmp -s "$T/c.txt" "$T/sh.txt"; ok $? "sh codec = bootflag.h codec over 256 x 6 ($(wc -l < "$T/c.txt" | tr -d ' ') rows)"
[ "$($SH "$S" __apply 0 bogus; echo "rc$?")" = rc1 ]; ok $? "unknown request refused"

# nvmem backend: a 256-byte fake SDAM with other bytes set
python3 -c "import sys;b=bytearray(range(256));b[0x49]=0;open(sys.argv[1],'wb').write(b)" "$T/sdam"
cp "$T/sdam" "$T/sdam.orig"
export PIANO_BOOTFLAG_NVMEM=$T/sdam
out=$($SH "$S" show); ok $([ "$out" = "flag 0x00: default macOS, next boot once: none" ]; echo $?) "show baseline: $out"
cmp -s "$T/sdam" "$T/sdam.orig"; ok $? "show leaves nvmem untouched"
$SH "$S" once-android > /dev/null; ok $? "once-android rc 0"
b=$(od -An -tu1 -j73 -N1 "$T/sdam" | tr -d ' '); ok $([ "$b" = 161 ]; echo $?) "byte 0x49 = 0xA1 ($b)"
python3 -c "import sys;a=open(sys.argv[1],'rb').read();b=open(sys.argv[2],'rb').read();sys.exit(0 if len(a)==len(b)==256 and all(x==y for i,(x,y) in enumerate(zip(a,b)) if i!=0x49) else 1)" "$T/sdam" "$T/sdam.orig"
ok $? "no other SDAM byte changed, size kept"
$SH "$S" default-android > /dev/null; $SH "$S" once-macos > /dev/null
b=$(od -An -tu1 -j73 -N1 "$T/sdam" | tr -d ' '); ok $([ "$b" = 166 ]; echo $?) "default-android + once-macos = 0xA6 ($b)"
out=$($SH "$S" show); ok $([ "$out" = "flag 0xa6: default Android, next boot once: macOS" ]; echo $?) "show: $out"
$SH "$S" reset > /dev/null; b=$(od -An -tu1 -j73 -N1 "$T/sdam" | tr -d ' '); ok $([ "$b" = 0 ]; echo $?) "reset back to 0x00"
python3 -c "import sys;b=bytearray(open(sys.argv[1],'rb').read());b[0x49]=0x5c;open(sys.argv[1],'wb').write(b)" "$T/sdam"
$SH "$S" once-android > /dev/null 2>&1; rc=$?; b=$(od -An -tu1 -j73 -N1 "$T/sdam" | tr -d ' ')
ok $([ $rc = 3 ] && [ "$b" = 92 ]; echo $?) "foreign byte 0x5c refused (rc $rc), untouched"
chmod 444 "$T/sdam"; python3 -c "import sys,os;os.chmod(sys.argv[1],0o644);b=bytearray(open(sys.argv[1],'rb').read());b[0x49]=0;open(sys.argv[1],'wb').write(b);os.chmod(sys.argv[1],0o444)" "$T/sdam"
if [ "$(id -u)" = 0 ]; then n=$((n+1)); echo "SKIP read-only nvmem: root writes a 0444 file, run this test unprivileged"; else
$SH "$S" once-android > /dev/null 2>&1; rc=$?; ok $([ $rc = 4 ] || [ $rc = 5 ]; echo $?) "read-only nvmem: write failure reported (rc $rc)"
fi
PIANO_BOOTFLAG_NVMEM=$T/missing $SH "$S" show > /dev/null 2>&1; ok $([ $? = 2 ]; echo $?) "missing nvmem: rc 2"
head -c 73 "$T/sdam" > "$T/truncated"
PIANO_BOOTFLAG_NVMEM=$T/truncated $SH "$S" reset > /dev/null 2>&1
ok $([ $? = 2 ] && [ "$(wc -c < "$T/truncated")" -eq 73 ]; echo $?) "truncated nvmem refused without extending the file"
unset PIANO_BOOTFLAG_NVMEM

# sysctl backend (macOS, once the kext sysctl exists): a fake sysctl keeps the value in a file
mkdir -p "$T/bin"; echo 0 > "$T/val"
cat > "$T/bin/sysctl" <<F
#!/bin/sh
case "\$1" in
  -n) cat "$T/val" ;;
  -w) r=\${2#*=}; echo "\$r" >> "$T/reqs"; [ -f "$T/deny" ] && exit 1
      case \$r in 1) echo 161;; 3) echo 0;; esac > "$T/val" ;;
esac
F
chmod +x "$T/bin/sysctl"
for value in '' 0161 416 9999999999999999999999999999 '161 0' '2+0' $'16\n1'; do
  printf '%s\n' "$value" > "$T/val"
  PATH=$T/bin:$PATH PIANO_BOOTFLAG_BACKEND=sysctl $SH "$S" reset > /dev/null 2>&1
  ok $([ $? = 2 ] && [ ! -e "$T/reqs" ]; echo $?) "malformed or oversized sysctl value refused before writing ($(wc -c < "$T/val") bytes)"
  rm -f "$T/reqs"
done
for value in 163 167 168 255; do
  printf '%s\n' "$value" > "$T/val"
  PATH=$T/bin:$PATH PIANO_BOOTFLAG_BACKEND=sysctl $SH "$S" reset > /dev/null 2>&1
  ok $([ $? = 3 ] && [ ! -e "$T/reqs" ] && [ "$(cat "$T/val")" = "$value" ]; echo $?) "foreign or invalid sysctl byte $value refused before writing"
done
echo 0 > "$T/val"
out=$(PATH=$T/bin:$PATH PIANO_BOOTFLAG_BACKEND=sysctl $SH "$S" show)
ok $([ "$out" = "flag 0x00: default macOS, next boot once: none" ] && [ ! -e "$T/reqs" ]; echo $?) "sysctl show only reads"
PATH=$T/bin:$PATH PIANO_BOOTFLAG_BACKEND=sysctl $SH "$S" once-android > /dev/null; ok $([ "$(cat "$T/val")" = 161 ] && [ "$(tail -1 "$T/reqs")" = 1 ]; echo $?) "sysctl backend: once-android sends request 1, reads 0xa1"
PATH=$T/bin:$PATH PIANO_BOOTFLAG_BACKEND=sysctl $SH "$S" once-clear > /dev/null; ok $([ "$(cat "$T/val")" = 0 ] && [ "$(tail -1 "$T/reqs")" = 3 ]; echo $?) "sysctl backend: once-clear sends request 3"
touch "$T/deny"; PATH=$T/bin:$PATH PIANO_BOOTFLAG_BACKEND=sysctl $SH "$S" once-android > /dev/null 2>&1; ok $([ $? = 4 ]; echo $?) "sysctl backend: a kext refusal is rc 4"
echo "$([ $fails = 0 ] && echo PASS || echo FAIL): bootflag.sh tests, $n checks, $fails failures"
[ $fails = 0 ]
