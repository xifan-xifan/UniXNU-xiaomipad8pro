// SPDX-License-Identifier: BSD-2-Clause-Patent
// The fixed refresh contract of NativeGopDisplay (boot-arg native-gop-display-120=1; docs/drivers/native-gop-display.md "120 Hz"):
// the numbers the driver gives macOS 27.3 for one rate, so that every interface WindowServer reads tells the same rate.
//   - the timing element (TimingElements, raw +56 = Hz << 16): QuartzCore's mode refresh, CGDisplayModeGetRefreshRate;
//   - IOMobileFramebufferLegacy::get_timing_attributes_gated (27.3 vtable slot 423): the family's own one returns 1/60 s whatever the
//     mode (min interval 0x04444444 in 32.32 seconds, max the same or 1/24 s); its setDisplayRefreshProperties() publishes what the
//     slot returns as the registry dictionary IOMFBDisplayRefresh, whose displayMinRefreshInterval QuartzCore's
//     AppleDisplay::update_display_limits stores and AppleInternalDisplay::heartbeat_rate falls back to;
//   - the VBL tick, on absolute deadlines one period apart (a relative re-arm drifts by the work loop's latency every tick).
// Portable (no kernel headers): NativeGopDisplay.cpp is the kernel side, tests/gop_host.cpp the host test.
#pragma once
#include <stdint.h>

namespace goprefresh {

// IOMFB::RefreshTimingAttributes as 27.3's IOMobileFramebufferLegacy::get_timing_attributes_gated (0xfffffe000ac32880) fills it:
// six 64-bit words, pairs of (32.32 fixed-point seconds, mach absolute time): the minimum interval, the maximum, the step.
enum : unsigned { kMinFixed, kMinAbs, kMaxFixed, kMaxAbs, kStepFixed, kStepAbs, kWords };

// One period in 32.32 seconds, rounded down as the family's constants are (60 Hz: 0x04444444, 24 Hz: 0x0aaaaaaa).
constexpr uint64_t periodFixed(uint32_t hz) { return hz ? (uint64_t(1) << 32) / hz : 0; }
// One period in nanoseconds (the family converts 1/60 s as 16666666 ns).
constexpr uint64_t periodNs(uint32_t hz) { return hz ? 1000000000ull / hz : 0; }

// A fixed rate: minimum = maximum = one period, no step (no adaptive sync, no ProMotion range: the panel's mode is fixed).
// nsToAbs converts nanoseconds to mach absolute time. False for a rate outside 30..240 (the driver's accepted range).
template <class NsToAbs> bool fixedAttributes(uint32_t hz, NsToAbs nsToAbs, uint64_t words[kWords])
{
    if (hz < 30 || hz > 240)
        return false;
    const uint64_t fixed = periodFixed(hz), abs = nsToAbs(periodNs(hz));
    words[kMinFixed] = fixed;
    words[kMinAbs] = abs;
    words[kMaxFixed] = fixed;
    words[kMaxAbs] = abs;
    words[kStepFixed] = 0;
    words[kStepAbs] = 0;
    return true;
}

// The VBL tick's next absolute deadline. due: the current one (0: none armed), now, period (all mach absolute time).
//   - none armed: one period from now;
//   - one still ahead (a swap submitted mid-period calls swap_begin_gated, which re-arms): kept, the phase does not move;
//   - one passed (the tick fired, or the work loop was late): the next on the same grid after now, skipping the missed ones (no
//     burst of catch-up ticks); more than 8 periods behind (a stop, a long stall): a fresh grid from now.
constexpr uint64_t nextDeadline(uint64_t due, uint64_t now, uint64_t period)
{
    if (!period)
        return now;
    if (!due || (now > due && now - due > 8 * period))
        return now + period;
    if (due > now)
        return due;
    return due + ((now - due) / period + 1) * period;
}

}   // namespace goprefresh
