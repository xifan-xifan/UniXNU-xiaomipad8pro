# SPDX-License-Identifier: BSD-2-Clause-Patent
# Source this to read the merged line, with a fallback for kernels before k27 0076.
piano_bootargs() {
  local args
  if args=$(LC_ALL=C sysctl -n kern.piano_bootargs 2>&1); then
    printf '%s\n' "$args"
  elif [ "$args" = "sysctl: unknown oid 'kern.piano_bootargs'" ]; then
    sysctl -n kern.bootargs 2>/dev/null
  else
    return 1
  fi
}

# Select the last exact key. Split on PE's separators and keep patterns in the line literal.
piano_bootarg() (
  local args word value=
  args=$(piano_bootargs) || return 1
  IFS=' 	'
  set -f
  for word in $args; do
    case "$word" in
      "$1") value=1 ;;
      "$1="*) value=${word#*=} ;;
    esac
  done
  printf '%s\n' "$value"
)
