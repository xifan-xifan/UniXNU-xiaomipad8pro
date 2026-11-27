#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/firmware-inputs.XXXXXX")
trap 'rm -rf -- "$W"' EXIT

if DARWINBOOT="$W/no checkout" bash "$D/test-darwinboot-extra-ram.sh" > "$W/log" 2>&1; then
  echo 'the test accepted an explicitly missing DarwinBoot checkout' >&2
  exit 1
fi
grep -F 'FAIL: DARWINBOOT checkout does not contain' "$W/log"
env -u DARWINBOOT UNIXNU_RESEARCH="$W/no sources" bash "$D/test-darwinboot-extra-ram.sh" > "$W/log" 2>&1
grep -F 'PASS: extra-ram region1 clipping' "$W/log"
grep -F 'PASS: whole-slab boundaries' "$W/log"
grep -Fx 'RESULT PASS (checkout-independent extra-RAM tests)' "$W/log"

if bash "$D/make-own-binaries.sh" "$W/no image.zst" "$W/output dir" > "$W/log" 2>&1; then
  echo 'the extractor accepted a missing UEFI image' >&2
  exit 1
fi
grep -F 'missing UEFI input image' "$W/log"
test ! -e "$W/output dir"

# The setup must read the lock before it initializes a checkout, even on Python 3.9.
mkdir "$W/bin"
cat > "$W/bin/git" <<'SH'
#!/bin/sh
[ "$1" = init ] && [ "$2" = -q ] && echo 'fixture stopped the first git init'
exit 19
SH
chmod +x "$W/bin/git"
status=0
PATH="$W/bin:$PATH" UNIXNU_RESEARCH="$W/new sources" bash "$D/setup-mu.sh" > "$W/log" 2>&1 || status=$?
test "$status" = 19
grep -Fx 'fixture stopped the first git init' "$W/log"

# Both extraction and patching use synthetic fixtures. Exercise publication
# without dependencies, a firmware image, or a network connection.
R="$W/fixture sources"
P="$R/piano-uefi-UniXNU-piano/tools"
mkdir -p "$P" "$R/Mu-Silicium-piano/Binaries/dodge" "$W/output dir"
printf 'previous\n' > "$W/output dir/SOURCE.sha256"
touch "$W/synthetic.img.zst"
export FIXTURE_PYTHON
FIXTURE_PYTHON=$(command -v python3)
cat > "$W/bin/python3" <<'SH'
#!/bin/bash
set -eu
if [ "$1" = -m ] && [ "$2" = venv ]; then
  mkdir -p "$3/bin"
  printf '#!/bin/sh\nexec "$FIXTURE_PYTHON" "$@"\n' > "$3/bin/python"
  printf '#!/bin/sh\nexit 0\n' > "$3/bin/pip"
  chmod +x "$3/bin/python" "$3/bin/pip"
else
  exec "$FIXTURE_PYTHON" "$@"
fi
SH
cat > "$W/bin/zstd" <<'SH'
#!/bin/sh
test "$1" = -dq && test "$2" = -o && printf 'synthetic firmware\n' > "$3"
SH
chmod +x "$W/bin/python3" "$W/bin/zstd"
cat > "$P/extract-qcom-dxe.py" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[2]).mkdir(parents=True)
PY
cat > "$P/patch-qcom-dxe.sh" <<'SH'
#!/bin/bash
echo "${FIXTURE_PATCH_ERROR:?}" >&2
exit 1
SH
extract() {
  PATH="$W/bin:$PATH" UNIXNU_RESEARCH="$R" FIXTURE_PATCH_ERROR="$1" \
    bash "$D/make-own-binaries.sh" "$W/synthetic.img.zst" "$W/output dir"
}
if extract 'TzDxeLA: unexpected byte at 0xf5b0' > "$W/log" 2>&1; then
  echo 'the extractor published firmware after an unexpected patch failure' >&2
  exit 1
fi
grep -Fx 'TzDxeLA: unexpected byte at 0xf5b0' "$W/log"
grep -Fx previous "$W/output dir/SOURCE.sha256"
extract 'UFSDxe: unexpected code at the patch sites' > "$W/log" 2>&1
grep -F 'expected at UFSDxe' "$W/log"
cp "$W/output dir/SOURCE.sha256" "$W/first.sha256"
extract 'UFSDxe: unexpected code at the patch sites' > "$W/log" 2>&1
cmp "$W/first.sha256" "$W/output dir/SOURCE.sha256"
