#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check that the survey and slice workers receive initialized boot options."""
from pathlib import Path

package = Path(__file__).resolve().parents[1]
driver = (package / 'PianoDpu.cpp').read_text()
slices = (package / 'DpuSlices.cpp').read_text()
start = driver.split('bool PianoDpu::start(IOService *provider)', 1)[1].split('IOReturn PianoDpu::', 1)[0]
assert start.index('dpuSlicesReadArgs();') < start.index('kernel_thread_start(') < start.index('dpuSlicesStart(this);')
wanted = slices.split('bool dpuSlicesWanted()\n{', 1)[1].split('}', 1)[0]
assert 'readSliceArgs(' not in wanted
print('PianoDpu startup: boot options initialized before either worker starts')
