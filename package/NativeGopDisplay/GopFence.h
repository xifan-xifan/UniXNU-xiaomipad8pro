// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace gopfence {

enum PollMode : uint32_t { Unavailable, Legacy, Deadline };
constexpr uint32_t kPollNs = 1000000;

constexpr bool deadlineControlAvailable(uint32_t modeHz, uint32_t publishedHz, bool flip, bool fences)
{
    return modeHz == 120 && publishedHz == 120 && flip && fences;
}

struct Result {
    bool answered;
    bool retired;
    bool timedOut;
};

// Keep the original ring sequence in both arms. A missing provider before the
// first read differs from a provider that stops answering with a fence pending.
template <class Read, class Elapsed, class Sleep>
Result wait(uint32_t fence, uint64_t budgetNs, bool deadline, Read read, Elapsed elapsed, Sleep sleep)
{
    bool answered = false;
    uint32_t retired = 0;
    while (read(&retired)) {
        answered = true;
        if (static_cast<int32_t>(retired - fence) >= 0)
            return {true, true, false};
        const uint64_t waited = elapsed();
        if (waited >= budgetNs)
            return {true, false, true};
        const uint64_t remaining = budgetNs - waited;
        sleep(deadline && remaining < kPollNs ? static_cast<uint32_t>(remaining) : kPollNs);
    }
    return {answered, false, false};
}

} // namespace gopfence
