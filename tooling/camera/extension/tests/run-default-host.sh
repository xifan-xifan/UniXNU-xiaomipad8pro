#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
set -eu
cd "$(dirname "$0")"
work=$(mktemp -d "${TMPDIR:-/tmp}/piano-camera-default.XXXXXX")
trap 'rm -rf "$work"' EXIT
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 -O1 -g \
  -Wall -Wextra -Werror -Wno-unused-parameter -fobjc-arc -fsanitize=address,undefined \
  camera_default_host.m -o "$work/camera-default-host" -framework Foundation -framework AVFoundation
"$work/camera-default-host"
# Compile the production entry point too; this never runs its app preference writer.
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 -O2 -Wall -Wextra -Werror \
  -fobjc-arc ../camera-default.m -o "$work/piano-camera-default" -framework Foundation -framework AVFoundation
plutil -lint ../com.unixnu.piano-camera-default.plist
