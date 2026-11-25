#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
set -eu
cd "$(dirname "$0")"
work=$(mktemp -d "${TMPDIR:-/tmp}/piano-camera-output.XXXXXX")
trap 'rm -rf "$work"' EXIT
xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  output_host.c -o "$work/output-host"
"$work/output-host"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 -O1 -g \
  -Wall -Wextra -Werror -Wno-unused-parameter -fobjc-arc -fsanitize=address,undefined \
  provider_host.m -o "$work/provider-host" \
  -framework Foundation -framework CoreMediaIO -framework CoreMedia -framework CoreVideo \
  -framework IOSurface -framework IOKit -framework CoreGraphics
"$work/provider-host"

xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=15.0 -O1 -g \
  -Wall -Wextra -Werror -Wno-unused-parameter -fobjc-arc -fsanitize=address,undefined \
  -c compat_frame_host.m -o "$work/compat-frame.o"
xcrun --sdk macosx clang++ -arch arm64 -mmacosx-version-min=15.0 -std=c++17 -O1 -g \
  -Wall -Wextra -Werror -fobjc-arc -fsanitize=address,undefined \
  "$work/compat-frame.o" ../../diagnostic/FrameEvidence.mm -o "$work/compat-frame-host" \
  -framework Foundation -framework CoreMediaIO -framework CoreMedia -framework CoreVideo \
  -framework IOSurface -framework IOKit -framework CoreGraphics -framework ImageIO
"$work/compat-frame-host" "$work"

# The movie probe's file check (PianoCameraCapture --movie) on movies written here; no camera is opened.
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 -O1 -g \
  -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations -fobjc-arc -fsanitize=address,undefined \
  movie_host.m -o "$work/movie-host" \
  -framework AppKit -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework CoreImage -framework CoreGraphics
"$work/movie-host"
