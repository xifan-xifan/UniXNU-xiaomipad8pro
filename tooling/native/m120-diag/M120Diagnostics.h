// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace m120diag {

// Existing native-gop-frameinfo-audit argument and live feature gates. Check
// these before taking any samples. This header owns no clocks, MMIO or logging.
struct OptIn {
    uint32_t auditArg = 0;
    bool pipeEnabled = false, frameInfoEnabled = false;
    constexpr bool pipe() const { return auditArg == 1 && pipeEnabled; }
    constexpr bool physical() const { return auditArg == 1 && frameInfoEnabled; }
};

struct QueueSample {
    uint64_t ticks = 0;
    uint32_t current = 0, created = 0, pending = 0, head = 0;
    uint32_t layers = 0, ready = 0, drops = 0;
    bool mainSurface = false;
};

enum class Preparation { Unobserved, PowerOff, Evaluated };
enum class Take { Unobserved, Used, Fallback };
enum class Ticket { Empty, Pending, Ready, Failed };
enum class TicketFailure { Unknown, Capture, Poll, Deadline };

struct PipeRecord {
    QueueSample prepare, program; // program sample precedes dequeue of the programmed request
    uint32_t swap = 0;
    uint64_t worker = 0;
    Preparation preparation = Preparation::Unobserved;
    Take take = Take::Unobserved;
    Ticket ticket = Ticket::Empty;
    TicketFailure failure = TicketFailure::Unknown;
    bool keyMatches = false; // result of existing full request/surface/swap key comparison
};

// These observations can overlap. They mirror the captured ABI's admission
// inputs, including the exact layer mask and the exception for drop bit 3.
enum HeadRejection : uint32_t {
    Layers = 1u << 0, NotReady = 1u << 1, Drops = 1u << 2, NoSurface = 1u << 3,
};

constexpr uint32_t headRejections(const QueueSample &sample)
{
    if (!sample.head) return 0;
    return (sample.layers != 1 ? Layers : 0u) |
           (!(sample.ready & 1) ? NotReady : 0u) |
           ((sample.drops & ~8u) ? Drops : 0u) |
           (!sample.mainSurface ? NoSurface : 0u);
}

enum class Fallback {
    Unobserved, Used, NoPreparation, PowerOff, EmptyHead, HeadReplaced,
    IneligibleHead, KeyMismatch, CaptureFailed, PollFailed, Deadline,
    FailedUnknown, NoTicket, Inconsistent, Count
};

// One bucket per recorded take. Head replacement takes precedence over a
// rejection of the earlier head. The independent rejection masks retain both.
constexpr Fallback fallback(const PipeRecord &record)
{
    if (record.take == Take::Unobserved) return Fallback::Unobserved;
    const bool usable = record.keyMatches && (record.ticket == Ticket::Pending || record.ticket == Ticket::Ready);
    if ((record.take == Take::Used) != usable) return Fallback::Inconsistent;
    if (record.take == Take::Used) return Fallback::Used;
    if (record.preparation == Preparation::Unobserved) return Fallback::NoPreparation;
    if (record.preparation == Preparation::PowerOff) return Fallback::PowerOff;
    if (!record.prepare.head) return Fallback::EmptyHead;
    if (record.prepare.head != record.swap) return Fallback::HeadReplaced;
    if (headRejections(record.prepare)) return Fallback::IneligibleHead;
    if (record.ticket == Ticket::Empty) return Fallback::NoTicket;
    if (!record.keyMatches) return Fallback::KeyMismatch;
    if (record.ticket != Ticket::Failed) return Fallback::Inconsistent;
    switch (record.failure) {
    case TicketFailure::Capture: return Fallback::CaptureFailed;
    case TicketFailure::Poll: return Fallback::PollFailed;
    case TicketFailure::Deadline: return Fallback::Deadline;
    case TicketFailure::Unknown: return Fallback::FailedUnknown;
    }
    return Fallback::Inconsistent;
}

struct CounterSample {
    uint32_t frame = 0, line = 0;
};

struct FlushRecord {
    uint64_t generation = 0, worker = 0, selectedIrq = 0;
    uint64_t armed = 0, enabled = 0, flushed = 0, latched = 0;
    uint64_t disabled = 0, disarmed = 0, end = 0;
    uint32_t mask = 0, latchCtl = 0;
    CounterSample intf1, intf2;
};

