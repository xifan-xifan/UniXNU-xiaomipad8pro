// SPDX-License-Identifier: BSD-2-Clause-Patent
// B7 offline model for split MMIO, DMA layout, and controller loan teardown.
// Build: c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -I../../PianoUsbHost -I../../PianoDwc3 xhci_dma_ring_model_host.cpp -o /tmp/xhci_dma_ring_model_host && /tmp/xhci_dma_ring_model_host
#include "Reg64.h"
#include "ControllerLoan.h"
#include "XhciHost.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <optional>
#include <vector>

namespace {

using namespace pianousbhost;

#define CHECK(condition) do { \
  if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::exit(1); \
  } \
} while (0)

const Xhci kCaps = [] {
  Xhci x{};
  x.sane = true;
  x.maxSlots = 8;
  x.capLength = 0x40;
  x.rtsOff = 0x1000;
  x.dbOff = 0x2000;
  return x;
}();
const XhciAt kAt = xhciAt(kCaps);
const uint64_t kCrcr = kAt.op + kOpCrcr;
const uint64_t kDcbaap = kAt.op + kOpDcbaap;
const uint64_t kErstba = kAt.rt + kRtIr0 + kIrErstba;
const uint64_t kErdp = kAt.rt + kRtIr0 + kIrErdp;
const std::array kPointerRegs{kCrcr, kDcbaap, kErstba, kErdp};
const RingLayout kLayout = ringLayout(kCaps, 8);
constexpr uint64_t kBus = 0x0000001289ab0000;
constexpr uint32_t kTrbBytes = 16, kContextAlign = 64, kBoundary = 0x10000;

struct Access {
  char kind;
  uint64_t offset = 0;
  uint32_t value = 0;
  bool operator==(const Access &) const = default;
};

// The two controllers latch on different halves. Track the latched value separately
// from the two staged words, which eventually look correct even for a reversed write.
struct SplitRegister {
  uint64_t offset;
  bool latchOnLow;
  uint64_t latched;
  std::array<uint32_t, 2> words{uint32_t(latched), uint32_t(latched >> 32)};
  std::vector<Access> trace{};

  unsigned word(uint64_t at) const
  {
    CHECK(at == offset || at == offset + 4);
    return at == offset ? 0 : 1;
  }

  uint32_t load(uint64_t at)
  {
    const uint32_t value = words[word(at)];
    trace.push_back({'r', at, value});
    return value;
  }

  void store(uint64_t at, uint32_t value)
  {
    const unsigned index = word(at);
    trace.push_back({'w', at, value});
    words[index] = value;
    if (index == (latchOnLow ? 0u : 1u)) latched = uint64_t(words[1]) << 32 | words[0];
  }

  void barrier() { trace.push_back({'b'}); }
};

// xHCI §5.1 defines low-first split address writes. DWC3 uses the high-first quirk.
void writeLowFirst(SplitRegister &r, uint64_t offset, uint64_t value)
{
  r.store(offset, uint32_t(value));
  r.barrier();
  r.store(offset + 4, uint32_t(value >> 32));
  r.barrier();
}

void testSplitMmio()
{
  const std::array values{(kBus + kLayout.cmd) | kCrcrRcs, kBus + kLayout.dcbaa,
                          kBus + kLayout.erst, (kBus + kLayout.evt + kTrbBytes) | kErdpEhb};
  constexpr uint64_t old = 0x0000000376540000;
  for (size_t i = 0; i < kPointerRegs.size(); ++i) {
    const uint64_t offset = kPointerRegs.at(i);
    const uint64_t value = values.at(i);
    const Access low{'w', offset, uint32_t(value)}, high{'w', offset + 4, uint32_t(value >> 32)}, barrier{'b'};
    SplitRegister dwc3{offset, true, old}, standard{offset, false, old};

    pianoxhci::write64(dwc3, offset, value);
    CHECK((dwc3.trace == std::vector{high, barrier, low, barrier}));
    CHECK(dwc3.latched == value);
    dwc3.trace.clear();
    CHECK(pianoxhci::read64(dwc3, offset) == value);
    CHECK((dwc3.trace == std::vector<Access>{{'r', offset, low.value}, {'r', offset + 4, high.value}}));

    writeLowFirst(standard, offset, value);
    CHECK((standard.trace == std::vector{low, barrier, high, barrier}));
    CHECK(standard.latched == value);

    SplitRegister reversedDwc3{offset, true, old}, reversedStandard{offset, false, old};
    writeLowFirst(reversedDwc3, offset, value);
    pianoxhci::write64(reversedStandard, offset, value);
    CHECK(reversedDwc3.latched == ((old & 0xffffffff00000000) | low.value));
    CHECK(reversedStandard.latched == ((value & 0xffffffff00000000) | uint32_t(old)));
    CHECK(reversedDwc3.latched != value && reversedStandard.latched != value);

    // Reusing an address below 4 GiB must also clear the old upper word.
    pianoxhci::write64(dwc3, offset, low.value);
    CHECK(dwc3.latched == low.value);
  }
}

