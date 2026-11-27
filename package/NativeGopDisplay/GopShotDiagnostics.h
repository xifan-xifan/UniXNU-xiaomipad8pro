// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../PianoDpu/PianoDpuFlipClient.h"

namespace gopshot {

enum class BusyReason : unsigned { oddEpoch, initialLock, finalLock, scanoutChanged, finalEpoch, dpuBusy, count };

inline BusyReason dpuBusyReason(PianoDpuScanoutBusy busy)
{
    switch (busy) {
    case PianoDpuScanoutBusy::initialLock: return BusyReason::initialLock;
    case PianoDpuScanoutBusy::finalLock: return BusyReason::finalLock;
    case PianoDpuScanoutBusy::changed: return BusyReason::scanoutChanged;
    case PianoDpuScanoutBusy::none: return BusyReason::dpuBusy;
    }
    return BusyReason::dpuBusy;
}

inline const char *busyReasonName(BusyReason reason)
{
    static constexpr const char *names[] = {
        "odd-epoch", "initial-flip-lock", "final-flip-lock", "scanout-changed", "final-epoch", "dpu-busy"
    };
    static_assert(sizeof names / sizeof names[0] == unsigned(BusyReason::count));
    return names[unsigned(reason)];
}

struct BusyCounts {
    uint64_t byReason[unsigned(BusyReason::count)] = {};

    uint64_t record(BusyReason reason)
    {
        return __atomic_add_fetch(&byReason[unsigned(reason)], 1, __ATOMIC_RELAXED);
    }
};

}  // namespace gopshot
