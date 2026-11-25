// SPDX-License-Identifier: BSD-2-Clause-Patent
// The input boost's event (2026-11-22, the owner's "touch/input boost"; docs/drivers/thermal.md section 15): every input kext tells
// PianoThermal that it just sent macOS a HID report, without a link dependency: callPlatformFunction("piano-perf-input", source) on
// the registered PianoThermal service, at most once per kMinGapNs per kext. PianoThermal stamps the time, wakes its thread when no
// input boost holds, and raises the CPU floors for piano-cpufreq-input-ms after the last report (its own rules decide; nothing here).
// Call notify() only where the kext calls IOHIDDevice::handleReport (thread context, no spin lock held): the first call looks the
// service up (IOService::copyMatchingService), later ones only call it. A kext without PianoThermal in the collection pays one failed
// lookup per second at most.
#ifndef PIANO_PERF_INPUT_H
#define PIANO_PERF_INPUT_H
#include <IOKit/IOService.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

namespace pianoperf {

enum Source : uint32_t { kTouch = 1, kKeyboard = 2, kPointer = 3, kButtons = 4 };

class InputNotifier {
 public:
  static constexpr uint64_t kMinGapNs = 8000000ull;    // 8 ms: one event per frame at 120 Hz is plenty
  static constexpr uint64_t kRetryNs = 1000000000ull;  // a missing PianoThermal is looked up again after 1 s

  void notify(uint32_t source)
  {
    const uint64_t now = nowNs();
    if (lastNs_ && now - lastNs_ < kMinGapNs) return;
    lastNs_ = now;
    IOService *svc = __atomic_load_n(&svc_, __ATOMIC_ACQUIRE);
    if (!svc) {
      if (lookupNs_ && now - lookupNs_ < kRetryNs) return;
      lookupNs_ = now;
      svc = lookup();
      if (!svc) return;
      IOService *expected = nullptr;
      if (!__atomic_compare_exchange_n(&svc_, &expected, svc, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        svc->release();   // another thread cached it first
        svc = expected;
      }
    }
    if (svc->isInactive()) return;
    if (!sym_) sym_ = OSSymbol::withCStringNoCopy("piano-perf-input");
    if (sym_) svc->callPlatformFunction(sym_, false, reinterpret_cast<void *>(uintptr_t(source)), nullptr, nullptr, nullptr);
    ++sent_;
  }
  uint32_t sent() const { return sent_; }

 private:
  static uint64_t nowNs()
  {
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
  }
  static IOService *lookup()
  {
    OSDictionary *match = IOService::serviceMatching("PianoThermal");
    IOService *s = match ? IOService::copyMatchingService(match) : nullptr;
    OSSafeReleaseNULL(match);
    return s;   // retained, kept for the boot
  }
  IOService *svc_ = nullptr;
  const OSSymbol *sym_ = nullptr;
  uint64_t lastNs_ = 0, lookupNs_ = 0;
  uint32_t sent_ = 0;
};

}  // namespace pianoperf

#endif