struct SplitCounter {
  static constexpr uint64_t offset = 0x100;  // synthetic register pair
  uint64_t count;
  uint64_t increment = 1;
  unsigned reads = 0;

  uint32_t load(uint64_t at)
  {
    CHECK(at == offset || at == offset + 4);
    const uint32_t value = at == offset ? uint32_t(count) : uint32_t(count >> 32);
    count += increment;
    ++reads;
    return value;
  }
};

std::optional<uint64_t> sampleCounter(SplitCounter &counter)
{
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    const uint32_t high = counter.load(SplitCounter::offset + 4);
    const uint32_t low = counter.load(SplitCounter::offset);
    if (high == counter.load(SplitCounter::offset + 4)) return uint64_t(high) << 32 | low;
  }
  return std::nullopt;
}

// MFINDEX itself is a 14-bit counter in one dword. Extend successive samples
// in software, assuming fewer than 2^14 microframes elapse between samples.
uint64_t extendMicroframe(uint64_t previous, uint32_t mfindex)
{
  return previous + ((mfindex - uint32_t(previous)) & 0x3fff);
}

void testCounters()
{
  SplitCounter raw{0x00000007ffffffff};
  CHECK(pianoxhci::read64(raw, SplitCounter::offset) == 0x00000008ffffffff);
  SplitCounter stable{0x0000000712345678, 0};
  CHECK(sampleCounter(stable) == stable.count && stable.reads == 3);
  SplitCounter rollover{0x00000007fffffffe};
  CHECK(sampleCounter(rollover) == 0x0000000800000002 && rollover.reads == 6);
  SplitCounter fullRollover{UINT64_MAX};
  CHECK(sampleCounter(fullRollover) == 3 && fullRollover.reads == 6);
  SplitCounter changing{0, uint64_t(1) << 32};
  CHECK(!sampleCounter(changing) && changing.reads == 9);

  uint64_t microframes = 0x00000004fffffffe;
  CHECK(extendMicroframe(microframes, 0x3ffe) == microframes);
  microframes = extendMicroframe(microframes, 0x3fff);
  CHECK(microframes == 0x00000004ffffffff);
  microframes = extendMicroframe(microframes, 0);
  CHECK(microframes == 0x0000000500000000);
  CHECK(extendMicroframe(microframes, 1) == 0x0000000500000001);
}

bool fitsWindow(uint64_t address, uint32_t bytes)
{
  return bytes && bytes <= kBoundary - address % kBoundary;
}

bool validObject(uint64_t address, uint32_t alignment, uint32_t bytes)
{
  return address % alignment == 0 && fitsWindow(address, bytes);
}

struct DmaSpan {
  uint64_t address;
  uint32_t bytes;
  bool operator==(const DmaSpan &) const = default;
};

std::optional<std::vector<DmaSpan>> splitScatterGather(std::initializer_list<DmaSpan> segments)
{
  std::vector<DmaSpan> chunks;
  for (auto [address, remaining] : segments) {
    if (remaining && remaining - 1 > UINT64_MAX - address) return std::nullopt;
    while (remaining) {
      const uint32_t bytes = std::min(remaining, kBoundary - uint32_t(address % kBoundary));
      chunks.push_back({address, bytes});
      remaining -= bytes;
      if (remaining) address += bytes;
    }
  }
  return chunks;
}

