#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check kernel startup wiring and the host trace row's publication bounds."""
from pathlib import Path
import re

package = Path(__file__).resolve().parents[1]
driver = (package / 'PianoDpu.cpp').read_text()
slices = (package / 'DpuSlices.cpp').read_text()
start = driver.split('bool PianoDpu::start(IOService *provider)', 1)[1].split('IOReturn PianoDpu::', 1)[0]
assert start.index('dpuSlicesReadArgs();') < start.index('kernel_thread_start(') < start.index('dpuSlicesStart(this);')
wanted = slices.split('bool dpuSlicesWanted()\n{', 1)[1].split('}', 1)[0]
assert 'readSliceArgs(' not in wanted
read_args = slices.split('void readSliceArgs()\n{', 1)[1].split('}', 1)[0]
# The sanitized interaction suite tests the startup policy itself.
assert 'gA = readStartupArgs(PE_parse_boot_argn, PE_parse_boot_arg_str);' in read_args
assert 'return gA.wanted();' in wanted
panel_start = slices.split('void panelStart()\n{', 1)[1].split('\n}', 1)[0]
panel_wake = slices.split('bool panelWake()\n{', 1)[1].split('\n}', 1)[0]
assert 'gPanelOptions.videoOnly = gA.panelVideoOnly;' in panel_start
assert 'panel::panelOn(gIo, gPath, gPanelOptions, gEnginesStopped, gPanelFetch, trace)' in panel_wake
assert 'r.scanoutRestored && reread("the wake")' in panel_wake

publish = slices.split('void publishPanelTrace()\n{', 1)[1].split('\n}', 1)[0]
rows = publish.split('for (unsigned i = 0; i < count; ++i)', 1)[1]
assert 'if (panel::isHostTraceStep(s.step))' in rows
assert 'a.ctrl, b.ctrl, a.status, b.status, a.fifo, b.fifo, a.lane, b.lane, a.clk, b.clk' in rows
row_format = re.search(r'"(m5 %u host %s row=.*)"', rows).group(1).replace('%llu', '%d')
line_capacity = int(re.search(r'char line\[(\d+)\]', publish).group(1))
u32_max, u64_max = (1 << 32) - 1, (1 << 64) - 1
row = row_format % (u32_max, 'after-enable', u32_max, u64_max, *((u32_max,) * 10))
assert len(row) + 1 <= line_capacity  # One byte for NUL in the buffer or newline in OSData.
assert 'OSData::withCapacity((count + 6) * sizeof line)' in publish
assert 'setProperty("piano-dpu-panel-trace", dump)' in publish
print('PianoDpu startup: boot options initialized before either worker starts and host trace rows fit')
