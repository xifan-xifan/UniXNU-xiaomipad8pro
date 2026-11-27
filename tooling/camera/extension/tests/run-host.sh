#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Copyright (c) 2026 Leo Zhang.
set -eu
cd "$(dirname "$0")"
local_buffers=${1:-}
provider_flags=()
if [[ $local_buffers == --local-buffers ]]; then
  provider_flags=(-DPIANO_CAMERA_HOST_HEAP)
elif [[ -n $local_buffers ]]; then
  printf 'usage: %s [--local-buffers]\n' "$0" >&2
  exit 2
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/piano-camera-output.XXXXXX")
trap 'rm -rf "$work"' EXIT
xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  output_host.c -o "$work/output-host"
"$work/output-host"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 -O1 -g \
  -Wall -Wextra -Werror -Wno-unused-parameter -fobjc-arc -fsanitize=address,undefined -fno-sanitize-recover=all \
  ${provider_flags[@]+"${provider_flags[@]}"} provider_host.m -o "$work/provider-host" \
  -framework Foundation -framework CoreMediaIO -framework CoreMedia -framework CoreVideo \
  -framework IOSurface -framework IOKit -framework CoreGraphics
"$work/provider-host"
if [[ $local_buffers == --local-buffers ]]; then exit 0; fi

xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=15.0 -O1 -g \
  -Wall -Wextra -Werror -Wno-unused-parameter -fobjc-arc -fsanitize=address,undefined -fno-sanitize-recover=all \
  -c compat_frame_host.m -o "$work/compat-frame.o"
xcrun --sdk macosx clang++ -arch arm64 -mmacosx-version-min=15.0 -std=c++17 -O1 -g \
  -Wall -Wextra -Werror -fobjc-arc -fsanitize=address,undefined -fno-sanitize-recover=all \
  "$work/compat-frame.o" ../../diagnostic/FrameEvidence.mm -o "$work/compat-frame-host" \
  -framework Foundation -framework CoreMediaIO -framework CoreMedia -framework CoreVideo \
  -framework IOSurface -framework IOKit -framework CoreGraphics -framework ImageIO
"$work/compat-frame-host" "$work"

# The movie probe's file check (PianoCameraCapture --movie) on movies written here; no camera is opened.
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 -O1 -g \
  -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations -fobjc-arc -fsanitize=address,undefined -fno-sanitize-recover=all \
  movie_host.m -o "$work/movie-host" \
  -framework AppKit -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework CoreImage -framework CoreGraphics
"$work/movie-host"
