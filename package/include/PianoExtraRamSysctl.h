// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// machdep.piano_extra_ram_<use>: the report of a PianoExtraRamKernel.h pool as a root-only, read-only string (docs/extra-ram.md).
// Include it in its own source file of the kext (it brings <sys/sysctl.h>; see PianoExtraRamKernel.h).
#pragma once
#include <sys/sysctl.h>
#include <sys/kauth.h>
#include <sys/errno.h>
#include "PianoExtraRamKernel.h"

SYSCTL_DECL(_machdep);

namespace pianoxram {
// The caller's handler: return pianoxram::sysctlRead(pool, req);
inline int sysctlRead(KernelPool &pool, struct sysctl_req *req)
{
  if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
  if (req->newptr) return EPERM;
  pool.refresh();
  return SYSCTL_OUT(req, pool.text, strnlen(pool.text, sizeof pool.text) + 1);
}
// oid must be static storage; name e.g. "piano_extra_ram_video".
inline void registerSysctl(struct sysctl_oid &oid, const char *name, int (*handler)(struct sysctl_oid *, void *, int, struct sysctl_req *))
{
  oid.oid_parent = &sysctl__machdep_children;
  oid.oid_number = OID_AUTO;
  // CTLFLAG_OID2: the structure carries oid_descr and oid_version (arm64 panics without it).
  oid.oid_kind = int(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2);
  oid.oid_name = name;
  oid.oid_handler = handler;
  oid.oid_fmt = "A";
  oid.oid_descr = "RAM16G driver pool: ADT runs, ownership, slabs, self-test (root only, read only)";
  oid.oid_version = SYSCTL_OID_VERSION;
  sysctl_register_oid(&oid);
}
} // namespace pianoxram
