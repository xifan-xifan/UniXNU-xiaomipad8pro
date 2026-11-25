/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
#pragma once
#include <sys/sysctl.h>
#include <sys/kauth.h>
#include <sys/errno.h>
#include "PianoObserve.h"
SYSCTL_DECL(_machdep);
namespace pianoobserve {
/* CTLFLAG_RD alone does not restrict readers. Check credentials before allocation or hardware loads. */
template <class Fill> int readOnly(struct sysctl_req *req, Fill fill) {
    if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
    if (req->newptr) return EPERM;
    constexpr size_t capacity = 65536;
    // sysctl(8) first asks for size, then allocates. Counters/text may grow between calls;
    // reserve the bound without touching hardware on the size-only query.
    if (!req->oldptr) return SYSCTL_OUT(req, nullptr, capacity);
    char *buffer = static_cast<char *>(IOMallocZeroData(capacity));
    if (!buffer) return ENOMEM;
    Text text = {buffer, capacity};
    fill(text);
    const int error = text.truncated ? EOVERFLOW : SYSCTL_OUT(req, buffer, text.used + 1);
    IOFreeData(buffer, capacity);
    return error;
}
inline void registerReadOnly(struct sysctl_oid &oid, const char *name, int selector,
                            int (*handler)(struct sysctl_oid *, void *, int, struct sysctl_req *)) {
    oid.oid_parent = &sysctl__machdep_children;
    oid.oid_number = OID_AUTO;
    oid.oid_kind = int(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2);
    oid.oid_name = name; oid.oid_arg2 = selector; oid.oid_handler = handler;
    oid.oid_fmt = "A"; oid.oid_descr = "Root-only passive sens-trace2 snapshot (no hardware writes)";
    oid.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&oid);
}
}
