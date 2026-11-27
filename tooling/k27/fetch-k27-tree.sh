#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The k27 tree from public sources: upstream Droid-VM/macOS-DroidVM at unixnu.lock's [pin."k27"] commit, this repository's
# patches/k27-tree on top (git am), the result checked against the pin's git tree id. No build output (out/) comes with it: the
# kernel, the KC base and the generated headers are made on the machine that builds (docs/ci.md, docs/bringup-debugging.md §4).
#
#   tooling/k27/fetch-k27-tree.sh DEST [LOCK]   -> DEST (a git checkout, branch piano); prints PIANO_K27_TREE=DEST
#
# A DEST that already holds the pinned result is left as it is; any other existing DEST is refused (never overwritten).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
DEST=${1:?usage: fetch-k27-tree.sh DEST [LOCK]}
LOCK=${2:-$D/unixnu.lock}
pins=$(python3 - "$D/scripts" "$LOCK" <<'PY'
import sys

sys.path.insert(0, sys.argv[1])
from importlib import import_module

try:
    pin = import_module('fetch-pins').read_pins(sys.argv[2])['k27']
    for key in ('git', 'commit', 'tree', 'patches'):
        print(pin[key])
except (KeyError, OSError, ValueError) as error:
    raise SystemExit(f'{sys.argv[2]}: invalid k27 pin ({error})') from None
PY
)
{
  IFS= read -r URL
  IFS= read -r BASE
  IFS= read -r TREE
  IFS= read -r PATCH_DIR
} <<< "$pins"
if [ -e "$DEST/.git" ]; then
  have=$(git -C "$DEST" rev-parse 'HEAD^{tree}' 2>/dev/null || true)
  if [ "$have" = "$TREE" ] && status=$(git -C "$DEST" status --porcelain --untracked-files=no) && [ -z "$status" ]; then
    echo "k27 tree at $DEST is the pinned one (tree ${TREE:0:12})"; echo "PIANO_K27_TREE=$DEST"; exit 0
  fi
  echo "$DEST exists and is not the pinned k27 tree (tree ${have:-none}, want $TREE): not touched" >&2; exit 1
fi
[ ! -e "$DEST" ] && [ ! -L "$DEST" ] || { echo "$DEST exists and is not a git checkout: not touched" >&2; exit 1; }
patches=("$D/$PATCH_DIR"/0*.patch)
[ -f "${patches[0]}" ] || { echo "$PATCH_DIR contains no numbered patches" >&2; exit 1; }

# Verify in a sibling directory so a fetch or patch failure leaves DEST available for retry.
PARENT=$(dirname -- "$DEST")
mkdir -p -- "$PARENT"
STAGE=$(mktemp -d "$PARENT/.k27-tree.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT
WORK=$STAGE/tree
git init -q -- "$WORK"
git -C "$WORK" remote add origin "$URL"
git -C "$WORK" fetch -q --depth 1 origin "$BASE" || git -C "$WORK" fetch -q origin
git -C "$WORK" checkout -q -b piano "$BASE"
# git am needs an identity for the committer only; the patches keep their authors
git -C "$WORK" -c user.name=k27-tree -c user.email=k27-tree@fetch.invalid -c core.whitespace=nowarn \
  am -q --whitespace=nowarn --committer-date-is-author-date "${patches[@]}"
got=$(git -C "$WORK" rev-parse 'HEAD^{tree}')
if [ "$got" != "$TREE" ]; then
  echo "k27 tree mismatch: patches/k27-tree on $BASE gives tree $got, unixnu.lock pins $TREE" >&2; exit 1
fi
# Reserve the final name before renaming. A concurrent fetch may have finished first.
mkdir -- "$DEST"
if ! python3 -c 'import os, sys; os.rename(sys.argv[1], sys.argv[2])' "$WORK" "$DEST"; then
  rmdir -- "$DEST"
  exit 1
fi
echo "k27 tree: $URL ${BASE:0:12} + ${#patches[@]} patches = tree ${TREE:0:12}"
echo "PIANO_K27_TREE=$DEST"