struct IrqRecord {
    uint64_t generation = 0, armedFlush = 0, ticks = 0;
    uint32_t status = 0, ctl = 0;
    uint32_t before1 = 0, before2 = 0;
    CounterSample after1, after2;
};

enum class Advance { Missing, Unstable, Same, Next, Other, Count };

constexpr Advance advance(uint32_t preFlush, uint32_t before, uint32_t after)
{
    if (before != after) return Advance::Unstable;
    const uint32_t delta = after - preFlush;
    return delta == 0 ? Advance::Same : delta == 1 ? Advance::Next : Advance::Other;
}

enum IrqObservation : uint32_t {
    NoIrq = 1u << 0, ArmMismatch = 1u << 1, NoIntf1Vsync = 1u << 2,
    BeforeFlush = 1u << 3, AfterEnd = 1u << 4, IrqCtlPending = 1u << 5,
    LatchCtlPending = 1u << 6, IncompleteBounds = 1u << 7,
    AfterDisable = 1u << 8, AfterDisarm = 1u << 9,
};

struct CounterPair {
    Advance intf1 = Advance::Missing, intf2 = Advance::Missing;
    uint32_t observations = 0;
};

constexpr uint32_t kIntf1Vsync = 1u << 27;

// Report the raw deltas and independently flag the provenance and timing.
// A matching counter delta carries no permission to publish a timestamp.
constexpr CounterPair compare(const FlushRecord &flush, const IrqRecord *irq)
{
    CounterPair result;
    if (!flush.armed || !flush.mask || !flush.flushed || flush.end < flush.flushed)
        result.observations |= IncompleteBounds;
    if (flush.latched && (flush.latchCtl & flush.mask)) result.observations |= LatchCtlPending;
    if (!irq || !flush.selectedIrq || irq->generation != flush.selectedIrq) {
        result.observations |= NoIrq;
        return result;
    }
    result.intf1 = advance(flush.intf1.frame, irq->before1, irq->after1.frame);
    result.intf2 = advance(flush.intf2.frame, irq->before2, irq->after2.frame);
    if (!flush.generation || irq->armedFlush != flush.generation) result.observations |= ArmMismatch;
    if (!(irq->status & kIntf1Vsync)) result.observations |= NoIntf1Vsync;
    if (irq->ticks <= flush.flushed) result.observations |= BeforeFlush;
    if (flush.end && irq->ticks > flush.end) result.observations |= AfterEnd;
    if (irq->ctl & flush.mask) result.observations |= IrqCtlPending;
    if (flush.disabled && irq->ticks > flush.disabled) result.observations |= AfterDisable;
    if (flush.disarmed && irq->ticks > flush.disarmed) result.observations |= AfterDisarm;
    return result;
}

// Caller-owned counters. Keep each instance on the corresponding serialized
// observation path (gate or worker) and take snapshots under the same ownership.
struct Counters {
    uint64_t take[static_cast<unsigned>(Fallback::Count)] = {};
    uint64_t prepareRejected[4] = {}, programRejected[4] = {};
    uint64_t intf1[static_cast<unsigned>(Advance::Count)] = {};
    uint64_t intf2[static_cast<unsigned>(Advance::Count)] = {};
    uint64_t irqObservations[10] = {};

    void observe(const OptIn &opt, const PipeRecord &record)
    {
        if (!opt.pipe()) return;
        ++take[static_cast<unsigned>(fallback(record))];
        const uint32_t prepared = record.preparation == Preparation::Evaluated ? headRejections(record.prepare) : 0;
        const uint32_t programmed = headRejections(record.program);
        for (unsigned bit = 0; bit < 4; ++bit) {
            prepareRejected[bit] += bool(prepared & (1u << bit));
            programRejected[bit] += bool(programmed & (1u << bit));
        }
    }

    void observe(const OptIn &opt, const FlushRecord &flush, const IrqRecord *irq)
    {
        if (!opt.physical()) return;
        const auto pair = compare(flush, irq);
        ++intf1[static_cast<unsigned>(pair.intf1)];
        ++intf2[static_cast<unsigned>(pair.intf2)];
        for (unsigned bit = 0; bit < 10; ++bit)
            irqObservations[bit] += bool(pair.observations & (1u << bit));
    }
};

} // namespace m120diag
