#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d)
trap 'rm -rf "$O"' EXIT
flags=(-Wall -Wextra -Werror)
if [ "${1:-}" = --sanitize ]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  shift
fi
if [ "$#" -ne 0 ]; then
  echo "usage: $0 [--sanitize]" >&2
  exit 2
fi
"${CC:-cc}" -std=c11 "${flags[@]}" "$D/tests/args_host.c" -o "$O/args_host"
"$O/args_host"
