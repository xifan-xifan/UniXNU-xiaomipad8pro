#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

CDPATH='' cd -- "$(dirname -- "$0")/../../.."
# shellcheck source=scripts/ci/host-test-jobs.sh
source scripts/ci/host-test-jobs.sh
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/piano-camera-host.XXXXXX")
trap 'rm -r -- "$build_dir"' EXIT

# The parser self-test uses synthetic tables, so it can run alongside validation of the checked-in tables.
check_camera_tables() {
  if [[ $1 == parser_self_test ]]; then
    python3 tooling/camera/sensor-tables.py --self-test
  else
    python3 tooling/camera/sensor-tables.py check data/camera
  fi
}
run_host_jobs "$build_dir" check_camera_tables parser_self_test manifest
python3 tooling/camera/sensor-tables.py header data/camera "$build_dir/CameraTables.h"

# The model writes whole 16 MiB frames, so use the native build's -O1.
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I package/PianoCamera -I package/PianoPower -I package/PianoKeyboard -I package/include -I "$build_dir" \
  package/PianoCamera/tests/camera_host.cpp -o "$build_dir/camera_host"
PIANO_CAMERA_MANIFEST=data/camera/MANIFEST "$build_dir/camera_host"
