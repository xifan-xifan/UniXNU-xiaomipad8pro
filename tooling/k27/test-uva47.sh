#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host tests of k27 0075 "user VA 47-bit" (tooling/k27/piano-uva47):
#   test-uva47-geometry  the policy's attribute, indices, width, map end and piano-uva47=0 control (pmap_uva47_policy.h);
#   test-uva47-walk      a model of the pmap's table builder against an independent ARMv8 4 KiB walker at T0SZ 17;
#   the patch            patches/k27/0075 adds pmap_uva47_policy.h byte for byte as osfmk/arm/pmap/pmap_uva47_policy.h, leaves the
#                        kernel pmap's attribute and pmap_pt_attr_4k alone, and the k27 source it applies to still has the constants
#                        the tests assume (UVA47_XNU, default the clone's out/k273/k27-tree/out/k27/xnu; skipped when absent).
# The built kernel is checked by tooling/k27/check-uva47-kc.py.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/piano-uva47.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
S=$D/tooling/k27/piano-uva47
for t in geometry walk; do
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O2 -I "$S" "$S/test_uva47_$t.c" -o "$T/test_uva47_$t"
  "$T/test_uva47_$t"
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I "$S" \
    "$S/test_uva47_$t.c" -o "$T/test_uva47_${t}_san"
  "$T/test_uva47_${t}_san" > /dev/null
  echo "test-uva47-$t: ASan/UBSan pass"
done
PATCHES=("$D"/patches/k27/0075-*.patch)
python3 "$D/tooling/k27/check-policy-header.py" osfmk/arm/pmap/pmap_uva47_policy.h "$S/pmap_uva47_policy.h" "${PATCHES[@]}"
python3 - "${PATCHES[0]}" <<'EOF'
import sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text(encoding='utf-8').splitlines()
removed = [l[1:] for l in lines if l.startswith('-') and not l.startswith('---')]
added = [l[1:] for l in lines if l.startswith('+') and not l.startswith('+++')]
bad = [l for l in removed if 'kernel_pmap->pmap_pt_attr = native_pt_attr' in l or '.pta_root_level = (T0SZ_BOOT - 16) / 9' in l
       or 'ARM_PTE_T0_REGION_MASK(TCR_EL1_4KB)' in l]
if bad:
    raise SystemExit('test-uva47: the patch changes the kernel/native attribute: %r' % bad)
need = ['#define ARM_PARAMETERIZED_PMAP', 'pmap_pt_attr_4k_u47', 'MACH_VM_MAX_ADDRESS_RAW 0x00007FFFFE000000ULL',
        '(-1ULL >> (T0SZ_HW))', 'piano-uva47: user VA 47 bits (T0SZ 17, L0 root), map end', 'piano-uva47: user VA 39 bits (piano-uva47=0), map end',
        'int return_value = 64 - T0SZ_HW;']
for n in need:
    if not any(n in l for l in added):
        raise SystemExit('test-uva47: the patch adds no line with %r' % n)
print('test-uva47: the patch keeps the kernel attribute native and adds the %d expected pieces' % len(need))
EOF
X=${UVA47_XNU:-$D/out/k273/k27-tree/out/k27/xnu}
if [ -d "$X/osfmk" ]; then
  python3 - "$X" <<'EOF'
import sys, re
x = sys.argv[1]
def val(path, name):
    m = re.search(r'#define\s+%s\s+(\S+)' % re.escape(name), open(x + '/' + path).read())
    if not m:
        raise SystemExit('test-uva47: %s defines no %s' % (path, name))
    return m.group(1)
want = {('osfmk/arm64/proc_reg.h', 'ARM_4K_TT_L0_SHIFT'): '39', ('osfmk/arm64/proc_reg.h', 'ARM_4K_TT_L1_SHIFT'): '30',
        ('osfmk/arm64/proc_reg.h', 'ARM_4K_TT_L2_SHIFT'): '21', ('osfmk/arm64/proc_reg.h', 'ARM_4K_TT_L3_SHIFT'): '12',
        ('osfmk/arm64/proc_reg.h', 'ARM_4K_TT_L0_INDEX_MASK'): '0x0000ff8000000000ULL',
        ('osfmk/mach/shared_region.h', 'SHARED_REGION_BASE_ARM64'): '0x180000000ULL',
        ('osfmk/mach/shared_region.h', 'SHARED_REGION_SIZE_ARM64'): '0x200000000ULL',
        ('osfmk/arm/cpu_capabilities.h', '_COMM_PAGE64_BASE_ADDRESS'): '(0x0000000FFFFFC000ULL)',
        ('osfmk/arm/cpu_capabilities.h', '_COMM_PAGE64_RO_ADDRESS'): '(0x0000000FFFFF4000ULL)',
        ('osfmk/arm/cpu_capabilities.h', '_COMM_PAGE64_NESTING_START'): '(0x0000000FC0000000ULL)',
        ('osfmk/vm/vm_map_store_internal.h', 'VMGO_START_SHIFT'): '16'}
for (p, n), v in want.items():
    got = val(p, n)
    if got != v:
        raise SystemExit('test-uva47: %s %s is %s, the tests assume %s' % (p, n, got, v))
pr = open(x + '/osfmk/arm64/proc_reg.h').read()
if not re.search(r'#define T0SZ_HW 17ULL', pr) or not re.search(r'#define T0SZ_BOOT 25ULL', pr):
    raise SystemExit('test-uva47: proc_reg.h lacks T0SZ_HW 17 / T0SZ_BOOT 25 (k27 0010)')
print('test-uva47: %s has the %d constants the tests assume, T0SZ_HW 17 and T0SZ_BOOT 25' % (x, len(want)))
EOF
else
  echo "test-uva47: no k27 source at $X: the constants check is skipped"
fi
echo "test-uva47: PASS"
