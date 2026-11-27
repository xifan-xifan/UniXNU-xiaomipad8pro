#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The release's assets around the built binaries (release.yml): the recipes, the install skill, the FD's hash, notes and one
# SHA-256 manifest over everything. Run from the tagged checkout.
#
#   tooling/ci/release-assets.sh ASSETS FD_HASH_DIR
#   ASSETS holds build.yml's kernel-kexts artifact (kernel-k27-native.macho, kernel-k273-native.macho, piano-kexts.tar.gz,
#   SHA256SUMS, BUILD-INFO.txt). ALLOW_NO_SKILL=1 lets a release go without skills/install-piano-macos (it is refused otherwise).
set -euo pipefail
A=${1:?ASSETS}; F=${2:?FD_HASH_DIR}
TAG=${TAG:-$(git describe --tags --exact-match 2>/dev/null || git rev-parse --short HEAD)}
die() { echo "$*" >&2; exit 1; }
binaries=(kernel-k27-native.macho kernel-k273-native.macho piano-kexts.tar.gz)
for f in "${binaries[@]}" BUILD-INFO.txt; do
  [ -f "$A/$f" ] || { echo "no $A/$f (build.yml's kernel-kexts artifact)" >&2; exit 1; }
done
# The upload step takes the whole assets directory. Admit the build inputs and the outputs of an earlier run.
shopt -s dotglob
for f in "$A"/*; do
  [ -f "$f" ] && [ ! -L "$f" ] || die "not a regular release asset: ${f##*/}"
  case ${f##*/} in
    kernel-k27-native.macho|kernel-k273-native.macho|piano-kexts.tar.gz|BUILD-INFO.txt|SHA256SUMS|BUILD-SHA256SUMS|\
      piano-recipes.tar.gz|install-piano-macos-skill.tar.gz|FD-SHA256.txt|RELEASE-NOTES.md) ;;
    *) die "unexpected release asset: ${f##*/}" ;;
  esac
done
shopt -u dotglob
build_sums=SHA256SUMS
[ ! -f "$A/BUILD-SHA256SUMS" ] || build_sums=BUILD-SHA256SUMS
[ -f "$A/$build_sums" ] || die "no $build_sums (build.yml's kernel-kexts artifact)"
skill=skills/install-piano-macos
has_skill=0
if [ "$(git cat-file -t "HEAD:$skill" 2>/dev/null)" = tree ]; then
  has_skill=1
elif [ "${ALLOW_NO_SKILL:-0}" != 1 ]; then
  die "skills/install-piano-macos is not in $TAG: a release without the install skill is refused (ALLOW_NO_SKILL=1 to override)"
fi

# Require a checksum for each binary before asking shasum to read any of the named files.
python3 - "$A" "$build_sums" "${binaries[@]}" <<'PY'
import re
import struct
import sys
from pathlib import Path

assets = Path(sys.argv[1])
manifest, *binaries = sys.argv[2:]
entries = []
for line in (assets / manifest).read_bytes().splitlines():
    match = re.fullmatch(rb'[0-9a-fA-F]{64} [ *](.+)', line)
    if not match:
        sys.exit(f'{manifest}: invalid build checksum line')
    entries.append(match[1])
if sorted(entries) != sorted(name.encode() for name in binaries):
    sys.exit(f'{manifest}: expected one checksum for each kernel and the kext archive')

for name in binaries[:2]:
    with (assets / name).open('rb') as f:
        header = f.read(32)
    if len(header) != 32:
        sys.exit(f'{name}: truncated Mach-O header')
    magic, _, _, filetype = struct.unpack_from('<IiiI', header)
    if magic != 0xfeedfacf or filetype != 2:
        sys.exit(f'{name}: not a kernel Mach-O (filetype {filetype})')
PY
( cd -- "$A" && shasum -a 256 -c "$build_sums" )

stage=$(mktemp -d "$A/.release-assets.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
cp -- "$A/$build_sums" "$stage/BUILD-SHA256SUMS"
# Take recipes and the skill from the release commit so local files and edits stay out of the archives.
git archive --format=tar.gz -o "$stage/piano-recipes.tar.gz" HEAD -- unixnu.lock feeds.toml board.toml patches configs tooling/ci tooling/k27/fetch-k27-tree.sh \
  tooling/k27/build-native-image.sh tooling/k27/kext tooling/k273 tooling/kc scripts/fetch-pins.py docs/ci.md
# The install skill (skills/install-piano-macos): the user side builds the root image and the kernel collection with it.
if [ "$has_skill" = 1 ]; then
  git archive --format=tar.gz -o "$stage/install-piano-macos-skill.tar.gz" HEAD -- "$skill"
fi
[ -f "$F/fd.sha256" ] && cp -- "$F/fd.sha256" "$stage/FD-SHA256.txt"
{
  echo "# UniXNU-piano $TAG"
  echo
  echo "Built by GitHub Actions on a hosted macos-26 runner from the tagged commit; qualified on the tablet before tagging."
  echo
  echo "| Asset | What |"
  echo "| --- | --- |"
  echo "| \`kernel-k27-native.macho\` | the k27 native kernel (xnu-12377.121.6 + patches/k27-tree's series + patches/k27), built from source |"
  echo "| \`kernel-k273-native.macho\` | the same kernel relinked at the macOS 27.3 collection's addresses (+ patches/k273, patches/k27 past 0066) |"
  echo "| \`piano-kexts.tar.gz\` | our native kexts' objects and descriptors (the kernel collection is linked on your side) |"
  echo "| \`piano-recipes.tar.gz\` | the pins, patches and build scripts these were built from |"
  [ -f "$stage/install-piano-macos-skill.tar.gz" ] && echo "| \`install-piano-macos-skill.tar.gz\` | the install skill: builds the root image and the kernel collection from your own IPSW and installs |"
  [ -f "$stage/FD-SHA256.txt" ] && echo "| \`FD-SHA256.txt\` | the SHA-256 the FD built from this tag must have (the FD itself is built on your side) |"
  echo "| \`SHA256SUMS\` | every asset's SHA-256 |"
  echo
  echo "No Apple software is in these assets: the kernelcache and the Kernel Debug Kit headers the build read were deleted on the runner."
  echo
  sed 's/^/    /' "$A/BUILD-INFO.txt"
} > "$stage/RELEASE-NOTES.md"
outputs=(BUILD-SHA256SUMS BUILD-INFO.txt RELEASE-NOTES.md "${binaries[@]}" piano-recipes.tar.gz)
for f in FD-SHA256.txt install-piano-macos-skill.tar.gz; do
  if [ -f "$stage/$f" ]; then outputs+=("$f"); else rm -f -- "$A/$f"; fi
done
mv -- "$stage"/* "$A/"
( cd -- "$A" && shasum -a 256 "${outputs[@]}" ) > "$stage/SHA256SUMS"
mv -- "$stage/SHA256SUMS" "$A/SHA256SUMS"
cat -- "$A/SHA256SUMS"
