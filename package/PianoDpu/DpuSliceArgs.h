// SPDX-License-Identifier: BSD-2-Clause-Patent
// Boot options for the writing slices. The caller supplies the boot-argument readers.
#pragma once
#include <string.h>
#include "DpuModes.h"
#include "DpuRefresh.h"

namespace pianodpu::slices {

constexpr unsigned kRefreshOff = 0, kRefreshAuto = 1, kRefreshPlan = 2;

struct SliceArgs {
  unsigned level = 0;
  unsigned refresh = kRefreshOff, refreshDb = 1, refreshTest = 0;
  unsigned modeHz = 0;
  refresh::PolicyConfig policy;
  unsigned pcc = 0, pccEncode = 1, pccNegative = 0;
  bool nightShift = false;
  bool pccTest = false;
  int32_t pccTestMilli[3] = {1000, 1000, 1000};
  unsigned panel = 0, panelEngines = 1, panelHs = 0, panelTest = 0;
  bool panelTrace = false, panelVideoOnly = false;
  unsigned ahbVote = 0;   // piano-dpu-ahb-vote: 1 = the gate's Linux rule for GCC's display AHB (DpuAhbVote.h)

  bool modesEnabled() const { return modeHz && !refresh && refreshDb; }
  bool wanted() const { return refresh || modesEnabled() || pcc || panel || nightShift; }
};

template <class ReadNumber, class ReadString>
SliceArgs readStartupArgs(ReadNumber readNumber, ReadString readString)
{
  const auto argNumber = [&](const char *name, uint32_t fallback, uint32_t lo, uint32_t hi) {
    uint32_t v = fallback;
    if (!readNumber(name, &v, sizeof v)) v = fallback;
    return v < lo ? lo : v > hi ? hi : v;
  };
  SliceArgs args;
  args.level = argNumber("piano-dpu", 0, 0, 100);
  char s[32] = {};
  if (readString("piano-dpu-refresh", s, sizeof s)) {
    if (!strcmp(s, "auto")) args.refresh = kRefreshAuto;
    else if (!strcmp(s, "plan")) args.refresh = kRefreshPlan;
    else if (!strcmp(s, "60")) args.refresh = 60;
    else if (!strcmp(s, "120")) args.refresh = 120;
  }
  args.refreshDb = argNumber("piano-dpu-refresh-db", 1, 0, 2);
  args.modeHz = modes::bootModeHz(argNumber("piano-dpu-modes", 0, 0, UINT32_MAX));
  args.refreshTest = argNumber("piano-dpu-refresh-test", 0, 0, 1000);
  args.policy.idleMs = argNumber("piano-dpu-refresh-idle", 1000, 100, 60000);
  args.policy.burstSwaps = argNumber("piano-dpu-refresh-burst", 2, 1, 8);
  args.policy.burstWindowMs = argNumber("piano-dpu-refresh-window", 250, 20, 5000);
  args.policy.dwellMs = argNumber("piano-dpu-refresh-dwell", 200, 50, 60000);
  args.pcc = argNumber("piano-dpu-pcc", 0, 0, 2);
  uint32_t nightShift = 0;
  readNumber("native-gop-display-night-shift", &nightShift, sizeof nightShift);
  args.nightShift = nightShift == 1;
  if (args.nightShift) args.pcc = 0;   // the two experiments share one PCC
  const unsigned panelArg = argNumber("piano-dpu-panel", 0, 0, UINT32_MAX);
  args.panel = panelArg != 0;
  args.panelTrace = panelArg == 1;
  args.panelVideoOnly = argNumber("piano-dpu-panel-video-only", 0, 0, UINT32_MAX) == 1;
  args.ahbVote = argNumber("piano-dpu-ahb-vote", (args.nightShift || args.modesEnabled() || args.panel) && args.level >= 2 ? 1 : 0, 0, 2);
  args.pccEncode = argNumber("piano-dpu-pcc-encode", 1, 0, 1);
  args.pccNegative = argNumber("piano-dpu-pcc-negative", 0, 0, 1);
  memset(s, 0, sizeof s);
  if (readString("piano-dpu-pcc-test", s, sizeof s)) {
    unsigned k = 0, v = 0;
    bool digit = false, ok = true;
    for (const char *c = s;; ++c) {
      if (*c >= '0' && *c <= '9') {
        v = v * 10 + unsigned(*c - '0');
        digit = true;
        if (v > 4000) {
          ok = false;
          break;
        }
      } else if (*c == ',' || !*c) {
        ok &= digit && k < 3;
        if (ok) args.pccTestMilli[k++] = int32_t(v);
        v = 0;
        digit = false;
        if (!*c) break;
      } else {
        ok = false;
        break;
      }
    }
    args.pccTest = ok && k == 3;
  }
  args.panelEngines = argNumber("piano-dpu-panel-engines", 1, 0, 1);
  args.panelHs = argNumber("piano-dpu-panel-hs", 0, 0, 1);
  args.panelTest = argNumber("piano-dpu-panel-test", 0, 0, 1000);
  return args;
}

}  // namespace pianodpu::slices
