// SPDX-License-Identifier: BSD-2-Clause-Patent
// Portable row-band partitioning shared by NativeGopDisplay and its host tests.
#pragma once
#include "GopCompose.h"

namespace gopcopy {

struct RowSlice {
    int32_t y0 = 0, y1 = 0;
};

inline unsigned sliceCount(int32_t y0, int32_t y1, unsigned workers)
{
    if (y1 <= y0) return 0;
    if (!workers) workers = 1;
    const uint32_t rows = static_cast<uint32_t>(int64_t(y1) - y0);
    return workers < rows ? workers : rows;
}

inline RowSlice sliceAt(int32_t y0, int32_t y1, unsigned workers, unsigned index)
{
    const unsigned count = sliceCount(y0, y1, workers);
    if (!count || index >= count) return {y1, y1};
    const uint32_t rows = static_cast<uint32_t>(int64_t(y1) - y0);
    const uint32_t base = rows / count, extra = rows % count;
    const int64_t start = int64_t(y0) + index * base + (index < extra ? index : extra);
    const int64_t height = base + (index < extra ? 1 : 0);
    return {static_cast<int32_t>(start), static_cast<int32_t>(start + height)};
}

} // namespace gopcopy
