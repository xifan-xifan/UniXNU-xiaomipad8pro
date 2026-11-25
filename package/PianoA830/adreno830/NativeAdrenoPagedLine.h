/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_PAGED_LINE_H
#define NATIVE_ADRENO_PAGED_LINE_H
/* UniXNU-piano (2026-11-23, GPU-PAGED P1, private host-checks/20261122-gpu-paged/P1-GATE.md): the paged VM's figures as text, for
 * machdep.piano_gpu_pt (every row) and the publish tick's `gpu pt:` lines (rows 1 and 2, each under 150 characters with "gpu "). Row 0
 * names the mode; a pool-mode VM has only row 0. */
#include "NativeAdrenoVm.h"
#ifdef ADRENO830_HOST_TEST
#include <stdio.h>
#else
#include <libkern/libkern.h>   /* snprintf */
#endif
static inline int NativeAdrenoPagedLine(unsigned row,const NativeAdrenoVmPagedStats &s,char *out,size_t size)
{
    typedef unsigned long long u64;
    if(!out || !size)return 0;
    out[0]=0;
    if(row==0)return snprintf(out,size,"pt mode=%s",s.paged?"paged":"pool");
    if(!s.paged)return 0;
    switch(row) {
    case 1:return snprintf(out,size,"pt: tables %llu/%llu peak %llu (%llu chunks %llu pages, %llu free) leaves %llu; mapped %lluM peak %lluM "
        "cap %lluM ws %lluM; refused t%llu c%llu r%llu",u64(s.table_pages),u64(s.table_cap),u64(s.table_peak),u64(s.chunks),
        u64(s.chunk_pages),u64(s.free_pages),u64(s.leaves),u64(s.mapped>>20),u64(s.mapped_peak>>20),u64(s.cap>>20),u64(s.reserve>>20),
        u64(s.refused_tables),u64(s.refused_cap),u64(s.refused_reserve));
    case 2:return snprintf(out,size,"pt2: maps %llu unmaps %llu val %llu asid %llu empty %llu trims %llu ret %llu pend %lld max %lluus "
        "grow-fail %llu retry %llu poison %u",u64(s.maps),u64(s.unmaps),u64(s.unmap_val),u64(s.unmap_asid),u64(s.kept_empty),u64(s.trims),
        u64(s.trim_retired),s.trim_pending==UINT64_MAX?-1LL:(long long)s.trim_pending,u64(s.trim_us_max),u64(s.grow_failures),
        u64(s.grow_retries),unsigned(s.poisoned));
    case 3:return snprintf(out,size,"pt3: records %llu val-pages %llu full-trims %llu audits %llu failed %llu why %llu at %#llx tables %llu "
        "leaves %llu %lluus",u64(s.records),u64(s.unmap_val_pages),u64(s.trim_full),u64(s.audits),u64(s.audit_failures),u64(s.audit_why),
        u64(s.audit_iova),u64(s.audit_tables),u64(s.audit_leaves),u64(s.audit_us));
    default:return 0;
    }
}
/* A signature of what moves, so the tick prints only while the figures change. */
static inline uint64_t NativeAdrenoPagedSignature(const NativeAdrenoVmPagedStats &s)
{
    return s.maps*1000003u^s.unmaps*999983u^s.trims*7919u^(s.refused_tables+s.refused_cap*3+s.refused_reserve*5)*104729u^
        s.audits*131u^s.grow_failures*17u^uint64_t(s.poisoned);
}
#endif
