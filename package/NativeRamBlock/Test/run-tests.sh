#!/bin/bash
# Host tests of NativeRamBlock's disk and store code (no tablet, no kernel): build each test with the host compiler and run it.
#   package/NativeRamBlock/Test/run-tests.sh [SEED.img]   SEED: also print SeedDisk.h's identity of a packed seed (seed-identity.cpp),
#                                                          to compare with tooling/persist/common.py seed_info
# From upstream (UniXNU drivers/NativeRamBlock/Test, see ../UPSTREAM): sparse, persist (adapted to the store guard), direct, seeddisk,
# seed-identity. UniXNU-piano: guard (the store guard, replace, flatten with power cuts, self-contained stores, header compatibility),
# volatile (VolatileDisk.h), volatile-stress (VolatilePool.h: sustained writes, the budget, low memory, unmap, threads with a refill
# thread and a watchdog, and the model of the 2026-11-11 hang; about 20 s), volatile-pool (VolatilePool.h against the 2026-11-12
# desktop session: the boot reservation, fragmented and exhausted address ranges, the backoff, bursts, unmap and the reads after
# it; about 4 s), power (PowerHold.h: the Data disk's part of a system
# sleep), concurrent (direct admission, page ranges, durable bitmap publication and drains), and the
# serial command/byte transcript from 8301b9bb. Binaries go to a temporary directory.
set -euo pipefail
T=$(cd "$(dirname "$0")" && pwd)
B=$(mktemp -d); trap 'rm -rf "$B"' EXIT
fail=0
for t in sparse persist direct seeddisk guard volatile volatile-stress volatile-pool power concurrent; do
  clang++ -std=c++17 -O2 -UNDEBUG -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -o "$B/$t" "$T/$t.cpp" \
    || { echo "FAIL $t: does not build"; fail=1; continue; }
  if out=$("$B/$t" 2>&1); then echo "$t: $out"; else echo "FAIL $t: $out"; fail=1; fi
done
# This digest covers the complete command/return trace and disk bytes from the base commit's DirectDisk.h.
serial_hash=3ef1a172ffcdd70afaa4999ebf9b4bdedd5492505dff900f08169be9707c88b1
if clang++ -std=c++17 -O2 -UNDEBUG -I "$T/.." -o "$B/serial-parity" "$T/serial-parity.cpp" &&
    "$B/serial-parity" > "$B/serial-transcript" &&
    [[ $(shasum -a 256 "$B/serial-transcript" | cut -d ' ' -f 1) = "$serial_hash" ]]; then
  echo "serial-parity: byte-identical I/O transcript and durable store versus 8301b9bb"
else
  echo "FAIL serial-parity: transcript differs from 8301b9bb"
  fail=1
fi
if [ $# -gt 0 ]; then
  clang++ -std=c++17 -O2 -Wno-unused-function -o "$B/seed-identity" "$T/seed-identity.cpp"
  echo "seed identity of $1: $("$B/seed-identity" "$1")"
fi
exit $fail
