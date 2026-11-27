#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the RX thread's calls into the real IOBasicOutputQueue boundary."""
import re
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'PianoUsbNet.cpp').read_text(encoding='utf-8')

# A synchronous restart can drain indefinitely on the RX thread. Reopening
# after a flush must also start at zero capacity so start() has no work to drain.
assert re.search(r'oqService\(net->fQueue,\s*IOBasicOutputQueue::kServiceAsync\);', source)
start = source.split('void PianoUsbNet::startTxQueue()', 1)[1].split('}', 1)[0]
assert re.search(r'oqStart\(fQueue\);\s*oqSetCapacity\(fQueue, kOutputPackets\);', start)
reset = source.split('void PianoUsbNet::resetTxQueue()', 1)[1].split('IOReturn PianoUsbNet::serviceTxAction', 1)[0]
assert re.search(r'oqSetCapacity\(fQueue, 0\);\s*oqStop\(fQueue\);\s*oqFlush\(fQueue\);', reset)
assert reset.index('oqFlush(fQueue)') < reset.index('gTxGate.resetDone()')
print('PianoUsbNet TX: asynchronous service and queue admission ordered around start and flush')

# A stalled-queue reset can hold the RX thread off the byte ring across a
# down/up pair. Clear buffered RX at each boundary and serialize admission
# with the clear, including an OUT completion racing interface disable.
assert re.search(r'gToHost\.clear\(\);[^\n]*\s+gFromHost\.clear\(\);', source)
for start, end in [('IOReturn disable(', '// The thread that starts us'), ('void link(', 'void status(')]:
    boundary = source.split(start, 1)[1].split(end, 1)[0]
    assert boundary.index('IOLockLock(gNetLock)') < boundary.index('gFromHost.clear()') < boundary.index('IOLockUnlock(gNetLock)')
rx = source.split('bool rxFrame(', 1)[1].split('}', 1)[0]
assert re.search(r'IOLockLock\(gNetLock\);\s+const bool ok = gLinkUp && gEnabled && gFromHost\.put\(src, length\);', rx)
print('PianoUsbNet RX: link-down and disable flush buffered frames under the admission lock')
