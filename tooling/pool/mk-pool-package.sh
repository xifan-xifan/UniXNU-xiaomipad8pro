#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# mk-pool-package.sh [--base ESP.img]: the pool install's package (out/pool/install) and output folder (out/persistent) in this
# worktree, for tooling/pool/pool-install.sh. Copies the base line's package (BASE_PACKAGE, default the out/v131-install
# of the supplied BASE_WORKTREE): install-functions.sh, install-gate.sh, maintenance/ (the maintenance image),
# evidence/v12-observations.json, and installed-check.py with exactly one change (the pool ESP record's kind k273-pool and default
# macos-27.3-pool accepted besides k273-only / macos-27.3). Adds pool-host.sh, pool-device.sh, pool-inventory-check.sh and the
# pool-manifest binary. out/persistent gets clones (cp -c) of the v13.1y10 worktree's records, images, GPT and boot files and its
# installed.json; --base ESP.img (a rehearsal before tonight's install) writes installed.json for that ESP instead, from its record.
# Writes PACKAGE.sha256 (checked by pool-install.sh's pre phase) and PACKAGE.tsv (sha256, bytes, path, source). Refuses while a round
# of this package holds the tablet lock.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd -P)
# The base worktree holds the ESP named by the main ledger.
MAINCK=$(git -C "$D" rev-parse --path-format=absolute --git-common-dir); MAINCK=${MAINCK%/.git}
LEDGER_IMG=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["image"])' \
    "${POOL_MAIN_LEDGER:-$MAINCK/out/persistent}/installed.json")
Y10W=${BASE_WORKTREE:-${Y10_WORKTREE:-}}
[ -n "$Y10W" ] || { echo 'set BASE_WORKTREE to the worktree holding the base install' >&2; exit 1; }
Y10P=${BASE_PACKAGE:-${Y10_PACKAGE:-$Y10W/out/v131-install}}
[ -f "$Y10W/out/persistent/$LEDGER_IMG" ] ||
    { echo "refused: BASE_WORKTREE does not contain the ESP named in the main ledger" >&2; exit 1; }
for f in install-functions.sh install-gate.sh installed-check.py maintenance; do
    [ -e "$Y10P/$f" ] || { echo "refused: the base package $Y10P has no $f" >&2; exit 1; }
done
PKG=$D/out/pool/install
OUT=$D/out/persistent
BASE=''
while [ $# -gt 0 ]; do case $1 in --base) BASE=${2:?}; shift 2;; *) echo "usage: mk-pool-package.sh [--base ESP.img]" >&2; exit 1;; esac; done
if [ -f /tmp/piano-tablet.lock/owner ] && grep -q "^$PKG/rounds/" /tmp/piano-tablet.lock/owner; then
    echo "refused: a pool round holds the tablet lock ($(cat /tmp/piano-tablet.lock/owner))" >&2; exit 1
fi
mkdir -p "$PKG" "$OUT"
copy() {   # SOURCE DEST: through a new file
    cp -p "$1" "$2.new" && mv -f "$2.new" "$2"
}
for f in install-functions.sh install-gate.sh; do copy "$Y10P/$f" "$PKG/$f"; done
python3 - "$Y10P/installed-check.py" "$PKG/installed-check.py" <<'PY'
import sys
src, dst = sys.argv[1:]
text = open(src).read()
old = 'require(profile["kind"] == "k273-only" and profile["default"] == "macos-27.3", "installed default must be macOS 27.3")'
new = ('require((profile["kind"], profile["default"]) in {("k273-only", "macos-27.3"), ("k273-pool", "macos-27.3-pool")},\n'
       '            "installed default must be macOS 27.3 (or its pool entry, T-20: tooling/pool/mk-pool-package.sh)")')
assert text.count(old) == 1, 'installed-check.py: the profile rule is not the expected one'
open(dst + '.new', 'w').write(text.replace(old, new))
PY
mv -f "$PKG/installed-check.py.new" "$PKG/installed-check.py"
rm -rf "$PKG/maintenance.new"; cp -cRp "$Y10P/maintenance" "$PKG/maintenance.new"; rm -rf "$PKG/maintenance"; mv "$PKG/maintenance.new" "$PKG/maintenance"
mkdir -p "$PKG/evidence"; copy "$Y10P/evidence/v12-observations.json" "$PKG/evidence/v12-observations.json"
for f in pool-host.sh pool-device.sh pool-inventory-check.sh pool-live-check.py; do copy "$D/tooling/pool/$f" "$PKG/$f"; done
[ -x "$D/out/pool/pool-manifest" ] || bash "$D/tooling/pool/build-pool-manifest.sh" > /dev/null
copy "$D/out/pool/pool-manifest" "$PKG/pool-manifest"
# out/persistent: clones of the v13.1y10 worktree's (never its session or backups)
for f in "$Y10W"/out/persistent/*; do
    b=$(basename "$f")
    case "$b" in session|*.bak-*|*.new|*-pool*) continue;; esac
    [ -f "$f" ] || continue
    if [ ! -f "$OUT/$b" ] || ! cmp -s "$f" "$OUT/$b"; then cp -c -p "$f" "$OUT/$b.new" 2>/dev/null || cp -p "$f" "$OUT/$b.new"; mv -f "$OUT/$b.new" "$OUT/$b"; fi
done
if [ -n "$BASE" ]; then
    python3 - "$OUT" "$BASE" <<'PY'
import json, sys, time
from pathlib import Path
out, base = Path(sys.argv[1]), sys.argv[2]
r = json.loads((out / (Path(base).stem + '.json')).read_text())
assert r['image'] == base
(out / 'installed.json.new').write_text(json.dumps({'image': r['image'], 'sha256': r['sha256'], 'seed': r['seed'], 'inputs': r.get('inputs'),
    'written': time.strftime('%Y-%m-%d %H:%M:%S'), 'by': 'mk-pool-package.sh --base (a rehearsal, as after the v13.1y10 install)'}, indent=1) + '\n')
PY
    mv -f "$OUT/installed.json.new" "$OUT/installed.json"
fi
( cd "$PKG" && find . -type f ! -name 'PACKAGE.*' ! -path './rounds/*' ! -path './rehearsals/*' | LC_ALL=C sort | xargs shasum -a 256 > PACKAGE.sha256.new ) && mv -f "$PKG/PACKAGE.sha256.new" "$PKG/PACKAGE.sha256"
{ printf 'sha256\tbytes\tpath\tsource\n'
  while read -r h p; do
      p=${p#./}; src=''
      case "$p" in install-functions.sh|install-gate.sh|maintenance/*|evidence/*) src=$Y10P/$p;; installed-check.py) src="$Y10P/installed-check.py (one rule changed)";;
                   pool-manifest) src=$D/out/pool/pool-manifest;; *) src=$D/tooling/pool/$p;; esac
      printf '%s\t%s\t%s\t%s\n' "$h" "$(stat -f %z "$PKG/$p")" "$p" "$src"
  done < "$PKG/PACKAGE.sha256"; } > "$PKG/PACKAGE.tsv"
printf 'pool package %s: %s files; out/persistent installed.json = %s\n' "$PKG" "$(wc -l < "$PKG/PACKAGE.sha256" | tr -d ' ')" \
    "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["image"])' "$OUT/installed.json")"