void testDmaAlignment()
{
  CHECK(validObject(kBus + 0xfff0, kTrbBytes, kTrbBytes));
  CHECK(!validObject(kBus + kTrbBytes + 1, kTrbBytes, kTrbBytes));
  CHECK(!validObject(kBus + 0xfff0, kTrbBytes, 2 * kTrbBytes));
  for (const auto object : {DmaSpan{kLayout.dcbaa, 8 * (kLayout.slots + 1)},
                           DmaSpan{kLayout.cmd, kTrbBytes * kLayout.cmdTrbs},
                           DmaSpan{kLayout.evt, kTrbBytes * kLayout.evtTrbs},
                           DmaSpan{kLayout.erst, kTrbBytes}, DmaSpan{0x400, kContextAlign}}) {
    const uint64_t address = kBus + object.address;
    CHECK(validObject(address, kContextAlign, object.bytes));
    CHECK(!validObject(address + kTrbBytes, kContextAlign, object.bytes));
  }
  CHECK(validObject(kBus + 0xffc0, kContextAlign, kContextAlign));
  CHECK(!validObject(kBus + 0xffc0, kContextAlign, kContextAlign + 1));

  const auto chunks = splitScatterGather({{0xfffffff0, 0x20030}, {kBus + 3, 7}, {0, 0}, {UINT64_MAX - 7, 8}});
  const std::vector<DmaSpan> expected{{0xfffffff0, 0x10}, {0x100000000, 0x10000}, {0x100010000, 0x10000},
                                      {0x100020000, 0x20}, {kBus + 3, 7}, {UINT64_MAX - 7, 8}};
  CHECK(chunks == expected);
  for (const auto &chunk : *chunks) CHECK(fitsWindow(chunk.address, chunk.bytes));
  CHECK(splitScatterGather({{kBus, kBoundary}}) == (std::vector<DmaSpan>{{kBus, kBoundary}}));
  CHECK(splitScatterGather({{0, 0}}) == std::vector<DmaSpan>{});
  CHECK(!splitScatterGather({{kBus, 16}, {UINT64_MAX - 7, 9}}));
}

enum class Fault { none, preCnr, halt, reset, postCnr, stalePointer };

struct Controller {
  static constexpr uint32_t never = UINT32_MAX;
  Fault fault;
  ControllerLoan loan;
  uint32_t command = 0, status = kUsbStsHalted;
  uint32_t elapsed = 0, haltAt = never, resetAt = never, readyAt = never;
  unsigned haltWrites = 0, resetWrites = 0, refusedResets = 0;
  unsigned doorbells = 0, rejectedDoorbells = 0, memoryWrites = 0;
  std::map<uint64_t, uint32_t> registers;
  std::vector<uint32_t> dma = std::vector<uint32_t>(kLayout.bytes / 4);
  uint32_t memoryBegin = 0, memoryEnd = kLayout.bytes;

  explicit Controller(Fault injected = Fault::none) : fault(injected) {}

  uint32_t &word(uint32_t offset)
  {
    CHECK(offset % 4 == 0 && offset >= memoryBegin && offset < memoryEnd);
    return dma.at(offset / 4);
  }
  uint32_t memRd(uint32_t offset) { return word(offset); }
  void memWr(uint32_t offset, uint32_t value) { word(offset) = value; ++memoryWrites; }
  void memBarrier() {}
  bool glueClocked() const { return true; }

  uint32_t rd(uint64_t offset) const
  {
    if (offset == kAt.op + kOpUsbCmd) return command;
    if (offset == kAt.op + kOpUsbSts) return status;
    if (offset == kAt.op + kOpPageSize) return 1;
    const auto it = registers.find(offset);
    const uint32_t value = it == registers.end() ? 0 : it->second;
    if (offset == kCrcr) return value & 0x3f;  // CRCR's pointer reads as zero.
    if (offset == kCrcr + 4) return 0;
    return value;
  }

  bool dmaIdle() const
  {
    return std::none_of(kPointerRegs.begin(), kPointerRegs.end(), [&](uint64_t offset) {
      const uint32_t lowFlags = offset == kErdp ? 0xf : 0x3f;
      return (registers.at(offset) & ~lowFlags) || registers.at(offset + 4);
    });
  }

  void ringDoorbell()
  {
    if (loan.phase == LoanPhase::host && (command & kUsbCmdRun) && !(status & (kUsbStsHalted | kUsbStsCnr))) {
      ++doorbells;
    } else {
      ++rejectedDoorbells;
    }
  }

