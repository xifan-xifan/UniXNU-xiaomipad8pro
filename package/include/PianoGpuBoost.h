// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GPU boost request, sender side (input kexts): PianoA830's "piano-gpu-boost" platform function (GPU-DAILY.md, boost342).
//   callPlatformFunction("piano-gpu-boost", false, (void *)(uintptr_t)mhzOrLevel, (void *)(uintptr_t)holdMs, nullptr, nullptr)
//   param1: 0 = PianoA830's input boost (its touch-boost level, 525 MHz, under every ceiling), 1..16 = a GMU level, else MHz;
//   param2: the hold in ms (0 = PianoA830's touch hold, 300 ms; at most 5000). Requests combine: the highest floor until the latest end.
//   kIOReturnUnsupported: PianoA830 runs without piano-gpu-boost=1 (the sender stops asking for the boot); kIOReturnOffline: switched
//   off at run time (machdep.piano_gpu_boost=0): the sender keeps asking (50 ms apart), so a switch back on works at once.
// InputBoostSender::note() after each HID report handed to macOS: at most one request per 50 ms, PianoA830 looked up at most every 2 s
// until found. Thread context only (it may look the service up); zero-initialised static storage, no constructor.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>

namespace pianogpuboost {
struct InputBoostSender {
  IOService *gpu;
  const OSSymbol *function;
  uint64_t lastNs, nextLookupNs;
  bool off;
  void note()
  {
    if (off) return;
    uint64_t now = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    if (lastNs && now - lastNs < 50000000ull) return;
    lastNs = now;
    if (!gpu) {
      if (now < nextLookupNs) return;
      nextLookupNs = now + 2000000000ull;
      if (!function) function = OSSymbol::withCStringNoCopy("piano-gpu-boost");
      OSDictionary *match = IOService::serviceMatching("PianoA830");
      if (!match || !function) { OSSafeReleaseNULL(match); return; }
      gpu = IOService::copyMatchingService(match);
      match->release();
      if (!gpu) return;
    }
    if (gpu->callPlatformFunction(function, false, nullptr, nullptr, nullptr, nullptr) == kIOReturnUnsupported) off = true;
  }
};
}  // namespace pianogpuboost
