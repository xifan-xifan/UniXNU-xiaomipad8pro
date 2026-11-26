// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026 Leo Zhang.
// The kernel half of PianoExtraRam.h (docs/extra-ram.md): one pool per using kext. It reads /chosen/piano-extra-ram and the boot
// arguments, runs the self-test through a write-combined CPU window (IODeviceMemory, as PianoAudio maps the cdsp slice: the runs lie
// outside XNU's pa_valid range, so pmap has no cacheable alias of them), hands out slabs under a lock, describes taken slabs as one
// physical IOMemoryDescriptor (kIOMemoryTypePhysical64, adjacent slabs merged) and keeps the report text. The root-only string sysctl
// machdep.piano_extra_ram_<use> is PianoExtraRamSysctl.h (apart, because <sys/sysctl.h> brings BSD names such as `struct session`
// that collide with drivers' own namespaces: include it in a separate source file of the kext).
// Boot arguments (all read here, so every user behaves the same):
//   piano-extra-ram=0|1          the master switch (default 0: nothing is mapped, the drivers allocate from XNU as before)
//   <the driver's own switch>    e.g. piano-video-extra-ram (default 0); both must be 1
//   piano-extra-ram-any=<use>    the one use served the use-0 runs (default none)
//   piano-extra-ram-stage=N      the highest allowed-span stage served (default 1; region0-allow.json "stage")
//   piano-extra-ram-test=0       skip the CPU self-test (default 1; a skipped test hands nothing out: tested != 1)
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include "PianoExtraRam.h"

namespace pianoxram {

class KernelPool {
 public:
  Pool pool;
  uint32_t master = 0, driver = 0, testArg = 1;
  bool started = false;
  IOLock *lock = nullptr;
  char text[2048] = "not started";

  // use: the caller's tag; driverArg: its own switch; xnuBase/xnuSize: gPhysBase/gPhysSize (the caller declares them).
  // Returns the free slabs after the self-test (0: the driver uses XNU memory as before).
  uint32_t start(uint32_t use, const char *driverArg, uint64_t xnuBase, uint64_t xnuSize, int (*device)(uint64_t pa, uint64_t bytes) = nullptr)
  {
    if (started) return free();
    started = true;
    lock = IOLockAlloc();
    PE_parse_boot_argn("piano-extra-ram", &master, sizeof master);
    if (driverArg) PE_parse_boot_argn(driverArg, &driver, sizeof driver);
    Config c;
    c.use = use;
    PE_parse_boot_argn("piano-extra-ram-any", &c.anyOwner, sizeof c.anyOwner);
    PE_parse_boot_argn("piano-extra-ram-stage", &c.stage, sizeof c.stage);
    PE_parse_boot_argn("piano-extra-ram-test", &testArg, sizeof testArg);
    c.xnuBase = xnuBase;
    c.xnuSize = xnuSize;
    if (!lock || !master || !driver) {
      // Off: report the ADT only (no mapping), so a read shows what would be served.
      readAdt(c);
      refresh();
      return 0;
    }
    readAdt(c);
    if (testArg) {
      Ops ops{device};
      pool.selfTest(ops);
    }
    refresh();
    return free();
  }

  uint32_t free()
  {
    if (!lock) return 0;
    IOLockLock(lock);
    const uint32_t n = pool.freeSlabs();
    IOLockUnlock(lock);
    return n;
  }
  bool take(unsigned n, bool contiguous, uint64_t *out)
  {
    if (!lock || !master || !driver) return false;
    IOLockLock(lock);
    const bool ok = pool.take(n, contiguous, out);
    IOLockUnlock(lock);
    refresh();
    return ok;
  }
  void give(const uint64_t *slabs, unsigned n)
  {
    if (!lock) return;
    IOLockLock(lock);
    for (unsigned i = 0; i < n; ++i) pool.give(slabs[i]);
    IOLockUnlock(lock);
    refresh();
  }
  bool holds(uint64_t pa)
  {
    if (!lock) return false;
    IOLockLock(lock);
    const bool h = pool.holds(pa);
    IOLockUnlock(lock);
    return h;
  }

  // The taken slabs as one physical descriptor (adjacent slabs merged into one range); the caller releases it.
  static IOMemoryDescriptor *describe(const uint64_t *slabs, unsigned n)
  {
    if (!slabs || !n) return nullptr;
    IOAddressRange *r = static_cast<IOAddressRange *>(IOMallocZeroData(sizeof(IOAddressRange) * n));
    if (!r) return nullptr;
    unsigned k = 0;
    for (unsigned i = 0; i < n; ++i) {
      if (k && r[k - 1].address + r[k - 1].length == slabs[i]) r[k - 1].length += kSlab;
      else r[k++] = {slabs[i], kSlab};
    }
    IOMemoryDescriptor *d = IOMemoryDescriptor::withAddressRanges(
        r, k, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryTypePhysical64), nullptr);
    IOFreeData(r, sizeof(IOAddressRange) * n);
    return d;
  }

  void refresh()
  {
    if (lock) IOLockLock(lock);
    pool.report(text, sizeof text, master, driver);
    if (lock) IOLockUnlock(lock);
  }

 private:
  struct Ops {
    int (*dev)(uint64_t, uint64_t);
    IOMemoryMap *last = nullptr;
    volatile uint64_t *map(uint64_t pa, uint64_t bytes)
    {
      IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
      last = m ? m->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
      if (m) m->release();
      return last ? reinterpret_cast<volatile uint64_t *>(last->getVirtualAddress()) : nullptr;
    }
    void unmap(volatile uint64_t *, uint64_t)
    {
      if (last) last->release();
      last = nullptr;
    }
    void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
    int device(uint64_t pa, uint64_t bytes) { return dev ? dev(pa, bytes) : -1; }
  };

  void readAdt(const Config &c)
  {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *d = chosen ? OSDynamicCast(OSData, chosen->getProperty("piano-extra-ram")) : nullptr;
    pool.load(d ? static_cast<const uint8_t *>(d->getBytesNoCopy()) : nullptr, d ? d->getLength() : 0, c);
    if (chosen) chosen->release();
  }
};

} // namespace pianoxram
