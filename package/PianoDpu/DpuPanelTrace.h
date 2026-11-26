// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "DpuSlices.h"

namespace pianodpu {
namespace panel {
using namespace slices;

enum class TraceStep {
  displayOff, displayOffDone, sleepIn, sleepInDone, stopSlave, stopMaster, fetchOff,
  undo, fetchOn, enable, align, start, flush, rate, sleepOut, displayOn,
  drain, ctlReset, handoff, routeOn, dsiReset, end
};

inline const char *traceStepName(TraceStep step)
{
  switch (step) {
  case TraceStep::displayOff:     return "dcs28";
  case TraceStep::displayOffDone: return "dcs28-end";
  case TraceStep::sleepIn:        return "dcs10";
  case TraceStep::sleepInDone:    return "dcs10-end";
  case TraceStep::stopSlave:      return "stop2";
  case TraceStep::stopMaster:     return "stop1";
  case TraceStep::fetchOff:       return "fetch-off";
  case TraceStep::undo:           return "undo";
  case TraceStep::fetchOn:        return "fetch-on";
  case TraceStep::enable:         return "enable";
  case TraceStep::align:          return "align";
  case TraceStep::start:          return "start";
  case TraceStep::flush:          return "flush";
  case TraceStep::rate:           return "rate";
  case TraceStep::sleepOut:       return "dcs11";
  case TraceStep::displayOn:      return "dcs29";
  case TraceStep::drain:          return "drain";
  case TraceStep::ctlReset:       return "ctl-reset";
  case TraceStep::handoff:        return "handoff";
  case TraceStep::routeOn:        return "route-on";
  case TraceStep::dsiReset:       return "dsi-reset";
  case TraceStep::end:            return "end";
  }
  return "?";
}

struct TraceLink {
  uint32_t en, status, frame, line;
};

struct TraceSample {
  uint64_t us;
  TraceLink link[2];
  uint32_t top, active, flush, intfFlush;
  TraceStep step;
  bool boundary;
};

struct TraceRing {
  static constexpr unsigned kCapacity = 2048;
  TraceSample samples[kCapacity];
  unsigned total = 0;

  void reset() { total = 0; }
  unsigned size() const { return total < kCapacity ? total : kCapacity; }
  TraceSample &next() { return samples[total++ % kCapacity]; }
  const TraceSample &at(unsigned i) const { return samples[(total - size() + i) % kCapacity]; }
};

struct Trace {
  TraceRing *ring = nullptr;
  unsigned ctl = 0;
  TraceStep step = TraceStep::end;

  template <class Io>
  void capture(Io &io, bool boundary, PollRead a = {}, PollRead b = {}) const
  {
    if (!ring) return;
    TraceSample &s = ring->next();
    s.us = io.nowUs();
    s.step = step;
    s.boundary = boundary;
    auto read = [&](uint64_t pa) {
      if (pa == a.pa) return a.value;
      if (pa == b.pa) return b.value;
      return io.rd(pa);
    };
    for (unsigned i = 1; i <= 2; ++i) {
      s.link[i - 1] = {read(intfReg(i, kIntfEn)), read(intfReg(i, kIntfStatus)),
                       read(intfReg(i, kIntfFrameCount)), read(intfReg(i, kIntfLineCount))};
    }
    s.top = read(ctlReg(ctl, kCtlTop));
    s.active = read(ctlReg(ctl, kCtlIntfActive));
    s.flush = read(ctlReg(ctl, kCtlFlush));
    s.intfFlush = read(ctlReg(ctl, kCtlIntfFlush));
  }

  template <class Io>
  void boundary(Io &io, TraceStep next)
  {
    step = next;
    capture(io, true);
  }

  template <class Io>
  void sample(Io &io, PollRead a = {}, PollRead b = {}) const
  {
    capture(io, false, a, b);
  }

  template <class Io>
  void pause(Io &io, uint32_t us, bool sleep, PollRead a = {}, PollRead b = {}) const
  {
    if (!ring) {
      PlainPoll{}.pause(io, us, sleep);
      return;
    }
    // Keep the next poll due at the end of its original wait. In particular,
    // do not accumulate a row of MMIO latency at every 1 ms stop poll.
    const uint64_t deadline = io.pollDeadline(us);
    sample(io, a, b);
    io.pollWait(deadline, sleep);
  }
};

}  // namespace panel
}  // namespace pianodpu
