#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/sign-ram-images.XXXXXX")
trap 'rm -rf -- "$W"' EXIT

PROFILE="$W/operator's release"
mkdir -p "$PROFILE/release/boot" "$W/home"
cat > "$PROFILE/release/boot/boot-profile.json" <<'JSON'
{"boot": {"rollback_index": 7, "avb_properties": {"test.name": "two words"}}}
JSON
output=$(HOME="$W/home" SIGN_DRY=1 PIANO_V110="$PROFILE" bash "$D/sign-on-remote.sh")
test "$output" = "SIGN_DRY rollback_index 7 props --prop 'test.name:two words' partition auto"

cat > "$W/stock.avb.json" <<'JSON'
{"avb": {"partition_name": "boot", "partition_size": 100663296, "rollback_index": 7, "props": {"test.name": "two words"}}}
JSON
output=$(HOME="$W/home" SIGN_DRY=1 bash "$D/sign-on-remote.sh" --props-from "$W/stock.avb.json")
test "$output" = "SIGN_DRY rollback_index 7 props --prop 'test.name:two words' partition 100663296"
