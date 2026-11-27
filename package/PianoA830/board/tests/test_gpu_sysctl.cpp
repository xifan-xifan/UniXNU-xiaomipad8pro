// SPDX-License-Identifier: BSD-2-Clause-Patent
// c++ -std=c++20 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. test_gpu_sysctl.cpp -o /tmp/test_gpu_sysctl && /tmp/test_gpu_sysctl
#include "PianoGpuSysctl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main()
{
  using namespace pianogpu;
  CHECK(snapshotLength(-1, 4) == 0 && snapshotLength(0, 4) == 0);
  CHECK(snapshotLength(3, 4) == 3 && snapshotLength(3, 4, true) == 4);
  CHECK(snapshotLength(4, 4, true) == 0 && snapshotLength(5, 4) == 0);

  A830RecoveryStats s{};
  s.recoveries = s.replayRecoveries = s.jobsReplayed = s.jobsFailed = s.failures = s.reports = UINT32_MAX;
  s.coalesced = s.watchdogHangs = s.lastUs = s.maxUs = UINT32_MAX;
  s.lastCause = s.lostCause = UINT8_MAX;
  char line[kRecoverySnapshotBytes];
  const size_t bytes = recoverySnapshot(line, 1, s, UINT32_MAX);
  const char expected[] =
      "replay 1 recoveries 4294967295 replay_recoveries 4294967295 jobs_replayed 4294967295 jobs_failed 4294967295 "
      "failures 4294967295 reports 4294967295 coalesced 4294967295 watchdog 4294967295 last_us 4294967295 "
      "max_us 4294967295 last_cause 255 lost_cause 255 holds 4294967295\n";
  CHECK(bytes == sizeof expected && !std::memcmp(line, expected, bytes));
  std::puts("PASS: structured GPU sysctl bounds and complete maximum-width recovery counters");
}
