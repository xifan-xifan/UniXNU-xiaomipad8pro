// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single-slot completion policy. Port supplies ordered MMIO, invalidate-only
// descriptor reads, a monotonic deadline and bounded pacing. No resubmission.
enum class UfsTransferEnd { Completed, Fatal, Timeout };
struct UfsTransferPoll {
  uint32_t polls = 0, doorbellSeen = 0, interruptSeen = 0, ocs = 15;
};
static constexpr uint32_t kUfsFatalStatus = 0x71880U;

template <class Port>
static UfsTransferEnd pollUfsTransfer(Port &port, UfsTransferPoll &state)
{
  for (;;) {
    ++state.polls;
    const bool logPoll = state.polls <= 2;
    if (logPoll) IOLog ("NativeUfs: poll %u read\n", state.polls);
    uint32_t doorbell = port.doorbell();
    state.doorbellSeen |= doorbell;
    state.interruptSeen |= port.interrupt();
    if (logPoll) IOLog ("NativeUfs: poll %u db=%u is=%08x\n", state.polls, doorbell, state.interruptSeen);
    if (state.interruptSeen & kUfsFatalStatus) return UfsTransferEnd::Fatal;
    if (!(doorbell & 1)) {
      state.ocs = port.completedOcs();
      // A zero doorbell alone does not prove that this command was accepted.
      // Also allow a completion so fast that software never observes DB=1.
      if (state.ocs != 15) return UfsTransferEnd::Completed;
    }
    // A bounded poll count turns a stalled register read or a frozen timebase into a reported timeout instead of a hang.
    if (port.expired() || state.polls > 2000000) return UfsTransferEnd::Timeout;
    port.delay();
  }
}