  void wr(uint64_t offset, uint32_t value)
  {
    if (offset == kAt.db) {
      ringDoorbell();
      return;
    }
    if (offset != kAt.op + kOpUsbCmd) {
      registers[offset] = value;
      return;
    }
    if (value & kUsbCmdHcrst) {
      if ((command & kUsbCmdRun) || (status & (kUsbStsHalted | kUsbStsCnr)) != kUsbStsHalted) {
        ++refusedResets;
        return;
      }
      ++resetWrites;
      command = value;
      status |= kUsbStsCnr;
      if (fault != Fault::reset) resetAt = elapsed + 2000;
      if (fault != Fault::reset && fault != Fault::postCnr) readyAt = elapsed + 4000;
      ringDoorbell();   // A late submission while HCRST and CNR are asserted.
      return;
    }
    const bool stopping = (command & kUsbCmdRun) && !(value & kUsbCmdRun);
    command = value;
    if (command & kUsbCmdRun) status &= ~kUsbStsHalted;
    if (stopping) {
      ++haltWrites;
      if (fault != Fault::halt) haltAt = elapsed + 200;
      CHECK(!(status & kUsbStsHalted));
      ringDoorbell();   // Inject between clearing RS and observing HCH.
    }
  }

  void delayUs(uint32_t us)
  {
    elapsed += us;
    if (haltAt <= elapsed) {
      status |= kUsbStsHalted;
      haltAt = never;
    }
    if (resetAt <= elapsed) {
      command &= ~kUsbCmdHcrst;
      for (const auto offset : kPointerRegs) {
        if (fault != Fault::stalePointer || offset != kCrcr) {
          registers.at(offset) = 0;
          registers.at(offset + 4) = 0;
        }
      }
      resetAt = never;
    }
    if (readyAt <= elapsed) {
      status &= ~kUsbStsCnr;
      readyAt = never;
    }
  }
  void sleepMs(uint32_t ms) { delayUs(ms * 1000); }
};

