#!/bin/bash
# Rebuild the Mesa work tree that tooling/metal/UPSTREAM pins as UniXNU-turnip's .cache/mesa-work: the tarball of the pinned Mesa commit,
# that repository's patches/mesa at the pin, and a tree's patches/mesa-macos. The published (alphabetical) order is not the applied order,
# because turnip-builtin-shader-pack rewrites the line turnip-darwin-kmd adds and lands its meson.options hunk at the end of the file, and
# turnip-darwin-deadline-queue patches the file turnip-darwin-virtgpu creates. So order by what unblocks the rest: take the first remaining
# patch that checks clean; where several fit, apply each in turn (patch -R undoes the trial), count how many of the others then fit, and
# keep the one that unblocks the most - ties by published name. Nothing is applied that did not pass a dry run first, and the order and any
# fuzzed hunk are printed, so the result is auditable rather than merely working.
#
#   tooling/metal/build-mesa-work.sh [--expect-left NAME,NAME] [--must-apply PATCH]... WORK MESA_TARBALL PATCHES [PATCHES2]...
#
# WORK receives .cache/mesa-work; the tarball's single top-level directory becomes it. Not every patch published in patches/mesa is in that
# pinned work tree - tooling/metal/UPSTREAM counts 21 - so name the ones that never apply with --expect-left, and anything else that stays
# is an error. Fetch the inputs where the network allows: the Mac build host resolves neither raw.githubusercontent nor a blobless clone of
# the monorepo, so the files arrive by scp from elsewhere. --must-apply adds a patch that is not in the pinned tree at all - the Mesa side
# of a bridge patch that needs it (turnip-render-mode-for-attachment-memory.patch for NativeMetal 0020) - applied after the pinned set, and
# a failure there is the build's failure rather than a leftover.
set -uo pipefail
expect=; extra=()
while [ "${1:-}" = --expect-left ]; do expect=${2?patch names}; shift 2; done
while [ "${1:-}" = --must-apply ]; do extra+=("${2?patch path}"); shift 2; done
[ $# -ge 3 ] || { echo "usage: $0 [--expect-left NAME,NAME] WORK MESA_TARBALL PATCHES_DIR [PATCHES_DIR...]" >&2; exit 2; }
W=$1/.cache/mesa-work; T=$2; shift 2
dirs=(); for d in "$@"; do dirs+=("$(cd "$d" && pwd)"); done
if [ ! -f "$W/meson.build" ]; then
  mkdir -p "$W" && tar -xf "$T" -C "$W" --strip-components 1 || exit 1
fi
fits() { patch -s -p1 -d "$W" --dry-run < "$1" > /dev/null 2>&1; }
apply_extra() {   # a patch outside the pinned set, after it: no dry-run games, it either applies or the build stops
  for p in ${extra[@]+"${extra[@]}"}; do
    patch -s -p1 -d "$W" --dry-run < "$p" > /tmp/mesa-extra-check.$$ 2>&1 || { echo "must-apply $(basename "$p") does not apply:"; head -6 /tmp/mesa-extra-check.$$; rm -f /tmp/mesa-extra-check.$$; return 1; }
    patch -s -p1 -d "$W" < "$p" || { echo "must-apply $(basename "$p") failed on the second pass"; return 1; }
    echo "must-apply: $(basename "$p")"
  done
  return 0
}
left=(); for d in "${dirs[@]}"; do for p in "$d"/*.patch; do [ -e "$p" ] && left+=("$p"); done; done
order=(); fuzz=()
report() {   # the order used, and what is left, on every exit path
  echo "applied=${#order[@]} fuzz=${#fuzz[@]}"
  [ "${1:-}" = left ] && { echo "left:"; printf '  %s\n' "${left[@]##*/}"; }
  printf '  %s\n' "${order[@]##*/}"
  [ "${#fuzz[@]}" -gt 0 ] && printf '  fuzz: %s\n' "${fuzz[@]}"
}
while [ "${#left[@]}" -gt 0 ]; do
  chosen=""; score=-1
  for c in "${left[@]}"; do
    fits "$c" || continue
    if [ "${#left[@]}" -eq 1 ]; then chosen="$c"; break; fi
    patch -s -p1 -d "$W" < "$c" > /dev/null 2>&1 || continue
    s=0; for o in "${left[@]}"; do [ "$o" = "$c" ] && continue; fits "$o" && s=$((s+1)); done
    patch -s -R -p1 -d "$W" < "$c" > /dev/null 2>&1
    if [ "$s" -gt "$score" ]; then score=$s; chosen="$c"; fi
    [ "$s" -gt 0 ] && break
  done
  if [ -z "$chosen" ]; then
    echo "STUCK with ${#left[@]} left"; report left
    for p in "${left[@]}"; do
      case ",$expect," in *",$(basename "$p"),"*) ;; *) echo "unexpected leftover: $(basename "$p")"; echo MESA-WORK-INCOMPLETE; exit 1 ;; esac
    done
    echo "leftovers as expected: $(for p in "${left[@]}"; do printf "%s " "$(basename "$p")"; done)"
    apply_extra || { echo MESA-WORK-INCOMPLETE; exit 1; }
    echo "mesa_work=$W files=$(find "$W" -type f | wc -l | tr -d ' ')"; echo MESA-WORK-DONE; exit 0
  fi
  out=$(patch -p1 -d "$W" < "$chosen" 2>&1) || { echo "FAILED applying $(basename "$chosen")"; echo "$out" | head -4; exit 1; }
  case "$out" in *"with fuzz"*|*Fuzzed*) fuzz+=("$(basename "$chosen")");; esac
  order+=("$chosen")
  rest=(); for p in "${left[@]}"; do [ "$p" = "$chosen" ] || rest+=("$p"); done; left=(${rest[@]+"${rest[@]}"})
done
report; apply_extra || { echo MESA-WORK-INCOMPLETE; exit 1; }
echo "mesa_work=$W files=$(find "$W" -type f | wc -l | tr -d ' ')"; echo MESA-WORK-DONE
