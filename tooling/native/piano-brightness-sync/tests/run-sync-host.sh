#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all)
xcrun clang -std=c11 "${flags[@]}" sync_policy_host.c -o "$out/sync_policy_host"
"$out/sync_policy_host"
xcrun clang "${flags[@]}" -fobjc-arc sync_daemon_host.m -framework Foundation -framework IOKit -o "$out/sync_daemon_host"
"$out/sync_daemon_host"
