#!/bin/bash
# FD variant B ("own303"): the Qualcomm DXEs of this tablet's own uefi_a (OS3.0.303, BOOT.MXF.2.5.1-00265), extracted and
# patched with piano-uefi's own tools (extract-qcom-dxe.py, patch-qcom-dxe.sh), instead of the set piano-uefi bundles
# (another firmware build: 13 of 104 drivers differ). The proprietary binaries stay outside every repository.
#   make-own-binaries.sh UEFI_A_IMG_ZST OUTDIR
# UFSDxe: piano-uefi's link-reset patch is written for fixed offsets of its own build and refuses ours (its own check); our
# UFSDxe stays unpatched, so UEFI may not bring the UFS link up. The RAM payload does not need UFS in UEFI.
set -euo pipefail
[ $# = 2 ] || { echo "usage: $0 UEFI_A_IMG_ZST OUTDIR" >&2; exit 2; }
[ -f "$1" ] || { echo "missing UEFI input image: $1" >&2; exit 1; }
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
PU=$R/piano-uefi-UniXNU-piano
DODGE=$R/Mu-Silicium-piano/Binaries/dodge
for tool in extract-qcom-dxe.py patch-qcom-dxe.sh; do
  [ -f "$PU/tools/$tool" ] || { echo "missing firmware tool: $tool" >&2; exit 1; }
done
[ -d "$DODGE" ] || { echo "missing Device-Binaries dodge tree" >&2; exit 1; }
OUT=$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve())' "$2")
INPUT=$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve())' "$1")
case "$INPUT" in
  "$OUT"|"$OUT"/*) echo "the output directory contains the input image" >&2; exit 1 ;;
esac
if [ -e "$OUT" ] && [ ! -f "$OUT/SOURCE.sha256" ]; then
  [ -d "$OUT" ] && [ -z "$(ls -A -- "$OUT")" ] || { echo "output must be empty or a previous firmware extraction" >&2; exit 1; }
fi
W=$(mktemp -d "${TMPDIR:-/tmp}/piano-own-binaries.XXXXXX")
trap 'rm -rf -- "$W"' EXIT
zstd -dq -o "$W/uefi_a.img" -- "$1"
python3 -m venv "$W/venv" >/dev/null
"$W/venv/bin/pip" -q install uefi_firmware >/dev/null
mkdir -p "$W/root/deps/Mu-Silicium/Binaries"
ln -s "$PU/tools" "$W/root/tools"
ln -s "$DODGE" "$W/root/deps/Mu-Silicium/Binaries/dodge"
"$W/venv/bin/python" "$PU/tools/extract-qcom-dxe.py" "$W/uefi_a.img" "$W/root/deps/Mu-Silicium/Binaries/piano"
status=0
(cd "$W/root" && PATH="$W/venv/bin:$PATH" bash tools/patch-qcom-dxe.sh) > "$W/patch.log" 2>&1 || status=$?
cat "$W/patch.log"
if [ "$status" -ne 0 ]; then
  if [ "$status" -ne 1 ] || ! grep -Fxq 'UFSDxe: unexpected code at the patch sites' "$W/patch.log"; then
    exit "$status"
  fi
  echo "patch-qcom-dxe.sh stopped (expected at UFSDxe for this build)"
fi
PAYLOAD=$W/root/deps/Mu-Silicium/Binaries/piano
shasum -a 256 "$W/uefi_a.img" | sed 's|  .*|  uefi_a.img|' > "$PAYLOAD/SOURCE.sha256"
mkdir -p -- "$(dirname -- "$OUT")"
rm -rf -- "$OUT"
cp -R -- "$PAYLOAD" "$OUT"
for inf in "$OUT"/QcomPkg/Drivers/*/*.inf; do
  if [ -f "$inf" ] && grep -qF 'patched.efi' "$inf"; then
    name=${inf##*/}
    printf '%s ' "${name%.inf}"
  fi
done
echo
