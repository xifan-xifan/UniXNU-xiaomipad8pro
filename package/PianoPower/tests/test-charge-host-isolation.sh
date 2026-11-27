#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The charge suite must keep compiler outputs separate across invocations.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/charge-host-isolation.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/repo/package/PianoPower/tests" "$work/repo/scripts/ci"
suite="$work/repo/package/PianoPower/tests/run-charge-host.sh"
cp "$here/run-charge-host.sh" "$suite"
cp "$here/../../../scripts/ci/host-test-jobs.sh" "$work/repo/scripts/ci/host-test-jobs.sh"

cat > "$work/cc" <<'EOF'
#!/bin/bash
set -eu
while [ "$1" != -o ]; do shift; done
printf '%s\n' "${2%/*}" >> "$OUTPUT_LOG"
printf '#!/bin/bash\nexit 0\n' > "$2"
chmod +x "$2"
EOF
chmod +x "$work/cc"

for run in first second; do
    TMPDIR="$work" OUTPUT_LOG="$work/$run" CC="$work/cc" CXX="$work/cc" bash "$suite"
done
first=$(sort -u "$work/first")
second=$(sort -u "$work/second")
if [ "$first" = "$second" ]; then
    echo "FAIL: both charge suites built into the same directory"
    exit 1
fi
echo "PASS: charge host build directories are isolated"
