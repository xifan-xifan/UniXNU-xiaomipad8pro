// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// RAM16G R3 (docs/extra-ram.md): PianoVideo's extra-RAM pool (use 3, video) and its report machdep.piano_extra_ram_video. Apart from
// PianoVideo.cpp because <sys/sysctl.h> declares `struct session`, which collides with pianovideo::session there.
#include <PianoExtraRamSysctl.h>

// constinit: no static constructor needed (a kext C++ initializer must not be relied on for the defaults, e.g. testArg = 1).
constinit pianoxram::KernelPool gPianoVideoExtra;

namespace {
struct sysctl_oid gOid;
int handler(struct sysctl_oid *, void *, int, struct sysctl_req *req) { return pianoxram::sysctlRead(gPianoVideoExtra, req); }
bool gRegistered = false;
} // namespace

void pianoVideoExtraRamSysctl()
{
  if (gRegistered) return;
  gRegistered = true;
  pianoxram::registerSysctl(gOid, "piano_extra_ram_video", handler);
}
