// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ExtraRam.h"

int main(void) {
  const unsigned long long floor = DarwinExtraRamRegion1[0][0];
  const unsigned long long limit = DarwinExtraRamRegion1[0][1];
  const unsigned long long slab = DARWIN_EXTRA_RAM_SLAB;
  DARWIN_EXTRA_RUN runs[DARWIN_EXTRA_RAM_MAX_RUNS];
  unsigned dropped = 0;

  // An unaligned window containing no complete slab cannot publish a record.
  unsigned count = DarwinExtraRamAdd(runs, 0, 2, floor + 1, floor + slab, floor, limit, &dropped);
  assert(count == 0 && dropped == 0);
  count = DarwinExtraRamAdd(runs, count, 2, floor + 1, floor + 3 * slab + 1, floor, limit, &dropped);
  assert(count == 1 && runs[0].base == floor + slab && runs[0].size == 2 * slab);
  count = DarwinExtraRamAdd(runs, count, 2, floor + 2 * slab, floor + 4 * slab, floor, limit, &dropped);
  assert(count == 1 && runs[0].base == floor + slab && runs[0].size == 3 * slab);
  count = DarwinExtraRamAdd(runs, count, 2, floor + 5 * slab, floor + 6 * slab, floor, limit, &dropped);
  assert(count == 2 && runs[0].base + runs[0].size < runs[1].base);

  // At capacity the final record fits and the encoder leaves the next byte alone.
  for (unsigned i = 0; i < DARWIN_EXTRA_RAM_MAX_RUNS; ++i) {
    runs[i] = (DARWIN_EXTRA_RUN){floor + 2 * i * slab, slab, 1, 1};
  }
  unsigned char bytes[DARWIN_EXTRA_RAM_PROP_BYTES + 1];
  memset(bytes, 0xff, sizeof bytes);
  DarwinExtraRamEncode(runs, DARWIN_EXTRA_RAM_MAX_RUNS, bytes);
  const unsigned last = DARWIN_EXTRA_RAM_PROP_BYTES - DARWIN_EXTRA_RAM_RECORD_BYTES;
  assert(bytes[last + 16] == 1 && bytes[last + 20] == 1);
  assert(bytes[DARWIN_EXTRA_RAM_PROP_BYTES] == 0xff);
  puts("PASS: whole-slab boundaries, disjoint published ranges and final record bounds");
  return 0;
}
