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
for f in kernel-k27-native.macho kernel-k273-native.macho piano-kexts.tar.gz SHA256SUMS BUILD-INFO.txt; do
  [ -f "$A/$f" ] || { echo "no $A/$f (build.yml's kernel-kexts artifact)" >&2; exit 1; }
done
( cd "$A" && shasum -a 256 -c SHA256SUMS )
mv "$A/SHA256SUMS" "$A/BUILD-SHA256SUMS"
# Only our binaries: no Apple payload (no fileset collection, no IPSW member) may be among the assets.
for f in "$A"/*.macho; do
  python3 - "$f" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(32)
magic, cpu, sub, filetype = struct.unpack('<IiiI', d[:16])
sys.exit(f'{sys.argv[1]}: not a kernel Mach-O (filetype {filetype})' if magic != 0xfeedfacf or filetype != 2 else 0)
PY
done
# The recipes: the pins and patches the binaries were built from, and the scripts that build them.
tar -czf "$A/piano-recipes.tar.gz" unixnu.lock feeds.toml board.toml patches configs tooling/ci tooling/k27/fetch-k27-tree.sh \
  tooling/k27/build-native-image.sh tooling/k27/kext tooling/k273 tooling/kc scripts/fetch-pins.py docs/ci.md
# The install skill (skills/install-piano-macos): the user side builds the root image and the kernel collection with it.
if [ -d skills/install-piano-macos ]; then
  tar -czf "$A/install-piano-macos-skill.tar.gz" skills/install-piano-macos
elif [ "${ALLOW_NO_SKILL:-0}" != 1 ]; then
  echo "skills/install-piano-macos is not in $TAG: a release without the install skill is refused (ALLOW_NO_SKILL=1 to override)" >&2
  exit 1
fi
[ -f "$F/fd.sha256" ] && cp "$F/fd.sha256" "$A/FD-SHA256.txt"
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
  [ -f "$A/install-piano-macos-skill.tar.gz" ] && echo "| \`install-piano-macos-skill.tar.gz\` | the install skill: builds the root image and the kernel collection from your own IPSW and installs |"
  [ -f "$A/FD-SHA256.txt" ] && echo "| \`FD-SHA256.txt\` | the SHA-256 the FD built from this tag must have (the FD itself is built on your side) |"
  echo "| \`SHA256SUMS\` | every asset's SHA-256 |"
  echo
  echo "No Apple software is in these assets: the kernelcache and the Kernel Debug Kit headers the build read were deleted on the runner."
  echo
  sed 's/^/    /' "$A/BUILD-INFO.txt"
} > "$A/RELEASE-NOTES.md"
( cd "$A" && rm -f SHA256SUMS && find . -maxdepth 1 -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs shasum -a 256 > SHA256SUMS.new \
  && mv SHA256SUMS.new SHA256SUMS && cat SHA256SUMS )
