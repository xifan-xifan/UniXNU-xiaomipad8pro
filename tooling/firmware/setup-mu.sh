#!/bin/bash
# Make the UEFI build checkouts of unixnu.lock under UNIXNU_RESEARCH (default ~/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research),
# as piano-uefi's tools/setup-deps.sh does, plus this repository's deltas. Shallow and sparse; safe to rerun.
#   Mu-Silicium-piano/                 Mu-Silicium e51cd877 + Common/Mu, Mu_OEM_Sample, Silicium-ACPI, openssl (no Mu_Basecore:
#                                      UniXNU's edk2 feed is the same commit)
#   Mu-Silicium-piano/Binaries/        Device-Binaries a1501e0c, sparse: dodge/MdeModulePkg + the dodge QcomPkg references
#   piano-uefi-UniXNU-piano/           worktree of piano-uefi 31545f9 (branch unixnu) + firmware/patches/*.patch
#   UniXNU/                            + firmware/patches/unixnu/*.patch (the UniXNUPkg the FD builds)
#   links: Platforms/Xiaomi/pianoPkg, Resources/Configs/piano.toml, Binaries/piano, Resources/MuPatches/piano-mu-basecore.patch
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
pin() {
  python3 - "$D/unixnu.lock" "$1" "$2" <<'PY'
import sys
try:
    import tomllib
except ModuleNotFoundError:
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        from pip._vendor import tomli as tomllib
with open(sys.argv[1], encoding='utf-8') as lock:
    print(tomllib.loads(lock.read())['pin'][sys.argv[2]][sys.argv[3]])
PY
}
refit_patches() {
  # The tree carries exactly these deltas, in name order. Two deltas in one file mean a patch does not reverse-check
  # while a later one is applied (0001 and 0004 both edit Config.c), so the patched state is judged as a set: when
  # every patch reverse-checks the deltas are in and nothing moves; otherwise the set is taken back in reverse name
  # order and re-applied forwards, which is the sequence a fresh checkout gets. Rerun-safe, and it fails on a patch
  # that genuinely does not apply.
  local tree=$1 p
  shift
  local -a fwd=("$@") rev=()
  local i
  for (( i=${#fwd[@]}-1; i>=0; i-- )); do rev+=("${fwd[$i]}"); done
  for p in "${fwd[@]}"; do
    git -C "$tree" apply --reverse --check "$p" 2>/dev/null && continue
    for p in "${rev[@]}"; do
      git -C "$tree" apply --reverse --check "$p" 2>/dev/null && git -C "$tree" apply --reverse "$p"
    done
    break
  done
  for p in "${fwd[@]}"; do
    git -C "$tree" apply --reverse --check "$p" 2>/dev/null || git -C "$tree" apply "$p"
  done
}
M=$R/Mu-Silicium-piano
if [ ! -d "$M/.git" ]; then
  url=$(pin Mu-Silicium git)
  commit=$(pin Mu-Silicium commit)
  git init -q "$M"
  git -C "$M" remote add origin "$url"
  git -C "$M" fetch -q --depth 1 origin "$commit"
  git -C "$M" checkout -q FETCH_HEAD
fi
git -C "$M" submodule update -q --init --depth 1 -- Common/Mu Common/Mu_OEM_Sample Silicium-ACPI Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl
B=$M/Binaries
if [ ! -d "$B/.git" ]; then
  url=$(pin Device-Binaries git)
  commit=$(pin Device-Binaries commit)
  mkdir -p "$B"
  git -C "$B" init -q
  git -C "$B" remote add origin "$url"
  git -C "$B" sparse-checkout set --no-cone /dodge/MdeModulePkg/ /dodge/QcomPkg/Drivers/SPMIDxe/ /dodge/QcomPkg/Drivers/ButtonsDxe/ \
    /dodge/QcomPkg/Drivers/UsbConfigDxe/ /dodge/QcomPkg/Drivers/UsbMsdDxe/ /dodge/QcomPkg/Drivers/PmicDxe/ /dodge/QcomPkg/Drivers/QcomChargerDxe/
  git -C "$B" fetch -q --depth 1 --filter=blob:none origin "$commit"
  git -C "$B" checkout -q FETCH_HEAD
fi
P=$R/piano-uefi-UniXNU-piano
if [ ! -d "$P" ]; then
  commit=$(pin piano-uefi commit)
  if [ ! -d "$R/piano-uefi/.git" ]; then
    url=$(pin piano-uefi git)
    git clone -q -b unixnu "$url" "$R/piano-uefi"
  fi
  git -C "$R/piano-uefi" worktree add -q --detach "$P" "$commit"
  for p in "$D"/firmware/patches/*.patch; do git -C "$P" apply "$p"; done
fi
refit_patches "$P" "$D"/firmware/patches/*.patch
# Mu-Silicium deltas (SEC keeps the boot loader's x0 = DT in the entry-argument HOB UniXNURamDiskDxe reads).
refit_patches "$M" "$D"/firmware/patches/mu-silicium/*.patch
# UniXNU core deltas (UniXNULib: a boot image's RAM payload wins over a FAT ESP on UFS; docs/persistent-boot.md).
refit_patches "$R/UniXNU" "$D"/firmware/patches/unixnu/*.patch
mkdir -p "$M/Resources/MuPatches"
ln -sfn "$P/uefi/pianoPkg" "$M/Platforms/Xiaomi/pianoPkg"
ln -sfn "$P/uefi/piano.toml" "$M/Resources/Configs/piano.toml"
ln -sfn "$P/uefi/patches/mu-basecore.patch" "$M/Resources/MuPatches/piano-mu-basecore.patch"
[ -e "$B/piano" ] || ln -sfn "$P/uefi/Binaries/piano" "$B/piano"
[ -d "$R/uefi-sheng/BootShim" ] || { echo "need uefi-sheng $(pin uefi-sheng commit) for UniXNU's BootShim.S at $R/uefi-sheng" >&2; exit 1; }
echo "UEFI checkouts ready under $R"
