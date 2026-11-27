#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Tablet side of RAM16G milestone M0 (read-only): one snapshot of the memory state of the INSTALLED boot, on stdout.
# Nothing is written, nothing is tuned: sysctl reads, vm_stat, memory_pressure WITHOUT options (it only reports;
# -l/-p would apply pressure and are never used), the GPU paged-VM and tier sysctls, and the NativeAdrenoGpu window keys.
#   ram-m0-tab.sh PHASE        PHASE = idle | apps | apps10 (a label only; the Mac side decides when to take it)
# Each section starts "== NAME" and the output ends "M0_DONE"; a missing tool or sysctl prints "MISSING <what>".
set -uo pipefail
PHASE=${1:-idle}
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C
sec() { echo "== $1"; }
sc() { local v; if v=$(sysctl -n "$1" 2>/dev/null); then echo "$1=$v"; else echo "MISSING sysctl $1"; fi; }
sec meta
echo "phase=$PHASE"
echo "date=$(date +%Y-%m-%dT%H:%M:%S%z)"
sc kern.bootsessionuuid
sc kern.osversion
uptime
sec size
for k in hw.memsize hw.memsize_usable hw.physmem hw.pagesize vm.pagesize kern.memorystatus_level kern.memorystatus_vm_pressure_level \
         vm.page_free_count vm.page_speculative_count vm.page_purgeable_count vm.memory_pressure vm.swapusage \
         vm.compressor_mode vm.compressor_bytes_used vm.compressor_compressed_bytes vm.compressor_pool_size \
         vm.compressor_segment_pages_compressed vm.compressor_available vm.vm_page_free_target vm.page_free_wanted; do
  sc "$k"
done
sec vm_stat
vm_stat 2>&1 || echo "MISSING vm_stat"
sec memory_pressure
memory_pressure 2>&1 | awk 'NR <= 40' || echo "MISSING memory_pressure"
sec piano
for k in machdep.piano_gpu_pt machdep.piano_gpu_tier machdep.piano_gpu_wire_workers machdep.piano_gpu_wire_priority machdep.piano_data_io; do sc "$k"; done
sec gpu_windows
if command -v ioreg >/dev/null; then
  ioreg -r -n NativeAdrenoGpu -d1 -w0 2>/dev/null | grep -oE '"NativeAdrenoClient[A-Za-z]+" = [0-9]+' || echo "MISSING NativeAdrenoGpu keys"
else echo "MISSING ioreg"; fi
sec top_rss
ps -axo rss=,comm= 2>/dev/null | sort -rn | head -15
sec swap_files
find /private/var/vm -mindepth 1 -maxdepth 1 -type f -exec stat -f '%z %N' {} + 2>/dev/null || echo "MISSING /private/var/vm"
echo M0_DONE