void startController(Controller &c, Rings &rings)
{
  CHECK(c.loan.begin(0, 60'000, false));
  const auto init = xhciInit(c, kCaps, rings);
  CHECK(init.ok && init.dcbaap == kBus + kLayout.dcbaa && init.erstba == kBus + kLayout.erst);
  CHECK(init.crcr == kCrcrRcs && init.erdp == kBus + kLayout.evt);
  CHECK(xhciRun(c, kCaps).ok);
  c.loan.started();
}

void testRingWrap()
{
  Controller c;
  Rings rings{kLayout, kBus + kContextAlign};
  CHECK(!xhciInit(c, kCaps, rings).ok);
  CHECK(c.memoryWrites == 0 && c.registers.empty());
  rings.bus = kBus;
  startController(c, rings);
  CHECK(c.memRd(kLayout.erst) == uint32_t(kBus + kLayout.evt));
  CHECK(c.memRd(kLayout.erst + 4) == uint32_t((kBus + kLayout.evt) >> 32));
  CHECK(c.memRd(kLayout.erst + 8) == kLayout.evtTrbs && c.memRd(kLayout.erst + 12) == 0);

  c.memoryBegin = kLayout.cmd;
  c.memoryEnd = kLayout.cmd + kLayout.cmdTrbs * kTrbBytes;
  const uint32_t link = c.memoryEnd - kTrbBytes;
  CHECK(c.memRd(link) == uint32_t(kBus + kLayout.cmd));
  CHECK(c.memRd(link + 4) == uint32_t((kBus + kLayout.cmd) >> 32));
  const unsigned commandsPerLap = kLayout.cmdTrbs - 1;
  for (const bool cycle : {true, false}) {
    for (unsigned i = 0; i < commandsPerLap; ++i) {
      CHECK(rings.cmdEnq == i && rings.cmdCycle == cycle);
      const auto offset = submitCommand(c, kCaps, rings, 0, 0, 0, trbTypeField(kTrbNoopCmd));
      CHECK(offset == kLayout.cmd + i * kTrbBytes);
      CHECK(c.memRd(offset + 12) == (trbTypeField(kTrbNoopCmd) | (cycle ? kTrbCycle : 0)));
    }
    CHECK(rings.cmdEnq == 0 && rings.cmdCycle != cycle);
    CHECK(c.memRd(link + 12) == (trbTypeField(kTrbLink) | kTrbToggle | (cycle ? kTrbCycle : 0)));
  }
  CHECK(c.doorbells == 2 * commandsPerLap && c.rejectedDoorbells == 0);

  c.memoryBegin = kLayout.evt;
  c.memoryEnd = kLayout.evt + kLayout.evtTrbs * kTrbBytes;
  Event event;
  CHECK(!nextEvent(c, rings, event) && rings.evtDeq == 0 && rings.evtCycle);
  for (const bool cycle : {true, false}) {
    for (unsigned i = 0; i < kLayout.evtTrbs; ++i) {
      CHECK(rings.evtDeq == i && rings.evtCycle == cycle);
      const uint32_t offset = kLayout.evt + i * kTrbBytes;
      const uint64_t pointer = kBus + kLayout.cmd + (i % commandsPerLap) * kTrbBytes;
      c.memWr(offset, uint32_t(pointer));
      c.memWr(offset + 4, uint32_t(pointer >> 32));
      c.memWr(offset + 8, kCcSuccess << 24);
      c.memWr(offset + 12, trbTypeField(kTrbCmdCompletion) | (cycle ? kTrbCycle : 0));
      CHECK(nextEvent(c, rings, event) && event.type() == kTrbCmdCompletion && event.code() == kCcSuccess);
      CHECK(event.pointer() == pointer);
      if (i == 0) {
        ackEvents(c, kCaps, rings);
        CHECK(rd64(c, kErdp) == ((kBus + kLayout.evt + kTrbBytes) | kErdpEhb));
      }
    }
    CHECK(!nextEvent(c, rings, event) && rings.evtDeq == 0 && rings.evtCycle != cycle);
    ackEvents(c, kCaps, rings);
    CHECK(rd64(c, kErdp) == ((kBus + kLayout.evt) | kErdpEhb));
  }
}

// Fault injection and the DMA/doorbell gates are model policy. The actual stop
// polling and the loan phase transitions come from XhciHost.h and ControllerLoan.h.
void testLoanReturn()
{
  struct Scenario {
    Fault fault;
    uint32_t elapsed;
    unsigned halts, resets;
    bool stopped, dmaIdle;
  };
  const Scenario cases[] = {
    {Fault::none,          6200, 1, 1, true,  true},
    {Fault::preCnr,    1000000, 0, 0, false, false},
    {Fault::halt,       100000, 1, 0, false, false},
    {Fault::reset,     1001200, 1, 1, false, false},
    {Fault::postCnr,   1002200, 1, 1, false, true},
    {Fault::stalePointer, 6200, 1, 1, true,  false},
  };
  for (const auto &s : cases) {
    Controller c(s.fault);
    Rings rings{kLayout, kBus};
    startController(c, rings);
    CHECK(!c.dmaIdle());

    const auto pointers = c.registers;
    c.wr(kAt.op + kOpUsbCmd, kUsbCmdHcrst);
    CHECK(c.refusedResets == 1 && c.resetWrites == 0 && c.command == kUsbCmdRun);
    CHECK(!(c.status & kUsbStsHalted) && c.registers == pointers);
    c.wr(kAt.db, 0);
    CHECK(c.doorbells == 1 && c.rejectedDoorbells == 0);

    CHECK(c.loan.returning());
    c.wr(kAt.db, 0);
    if (s.fault == Fault::none || s.fault == Fault::stalePointer || s.fault == Fault::preCnr) {
      c.status |= kUsbStsCnr;
      if (s.fault != Fault::preCnr) c.readyAt = 2000;
    }
    const auto stopped = xhciStop(c, kCaps);
    CHECK(stopped.ok == s.stopped && c.elapsed == s.elapsed);
    CHECK(c.haltWrites == s.halts && c.resetWrites == s.resets && c.refusedResets == 1);
    CHECK(c.dmaIdle() == s.dmaIdle);
    if (stopped.ok) {
      CHECK(c.command == 0 && (c.status & (kUsbStsHalted | kUsbStsCnr)) == kUsbStsHalted);
    }

    const bool canReclaim = stopped.ok && c.dmaIdle();
    c.loan.returned(stopped.ok, canReclaim, canReclaim);
    CHECK(c.loan.phase == (s.fault == Fault::none ? LoanPhase::device : LoanPhase::held));
    c.wr(kAt.db, 0);
    CHECK(c.doorbells == 1 && c.rejectedDoorbells == 2 + s.halts + s.resets);
  }
}

}  // namespace

int main()
{
  testSplitMmio();
  testCounters();
  testDmaAlignment();
  testRingWrap();
  testLoanReturn();
  std::puts("ok: xhci_dma_ring_model_host");
}
