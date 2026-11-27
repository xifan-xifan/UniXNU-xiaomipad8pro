#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

CDPATH='' cd -- "$(dirname -- "$0")/../../.."
# shellcheck source=scripts/ci/host-test-jobs.sh
source scripts/ci/host-test-jobs.sh
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/piano-video-host.XXXXXX")
trap 'rm -r -- "$build_dir"' EXIT

checks=(-O1 -g -Wall -Wextra -Werror '-fsanitize=address,undefined' -fno-sanitize-recover=all)
export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1}"
export UBSAN_OPTIONS="halt_on_error=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"

run_video_test() {
  local test=$1
  case $test in
    bringup_host)
      "${CC:-cc}" -std=c11 "${checks[@]}" -c package/PianoVideo/QcomMdt.c -o "$build_dir/QcomMdt.o"
      "${CXX:-c++}" -std=c++20 "${checks[@]}" -I package/PianoVideo -I package/include \
        package/PianoVideo/tests/bringup_host.cpp "$build_dir/QcomMdt.o" -o "$build_dir/bringup_host"
      # The model uses a synthetic image. The native build can also check the optional vpu35_4v.mbn.
      VIDEO_HOST_FW='' "$build_dir/bringup_host"
      ;;
    fixture_host)
      python3 package/PianoVideo/tests/fixture_host.py
      ;;
    vt_serializer_host)
      "${CXX:-c++}" -std=c++20 "${checks[@]}" -Wno-deprecated-declarations \
        package/PianoVideo/tests/vt_serializer_host.mm -framework CoreFoundation -framework CoreMedia \
        -framework CoreVideo -framework VideoToolbox -o "$build_dir/vt_serializer_host"
      "$build_dir/vt_serializer_host"
      ;;
    *)
      "${CXX:-c++}" -std=c++20 "${checks[@]}" -I package/PianoVideo -I package/PianoVideo/vt -I package/include \
        "package/PianoVideo/tests/$test.cpp" -o "$build_dir/$test"
      "$build_dir/$test"
      case $test in
        enc_host)
          python3 package/PianoVideo/tests/enc_stream_check.py --self-test --pattern-tool "$build_dir/enc_host"
          ;;
        hevc_host)
          python3 package/PianoVideo/tests/hevc_files_host.py --host "$build_dir/hevc_host" \
            package/PianoVideo/tests/fixtures/v9/main10-1080.mp4 package/PianoVideo/tests/fixtures/v9/main8-1080.mp4
          ;;
      esac
      ;;
  esac
}

tests=(bringup_host stream_host session_host video_host geometry_host hevc_host enc_host fixture_host
       vt_host vt_surface_host vt_sync_host vt_contract_host vt_cancel_host video_vt_cancel_e1_handoff_host)
if [ "$(uname -s)" = Darwin ]; then
  tests+=(vt_serializer_host)
fi
run_host_jobs "$build_dir" run_video_test "${tests[@]}"
