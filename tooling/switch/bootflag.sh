#!/system/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# piano's Android/macOS boot flag from a shell: Android (Magisk module, root) through the SDAM nvmem file, and, once the kext
# sysctl exists, macOS through `sysctl machdep.piano_bootflag`. The codec is firmware/selector/bootflag.h's, byte for byte (the
# host test compares both over every value and request).
#   bootflag.sh show | once-android | once-macos | once-clear | default-android | default-macos | reset
# Backends: PIANO_BOOTFLAG_NVMEM=FILE (Android; default: the nvmem device whose of_node is sdam@7100), or
# PIANO_BOOTFLAG_BACKEND=sysctl (macOS, root): `sysctl machdep.piano_bootflag` reads the byte, writing a request number (1..6, the
# kext applies and checks it: package/include/PianoBootFlagSdam.h) needs the boot argument piano-bootflag-write=1.
# Offset 0x49 (73). A byte that is not ours is never written (exit 3).
OFF=73

ours() {  # $1 decimal byte
  v=$1
  [ "$v" -eq 0 ] && return 0
  [ $((v & 248)) -eq 160 ] || return 1
  [ $((v & 3)) -ne 3 ]
}

apply() {  # $1 old, $2 request -> prints the new byte; 1 = refuse
  ours "$1" || return 1
  b=0; [ "$1" -ne 0 ] && b=$(($1 & 7))
  case "$2" in
    once-android) b=$(((b & 4) | 1)) ;;
    once-macos) b=$(((b & 4) | 2)) ;;
    once-clear) b=$((b & 4)) ;;
    default-android) b=$((b | 4)) ;;
    default-macos) b=$((b & 3)) ;;
    reset) b=0 ;;
    *) return 1 ;;
  esac
  [ "$b" -eq 0 ] && echo 0 || echo $((160 | b))
}

describe() {
  v=$1
  if ! ours "$v"; then echo "not ours ($(printf '0x%02x' "$v")): untouched, the selector boots macOS"; return; fi
  d=macOS; [ "$v" -ne 0 ] && [ $((v & 4)) -ne 0 ] && d=Android
  o=none; [ "$v" -ne 0 ] && { [ $((v & 1)) -ne 0 ] && o=Android; [ $((v & 2)) -ne 0 ] && o=macOS; }
  echo "flag $(printf '0x%02x' "$v"): default $d, next boot once: $o"
}

nvmem() {
  if [ -n "$PIANO_BOOTFLAG_NVMEM" ]; then echo "$PIANO_BOOTFLAG_NVMEM"; return; fi
  for d in /sys/bus/nvmem/devices/*; do
    case "$(readlink -f "$d/of_node" 2>/dev/null)" in *sdam@7100) echo "$d/nvmem"; return ;; esac
  done
  return 1
}

rd() {
  if [ "$PIANO_BOOTFLAG_BACKEND" = sysctl ]; then x=$(sysctl -n machdep.piano_bootflag) || return 1; [ "$x" -ge 0 ] 2>/dev/null || return 1; echo "$x"; return; fi
  f=$(nvmem) || { echo "no sdam@7100 nvmem device" >&2; return 1; }
  x=$(dd if="$f" bs=1 skip=$OFF count=1 2>/dev/null | od -An -tu1 | tr -d ' \n')
  [ -n "$x" ] || return 1
  echo "$x"
}

reqnum() {
  case "$1" in once-android) echo 1;; once-macos) echo 2;; once-clear) echo 3;; default-android) echo 4;; default-macos) echo 5;;
    reset) echo 6;; *) return 1;; esac
}

wrt() {  # $1 decimal (nvmem backend)
  f=$(nvmem) || return 1
  printf "\\$(printf '%03o' "$1")" | dd of="$f" bs=1 seek=$OFF count=1 conv=notrunc 2>/dev/null
}

main() {
  [ "${1:-}" = __apply ] && { apply "$2" "$3"; return; }
  [ "${1:-}" = __ours ] && { ours "$2"; return; }
  req=${1:-show}
  old=$(rd) || { echo "bootflag: cannot read the flag" >&2; return 2; }
  if [ "$req" = show ]; then describe "$old"; return 0; fi
  new=$(apply "$old" "$req") || { ours "$old" && echo "bootflag: unknown request $req" >&2 || echo "bootflag: $(describe "$old"); refused" >&2; return 3; }
  if [ "$PIANO_BOOTFLAG_BACKEND" = sysctl ]; then   # the kext applies the request under its lock and reads it back
    sysctl -w machdep.piano_bootflag="$(reqnum "$req")" >/dev/null 2>&1 || { echo "bootflag: the kext refused (piano-bootflag-write=1? root?)" >&2; return 4; }
    now=$(rd)
    [ "$now" = "$new" ] || { echo "bootflag: read back $now, want $new" >&2; return 5; }
    describe "$now"; return 0
  fi
  if [ "$new" -ne "$old" ]; then
    wrt "$new" || { echo "bootflag: write failed" >&2; return 4; }
    now=$(rd)
    [ "$now" = "$new" ] || { echo "bootflag: read back $now, want $new" >&2; return 5; }
  fi
  describe "$new"
}

main "$@"
