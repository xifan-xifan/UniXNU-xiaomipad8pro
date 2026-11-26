#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The k27 tree from public sources: upstream Droid-VM/macOS-DroidVM at unixnu.lock's [pin."k27"] commit, this repository's
# patches/k27-tree on top (git am), the result checked against the pin's git tree id. No build output (out/) comes with it: the
# kernel, the KC base and the generated headers are made on the machine that builds (docs/ci.md, docs/bringup-debugging.md §4).
#
#   tooling/k27/fetch-k27-tree.sh DEST          -> DEST (a git checkout, branch piano); prints PIANO_K27_TREE=DEST
#
# A DEST that already holds the pinned result is left as it is; any other existing DEST is refused (never overwritten).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
DEST=${1:?usage: fetch-k27-tree.sh DEST}
pin() { python3 -c "import tomllib,sys;print(tomllib.load(open('$D/unixnu.lock','rb'))['pin']['k27'][sys.argv[1]])" "$1"; }
URL=$(pin git); BASE=$(pin commit); TREE=$(pin tree); PATCHES=$D/$(pin patches)
if [ -d "$DEST/.git" ]; then
  have=$(git -C "$DEST" rev-parse 'HEAD^{tree}' 2>/dev/null || true)
  if [ "$have" = "$TREE" ] && [ -z "$(git -C "$DEST" status --porcelain --untracked-files=no)" ]; then
    echo "k27 tree at $DEST is the pinned one (tree ${TREE:0:12})"; echo "PIANO_K27_TREE=$DEST"; exit 0
  fi
  echo "$DEST exists and is not the pinned k27 tree (tree ${have:-none}, want $TREE): not touched" >&2; exit 1
fi
[ ! -e "$DEST" ] || { echo "$DEST exists and is not a git checkout: not touched" >&2; exit 1; }
git init -q "$DEST"
git -C "$DEST" remote add origin "$URL"
git -C "$DEST" fetch -q --depth 1 origin "$BASE" || git -C "$DEST" fetch -q origin
git -C "$DEST" checkout -q -b piano "$BASE"
# git am needs an identity for the committer only; the patches keep their authors
git -C "$DEST" -c user.name=k27-tree -c user.email=k27-tree@fetch.invalid -c core.whitespace=nowarn \
  am -q --whitespace=nowarn --committer-date-is-author-date "$PATCHES"/0*.patch
got=$(git -C "$DEST" rev-parse 'HEAD^{tree}')
if [ "$got" != "$TREE" ]; then
  echo "k27 tree mismatch: patches/k27-tree on $BASE gives tree $got, unixnu.lock pins $TREE" >&2; exit 1
fi
echo "k27 tree: $URL ${BASE:0:12} + $(ls "$PATCHES"/0*.patch | wc -l | tr -d ' ') patches = tree ${TREE:0:12}"
echo "PIANO_K27_TREE=$DEST"
