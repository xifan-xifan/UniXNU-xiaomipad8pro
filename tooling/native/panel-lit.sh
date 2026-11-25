#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Capture a fresh panel image, or grade a saved fixture with --grade.
set -euo pipefail
if [ "${1:-}" = --grade ]; then
  shift
else
  photo=${1:?photo path required}
  rm -f -- "$photo"
  imagesnap -q -w 1.5 -d 'HD Pro Webcam C920' "$photo"
fi
photo=${1:?photo path required}
[ -s "$photo" ] || exit 2

# Match the crop and threshold qualified in lock-corner-ab4.
yavg=$(ffmpeg -nostdin -hide_banner -loglevel error -i "$photo" -frames:v 1 \
  -vf 'crop=iw*0.45:ih*0.45:iw*0.25:ih*0.25,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-' \
  -f null - | sed -n 's/^lavfi.signalstats.YAVG=\([0-9.]*\)$/\1/p')
[ -n "$yavg" ] || exit 2
if awk -v y="$yavg" 'BEGIN { exit !(y > 12) }'; then
  printf 'panel-lit: YAVG=%s lit=1\n' "$yavg"
else
  printf 'panel-lit: YAVG=%s lit=0\n' "$yavg"
  exit 1
fi
