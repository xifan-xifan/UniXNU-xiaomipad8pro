// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "M120Diagnostics.h"
#include <cassert>
#include <initializer_list>

using namespace m120diag;

static void testPipe()
{
    const OptIn on{1, true, true};
    Counters counts;
    PipeRecord record;
    const auto expect = [&](Fallback bucket) {
        assert(fallback(record) == bucket);
        counts.observe(on, record);
    };
    expect(Fallback::Unobserved);
    record.ticket = Ticket::Pending;
    record.keyMatches = true;
    record.take = Take::Used;
    expect(Fallback::Used);

    record.take = Take::Fallback;
    record.swap = 41;
    record.ticket = Ticket::Empty;
    record.keyMatches = false;
    expect(Fallback::NoPreparation);
    record.preparation = Preparation::PowerOff;
    expect(Fallback::PowerOff);
    record.preparation = Preparation::Evaluated;
    expect(Fallback::EmptyHead);

    record.prepare.head = 40;
    record.prepare.layers = 0x10001;
    record.prepare.ready = 2;
    record.prepare.drops = 9;
    record.program = {10, 39, 2, 1, 41, 1, 1, 8, true};
    assert(headRejections(record.prepare) == (Layers | NotReady | Drops | NoSurface));
    assert(headRejections(record.program) == 0);
    expect(Fallback::HeadReplaced);
    record.prepare.head = 41;
    expect(Fallback::IneligibleHead);
    for (unsigned i = 0; i < 4; ++i)
        assert(counts.prepareRejected[i] == 2 && counts.programRejected[i] == 0);
    record.prepare = record.program;
    assert(headRejections({}) == 0);
    expect(Fallback::NoTicket);
    record.ticket = Ticket::Ready;
    expect(Fallback::KeyMismatch);

    record.keyMatches = true;
    record.ticket = Ticket::Failed;
    expect(Fallback::FailedUnknown);
    record.failure = TicketFailure::Capture;
    expect(Fallback::CaptureFailed);
    record.failure = TicketFailure::Poll;
    expect(Fallback::PollFailed);
    record.failure = TicketFailure::Deadline;
    expect(Fallback::Deadline);
    record.ticket = Ticket::Ready;
    expect(Fallback::Inconsistent);
    for (uint64_t count : counts.take) assert(count == 1);

    for (const OptIn off : {OptIn{}, OptIn{0, true, true}, OptIn{2, true, true}, OptIn{1, false, true}})
        counts.observe(off, record);
    for (uint64_t count : counts.take) assert(count == 1);
}

static void testPhysical()
{
    const OptIn on{1, true, true};
    Counters counts;
    FlushRecord flush{5, 20, 9, 100, 101, 102, 107, 108, 109, 110, 3, 0, {30, 105}, {UINT32_MAX, 106}};
    IrqRecord irq{9, 5, 104, kIntf1Vsync, 0, 30, 0, {30, 3}, {0, 4}};
    const auto expect = [&](Advance intf1, Advance intf2, uint32_t flags, const IrqRecord *sample) {
        const auto pair = compare(flush, sample);
        assert(pair.intf1 == intf1 && pair.intf2 == intf2 && pair.observations == flags);
        counts.observe(on, flush, sample);
    };
    expect(Advance::Same, Advance::Next, 0, &irq);
    assert(flush.intf1.line == 105 && irq.after1.line == 3 && flush.worker == 20);
    assert(advance(42, 43, 43) == Advance::Next);
    assert(advance(41, 40, 40) == Advance::Other);
    assert(advance(UINT32_MAX, UINT32_MAX, 0) == Advance::Unstable);
    expect(Advance::Missing, Advance::Missing, NoIrq, nullptr);
    irq.generation = 8;
    expect(Advance::Missing, Advance::Missing, NoIrq, &irq);

    irq.generation = 9;
    irq.armedFlush = 4;
    irq.ticks = 102;
    irq.status = 0;
    irq.ctl = 1;
    irq.after1.frame = 31;
    irq.before2 = irq.after2.frame = 7;
    expect(Advance::Unstable, Advance::Other, ArmMismatch | NoIntf1Vsync | BeforeFlush | IrqCtlPending, &irq);

    irq.armedFlush = 5;
    irq.ticks = 111;
    irq.status = kIntf1Vsync;
    irq.ctl = 4; // an unrelated CTL bit
    irq.before1 = irq.after1.frame;
    irq.before2 = 8;
    flush.latchCtl = 2;
    expect(Advance::Next, Advance::Unstable, AfterEnd | LatchCtlPending | AfterDisable | AfterDisarm, &irq);
    flush.armed = flush.end = flush.selectedIrq = 0;
    expect(Advance::Missing, Advance::Missing, IncompleteBounds | NoIrq | LatchCtlPending, &irq);
    assert(counts.intf1[static_cast<unsigned>(Advance::Same)] == 1);
    assert(counts.intf2[static_cast<unsigned>(Advance::Missing)] == 3);
    assert(counts.irqObservations[0] == 3 && counts.irqObservations[6] == 2);
    const OptIn off{1, true, false};
    counts.observe(off, flush, &irq);
    counts.observe({}, flush, &irq);
    assert(counts.irqObservations[0] == 3);
}

int main()
{
    static_assert(!OptIn{}.pipe() && !OptIn{}.physical());
    testPipe();
    testPhysical();
}
