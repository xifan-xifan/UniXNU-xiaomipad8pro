#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# One entry for an outside build of the piano 27.3 images (skills/install-piano-macos/SKILL.md): every Apple input fetched from
# Apple on this Mac and checked against tooling/repro/pins.json, then the existing build tools in order. Nothing here touches a
# tablet; nothing from the IPSW is ever written outside out/.
#
#   tooling/repro/fetch-and-build.sh [--dry-run | --check] [STEP ...]
#     STEP: host inputs systemos root kernel fd image (default: all, in this order)
#     --dry-run  print every command a run would execute (read-only probes still run: check-host.sh, fetch-ipsw.py --dry-run)
#     --check    run nothing that writes: each step's outputs checked against the pins (no network)
#
#   host      tooling/repro/check-host.sh: Apple silicon, macOS, Xcode, tools (FAIL stops; GAP lines name local-only inputs)
#   inputs    tooling/repro/fetch-ipsw.py: the K114c members, AEA keys from Apple, decrypted images, the KC base (pins at each step);
#             tooling/repro/boot-dtb.py: out/inputs/piano-boot.dtb from the Ubuntu v1.1.0 maintenance image (PIANO_V110 or MAINT_IMG)
#   systemos  tooling/k273/make-systemos.sh (sudo) -> out/inputs/k273/root/systemos.img, then its file-level manifest against the pin
#             (tooling/repro/volume-manifest.py --compare, sudo): the image bytes are not reproducible, the tree is
#   root      tooling/k273/make-root-image.sh -> root.img, root.json, trust bundle, seed; tooling/repro/verify-root.py (pins)
#             needs out/inputs/kernel.macho.trustcache and a Data seed (K273_SEED or out/inputs/scratch-data.img): Known gaps G2, G3
#   kernel    tooling/k273/prepare-k27-tree.sh: the k27 kernel relinked at the 27.3 KC's addresses (PIANO_K27_TREE: Known gap G1)
#   fd        tooling/firmware/build-fd.sh: the UEFI FD from the unixnu.lock checkouts (UNIXNU_RESEARCH)
#   image     tooling/k273/build-k273-image.sh -> out/ram (KC, ADT, RAM images), verified by its own checks
#             K273_ROOT_PART (default linux_data: a second unit's first root write keeps Ubuntu's root) and PIANO_* as documented there
# Environment: SUDO (default sudo; e.g. 'sudo -A' with SUDO_ASKPASS; make-systemos.sh itself calls sudo: run in a Terminal where sudo
# can ask), REMOVE_AEA=1 (delete the AEA archives once decrypted), QUICK=1 (--check: the root image's structure only).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
MODE=run
case ${1:-} in --dry-run) MODE=dry; shift ;; --check) MODE=check; shift ;; esac
if [ "$#" -eq 0 ]; then set -- host inputs systemos root kernel fd image; fi
I=${K273_INPUTS:-$D/out/inputs/k273}
M=$I/26C5035r
R=$I/root
read -r -a SUDO_CMD <<< "${SUDO:-sudo}"
PY=(python3 -I)
die() { echo "FAIL: $*" >&2; exit 1; }
say() { echo "== $*"; }
run() {
  if [ "$MODE" = dry ]; then printf 'would run:'; printf ' %q' "$@"; printf '\n'
  else echo "run: $*"; "$@"; fi
}
maint_img() {
  local c
  for c in "${MAINT_IMG:-}" "${PIANO_V110:+$PIANO_V110/release/boot/boot-ram-maintenance.img}"; do
    [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}
for s; do
  case $s in
    host)
      say "host"
      if ! bash tooling/repro/check-host.sh; then
        [ "$MODE" = dry ] || die "check-host.sh reported FAIL"
        echo "(dry run: a real run stops here)"
      fi
      ;;
    inputs)
      say "inputs: the pinned IPSW members from Apple's CDN"
      input_args=(--inputs "$I")
      if [ "${REMOVE_AEA:-0}" = 1 ]; then input_args+=(--remove-aea); fi
      case $MODE in
        dry) "${PY[@]}" tooling/repro/fetch-ipsw.py --dry-run "${input_args[@]}" ;;
        check) "${PY[@]}" tooling/repro/fetch-ipsw.py --check --inputs "$I"; "${PY[@]}" tooling/repro/boot-dtb.py --check ;;
        run) "${PY[@]}" tooling/repro/fetch-ipsw.py "${input_args[@]}"
             if [ -f out/inputs/piano-boot.dtb ] && "${PY[@]}" tooling/repro/boot-dtb.py --check > /dev/null; then echo "ok    out/inputs/piano-boot.dtb"
             else MI=$(maint_img) || die "no maintenance image: set PIANO_V110 (the unpacked Ubuntu v1.1.0 release) or MAINT_IMG"
                  "${PY[@]}" tooling/repro/boot-dtb.py "$MI"; fi ;;
      esac
      ;;
    systemos)
      say "systemos: the SystemOS window, then its file-level manifest against the pin"
      if [ "$MODE" = check ]; then
        [ -f "$R/systemos.img" ] && [ -f "$R/systemos.img.json" ] || die "no $R/systemos.img(.json)"
        echo "the manifest check needs root: ${SUDO_CMD[*]} ${PY[*]} tooling/repro/volume-manifest.py --image $R/systemos.img --compare tooling/repro/pins.json"
        "${SUDO_CMD[@]}" "${PY[@]}" tooling/repro/volume-manifest.py --image "$R/systemos.img" --compare tooling/repro/pins.json
      else
        if [ -f "$R/systemos.img" ] && [ -f "$R/systemos.img.json" ]; then echo "kept: $R/systemos.img (remove it and its .json to rebuild)"
        else run mkdir -p "$R"
             run bash tooling/k273/make-systemos.sh "$M/141-22233-021.dmg" "$M/141-22145-021.dmg" "$M/141-22106-021.dmg" "$R/systemos.img"; fi
        run "${SUDO_CMD[@]}" "${PY[@]}" tooling/repro/volume-manifest.py --image "$R/systemos.img" --json "$R/systemos.manifest.json" --compare tooling/repro/pins.json
      fi
      ;;
    root)
      say "root: the root image (OS + SystemOS windows), trust bundle and seed"
      root_args=("$R")
      if [ "$MODE" = check ] && [ "${QUICK:-0}" = 1 ]; then root_args+=(--quick); fi
      if [ -f "$R/systemos.manifest.json" ]; then root_args+=(--manifest-json "$R/systemos.manifest.json"); fi
      if [ "$MODE" = check ]; then "${PY[@]}" tooling/repro/verify-root.py "${root_args[@]}"
      else
        [ -f out/inputs/kernel.macho.trustcache ] || [ "$MODE" = dry ] || die "no out/inputs/kernel.macho.trustcache (Known gap G2)"
        [ -n "${K273_SEED:-}" ] || [ -f out/inputs/scratch-data.img ] || [ "$MODE" = dry ] || die "no Data seed: K273_SEED or out/inputs/scratch-data.img (Known gap G3)"
        run bash tooling/k273/make-root-image.sh "$R"
        run "${PY[@]}" tooling/repro/verify-root.py "${root_args[@]}"
      fi
      ;;
    kernel)
      say "kernel: the k27 kernel relinked at the 27.3 collection's addresses"
      if [ "$MODE" = check ]; then
        T=${K273_TREE:-$D/out/k273/k27-tree}
        ls "$T"/out/k27/kernel-k273-*-native.macho > /dev/null 2>&1 || die "no relinked kernel in $T/out/k27"
        for k in "$T"/out/k27/kernel-k273-*-native.macho; do echo "kernel $(basename "$k") $(shasum -a 256 "$k" | cut -c1-16)..."; done
      else
        # shellcheck disable=SC1091 # The checkout helper is sourced only for the real build.
        [ "$MODE" = dry ] || (. tooling/k27/k27-tree.sh) || die "PIANO_K27_TREE is not set (Known gap G1)"
        run bash tooling/k273/prepare-k27-tree.sh
      fi
      ;;
    fd)
      say "fd: the UEFI firmware volume"
      if [ "$MODE" = check ]; then
        F=out/fd/piano-uefi-A-bundled.fd
        [ -f "$F" ] && [ -f "$F.ram-payload-first" ] \
          && [ "$(cat "$F.ram-payload-first")" = "$(shasum -a 256 "$F" | cut -c1-64)" ] || die "no stamped $F"
        echo "ok    $F (ram-payload-first stamp matches)"
      else run bash tooling/firmware/build-fd.sh; fi
      ;;
    image)
      say "image: KC, ADT and the RAM images in out/ram"
      if [ "$MODE" = check ]; then
        [ -f out/ram/build-A-bundled.json ] || die "no out/ram/build-A-bundled.json"
        "${PY[@]}" tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
      else
        run env K273_ROOT=k273 K273_ROOT_PART="${K273_ROOT_PART:-linux_data}" PIANO_GPU="${PIANO_GPU:-2}" \
          PIANO_DATA_PERSIST=0 PIANO_RESET_AFTER="${PIANO_RESET_AFTER:-900}" bash tooling/k273/build-k273-image.sh
      fi
      ;;
    *) die "unknown step $s (host inputs systemos root kernel fd image)" ;;
  esac
done
if [ "$MODE" = dry ]; then echo "dry run: nothing was fetched, built or written"; else echo "PASS: $*"; fi
