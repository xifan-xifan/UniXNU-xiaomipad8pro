// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "../adreno830/A830Recovery.h"
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

namespace pianogpu {

// snprintf returns the required size. A structured snapshot must fit in its entirety.
inline size_t snapshotLength(int written, size_t capacity, bool withNul = false)
{
  if (written <= 0 || size_t(written) >= capacity) return 0;
  return size_t(written) + withNul;
}

constexpr size_t kRecoverySnapshotBytes = 320;
inline size_t recoverySnapshot(char (&line)[kRecoverySnapshotBytes], uint32_t replay, const A830RecoveryStats &s,
                               uint32_t holds)
{
  const int written = snprintf(line, sizeof line,
      "replay %u recoveries %u replay_recoveries %u jobs_replayed %u jobs_failed %u failures %u "
      "reports %u coalesced %u watchdog %u last_us %u max_us %u last_cause %u lost_cause %u holds %u\n",
      replay, s.recoveries, s.replayRecoveries, s.jobsReplayed, s.jobsFailed, s.failures, s.reports, s.coalesced,
      s.watchdogHangs, s.lastUs, s.maxUs, unsigned(s.lastCause), unsigned(s.lostCause), holds);
  return snapshotLength(written, sizeof line, true);
}

} // namespace pianogpu
