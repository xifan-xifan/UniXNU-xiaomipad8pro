#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
make -C "$(dirname "$0")" test OUT="$out"
