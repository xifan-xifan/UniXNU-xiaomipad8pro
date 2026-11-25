// SPDX-License-Identifier: BSD-2-Clause-Patent
// AppleSmc.cpp: piano's AppleSMC stand-in (docs/policy.md section 12), linked into PianoPolicy's kext with its own personality on
// IOResources and its own boot argument; it does not need piano-policy and changes no clock.
//
// macOS shows the three-option Energy Mode (Automatic / Low Power / High Power) in System Settings > Battery only when
// IOPMFeatureIsAvailable("HighPowerMode") is true; for Mac17,11 that needs an AppleSMC service whose user client answers the SMC
// key 'MSHP' with 1 (Smc.h). This is that service and nothing more: 'MSHP' is the only key it knows; every other key and every
// write is "not found" (Smc.h answer(), host-tested in tests/smc_host.cpp).
//   AppleSMC        an IOService on IOResources, IOUserClientClass AppleSMCClient (the sandboxed Battery settings extension may open
//                   exactly that user client class). Deliberately NOT an AppleSMCFamily: Apple's kexts cast to that metaclass
//                   (IOUSBHostFamily's AppleUSBHostResourcesTypeC) and must keep failing; and it is not published at all on a device
//                   tree whose product node carries usb-c-smc-pwr (the only case in which that IOUSBHostFamily path runs, Smc.h).
//   AppleSMCClient  selectors 0 (open) and 1 (close) succeed, 2 answers one parameter structure (80 or 168 bytes, up to
//                   kMaxBytes), anything else kIOReturnUnsupported. The client types it lets open depend on the level: 1 only type
//                   1 (IOKit.framework's callSMCFunction, powerd), 2 also type 0 (libSMC: CoreBrightness, thermalmonitord, ...).
// Every process that opens it is logged once by name (and its first 'MSHP' answer, and its first refusal), every key once with the
// first process that asked; the counters are in sysctl machdep.piano_smc.
//
// Boot arguments:
//   piano-smc=1        publish the stand-in; client type 1 only (the Energy Mode path)       piano-smc=0 (default): nothing at all
//   piano-smc=2        also client type 0 (libSMC's users then see "an SMC without keys" instead of "no SMC")
//   piano-smc-mshp=0   publish it but answer 'MSHP' "not found" too (the A/B for "an SMC is there" without High Power Mode)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include "Smc.h"

using namespace pianosmc;

namespace {

struct SmcArgs {
  uint32_t level = 0, typeMask = 0;
  Config config;
};
SmcArgs gSmc;

uint32_t smcBootArg(const char *name, uint32_t fallback)
{
  uint32_t v = fallback;
  return PE_parse_boot_argn(name, &v, sizeof v) ? v : fallback;
}

void readSmcArgs()
{
  gSmc.level = smcBootArg("piano-smc", 0);
  if (gSmc.level > 2) gSmc.level = 2;
  gSmc.typeMask = typeMaskForLevel(gSmc.level);
  gSmc.config.mshp = smcBootArg("piano-smc-mshp", 1) != 0;
}

void smcNote(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void smcNote(const char *fmt, ...)
{
  char line[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  IOLog("PianoSmc: %s\n", line);
}

// ------------------------------------------------------------------------------------------------ counters (under gSmcLock)
constexpr unsigned kClients = 24, kKeys = 32;
struct ClientRow {
  char name[33];
  uint32_t opens, refused, calls, mshp, notFound, bad, types;
  bool mshpLogged, refusedLogged;
};
struct KeyRow {
  uint32_t key;
  uint8_t command;
  uint32_t count;
  char first[33];
};
struct Counters {
  uint32_t opens, refused, closes, calls, mshp, notFound, bad, otherSelectors, keysDropped, clientsDropped;
  ClientRow clients[kClients];
  unsigned nClients;
  KeyRow keys[kKeys];
  unsigned nKeys;
  bool u1Refused;
  char u1Where[48];
};
IOLock *gSmcLock;
Counters gCounters;

// The row of a process name, made on first sight (nullptr when the table is full; the totals still count).
ClientRow *clientRow(const char *name)
{
  for (unsigned i = 0; i < gCounters.nClients; ++i)
    if (!strncmp(gCounters.clients[i].name, name, sizeof gCounters.clients[i].name)) return &gCounters.clients[i];
  if (gCounters.nClients == kClients) {
    ++gCounters.clientsDropped;
    return nullptr;
  }
  ClientRow *r = &gCounters.clients[gCounters.nClients++];
  bzero(r, sizeof *r);
  strlcpy(r->name, name, sizeof r->name);
  return r;
}

// ------------------------------------------------------------------------------------------------ machdep.piano_smc
constexpr size_t kStatusBytes = 4096;
void appendf(char *buf, size_t &at, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void appendf(char *buf, size_t &at, const char *fmt, ...)
{
  if (at + 1 >= kStatusBytes) return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf + at, kStatusBytes - at, fmt, ap);
  va_end(ap);
  if (n > 0) at = at + size_t(n) < kStatusBytes ? at + size_t(n) : kStatusBytes - 1;
}

int sysctlSmc(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
  char *buf = static_cast<char *>(IOMallocZeroData(kStatusBytes));
  if (!buf) return ENOMEM;
  size_t at = 0;
  if (gSmcLock) IOLockLock(gSmcLock);
  appendf(buf, at, "smc %u types 0x%x mshp %u u1 %s; opens %u refused %u closes %u calls %u mshp %u notfound %u bad %u other %u\n",
          gSmc.level, gSmc.typeMask, gSmc.config.mshp ? 1 : 0, gCounters.u1Refused ? "REFUSED" : "ok", gCounters.opens,
          gCounters.refused, gCounters.closes, gCounters.calls, gCounters.mshp, gCounters.notFound, gCounters.bad,
          gCounters.otherSelectors);
  for (unsigned i = 0; i < gCounters.nClients; ++i) {
    const ClientRow &r = gCounters.clients[i];
    appendf(buf, at, "client %s types 0x%x opens %u refused %u calls %u mshp %u notfound %u bad %u\n", r.name, r.types, r.opens,
            r.refused, r.calls, r.mshp, r.notFound, r.bad);
  }
  for (unsigned i = 0; i < gCounters.nKeys; ++i) {
    const KeyRow &k = gCounters.keys[i];
    char t[5];
    keyText(k.key, t);
    appendf(buf, at, "key '%s' 0x%08x cmd %u n %u first %s\n", t, k.key, k.command, k.count, k.first);
  }
  if (gCounters.clientsDropped || gCounters.keysDropped)
    appendf(buf, at, "dropped clients %u keys %u\n", gCounters.clientsDropped, gCounters.keysDropped);
  if (gSmcLock) IOLockUnlock(gSmcLock);
  const int err = SYSCTL_OUT(req, buf, strlen(buf) + 1);
  IOFreeData(buf, kStatusBytes);
  return err;
}

// Filled in at run time rather than by SYSCTL_OID's static initializer (its parent and handler are signed pointers on arm64e).
struct sysctl_oid gOidSmc;
bool gOidSmcRegistered;

}  // namespace

SYSCTL_DECL(_machdep);

// ------------------------------------------------------------------------------------------------ AppleSMC
class AppleSMC : public IOService {
  OSDeclareDefaultStructors(AppleSMC)

 public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(AppleSMC, IOService)

IOService *AppleSMC::probe(IOService *provider, SInt32 *score)
{
  readSmcArgs();
  if (!gSmc.level) return nullptr;
  return IOService::probe(provider, score);
}

namespace {
// The U1 gate (Smc.h): the device tree's product node, or a service named "product" (what IOUSBHostFamily's probe looks up),
// carrying usb-c-smc-pwr. Fills `where` with the one that does.
bool productCarriesUsbcSmcPower(char *where, size_t n)
{
  bool found = false;
  if (IORegistryEntry *e = IORegistryEntry::fromPath("IODeviceTree:/product")) {
    if (OSObject *o = e->copyProperty(kUsbcSmcPowerProperty)) {
      found = true;
      o->release();
      strlcpy(where, "IODeviceTree:/product", n);
    }
    e->release();
  }
  if (!found) {
    if (OSDictionary *m = IOService::nameMatching("product")) {
      if (IOService *s = IOService::copyMatchingService(m)) {
        if (OSObject *o = s->copyProperty(kUsbcSmcPowerProperty)) {
          found = true;
          o->release();
          strlcpy(where, "a service named product", n);
        }
        s->release();
      }
      m->release();
    }
  }
  return found;
}
}  // namespace

bool AppleSMC::start(IOService *provider)
{
  if (!gSmcLock) gSmcLock = IOLockAlloc();
  if (!gSmcLock) return false;
  char where[48] = "";
  if (productCarriesUsbcSmcPower(where, sizeof where)) {
    IOLockLock(gSmcLock);
    gCounters.u1Refused = true;
    strlcpy(gCounters.u1Where, where, sizeof gCounters.u1Where);
    IOLockUnlock(gSmcLock);
    smcNote("start: NOT published: %s carries %s (IOUSBHostFamily's Type-C resources would wait for an AppleSMC and call it as an "
            "AppleSMCFamily; docs/policy.md 12.3)", where, kUsbcSmcPowerProperty);
    return false;
  }
  if (!IOService::start(provider)) return false;
  setProperty("IOUserClientClass", "AppleSMCClient");
  setProperty("piano-smc", gSmc.level, 32);
  setProperty("piano-smc-mshp", gSmc.config.mshp ? 1u : 0u, 32);
  setProperty("piano-smc-client-types", gSmc.typeMask, 32);
  if (!gOidSmcRegistered) {
    bzero(&gOidSmc, sizeof gOidSmc);
    gOidSmc.oid_parent = &sysctl__machdep_children;
    gOidSmc.oid_number = OID_AUTO;
    gOidSmc.oid_kind = CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2;
    gOidSmc.oid_name = "piano_smc";
    gOidSmc.oid_handler = sysctlSmc;
    gOidSmc.oid_fmt = "A";
    gOidSmc.oid_descr = "PianoSmc: the AppleSMC stand-in's clients and keys";
    gOidSmc.oid_version = SYSCTL_OID_VERSION;
    sysctl_register_oid(&gOidSmc);
    gOidSmcRegistered = true;
  }
  registerService();
  char t[5];
  keyText(kKeyMSHP, t);
  smcNote("start: AppleSMC stand-in published (piano-smc=%u: client types 0x%x; '%s' = %s; every other key and every write: not "
          "found; machdep.piano_smc)", gSmc.level, gSmc.typeMask, t, gSmc.config.mshp ? "1 (High Power Mode supported)" : "not found");
  return true;
}

// ------------------------------------------------------------------------------------------------ AppleSMCClient
class AppleSMCClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleSMCClient)

 public:
  bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) override;
  bool start(IOService *provider) override;
  IOReturn clientClose() override;
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
                          void *reference) override;

 private:
  char fName[33] = "";
  int fPid = -1;
  uint32_t fType = 0;
  bool fEntitled = false;
};

OSDefineMetaClassAndStructors(AppleSMCClient, IOUserClient)

// Runs in the opening process's thread (IOServiceOpen): its name and pid are the caller's.
bool AppleSMCClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties)
{
  proc_selfname(fName, sizeof fName);
  fPid = proc_selfpid();
  fType = type;
  const bool allowed = typeAllowed(gSmc.typeMask, type);
  if (allowed) {
    if (OSObject *e = IOUserClient::copyClientEntitlement(owningTask, "com.apple.private.applesmc.user-access")) {
      fEntitled = e == kOSBooleanTrue;
      e->release();
    }
  }
  bool first = false, firstRefusal = false;
  uint32_t total = 0;
  IOLockLock(gSmcLock);
  ClientRow *r = clientRow(fName);
  if (allowed) {
    total = ++gCounters.opens;
    if (r) first = r->opens++ == 0;
  } else {
    total = ++gCounters.refused;
    if (r) {
      ++r->refused;
      firstRefusal = !r->refusedLogged;
      r->refusedLogged = true;
    }
  }
  if (r) r->types |= type < 32 ? 1u << type : 0;
  IOLockUnlock(gSmcLock);
  if (first)
    smcNote("open: %s (pid %d) type %u%s (open %u)", fName, fPid, type, fEntitled ? ", applesmc user-access entitled" : "", total);
  if (firstRefusal)
    smcNote("open REFUSED: %s (pid %d) type %u (client types 0x%x; refusal %u): it stays on its no-SMC path", fName, fPid, type,
            gSmc.typeMask, total);
  if (!allowed) return false;
  return IOUserClient::initWithTask(owningTask, securityToken, type, properties);
}

bool AppleSMCClient::start(IOService *provider)
{
  if (!OSDynamicCast(AppleSMC, provider)) return false;
  return IOUserClient::start(provider);
}

IOReturn AppleSMCClient::clientClose()
{
  IOLockLock(gSmcLock);
  ++gCounters.closes;
  IOLockUnlock(gSmcLock);
  if (!isInactive()) terminate();
  return kIOReturnSuccess;
}

IOReturn AppleSMCClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *,
                                        void *)
{
  if (selector == kSelOpen || selector == kSelClose) return kIOReturnSuccess;
  if (selector != kSelCall) {
    IOLockLock(gSmcLock);
    ++gCounters.otherSelectors;
    IOLockUnlock(gSmcLock);
    return kIOReturnUnsupported;
  }
  // The structure is inline (a few hundred bytes at most); a descriptor or a missing buffer is refused.
  Call c;
  c.ret = kBadSize;
  uint8_t buf[kMaxBytes];
  size_t inBytes = 0, outBytes = 0;
  if (args && args->structureInput && args->structureOutput && !args->structureInputDescriptor && !args->structureOutputDescriptor) {
    inBytes = args->structureInputSize;
    outBytes = args->structureOutputSize;
    if (inBytes >= kMinBytes && inBytes <= kMaxBytes && outBytes >= kMinBytes && outBytes <= kMaxBytes) {
      memcpy(buf, args->structureInput, inBytes);
      c = answer(gSmc.config, buf, inBytes, buf, outBytes);
      if (c.ret == 0) memcpy(args->structureOutput, buf, outBytes);
    }
  }
  bool newKey = false, firstMshp = false;
  uint32_t badCount = 0;
  IOLockLock(gSmcLock);
  ++gCounters.calls;
  ClientRow *r = clientRow(fName);
  if (r) ++r->calls;
  if (c.ret) {
    badCount = ++gCounters.bad;
    if (r) badCount = ++r->bad;
  } else if (c.mshp) {
    ++gCounters.mshp;
    if (r) {
      ++r->mshp;
      firstMshp = !r->mshpLogged;
      r->mshpLogged = true;
    }
  } else {
    ++gCounters.notFound;
    if (r) ++r->notFound;
  }
  if (c.ret == 0) {
    unsigned i = 0;
    for (; i < gCounters.nKeys; ++i)
      if (gCounters.keys[i].key == c.key && gCounters.keys[i].command == c.command) break;
    if (i < gCounters.nKeys) {
      ++gCounters.keys[i].count;
    } else if (gCounters.nKeys < kKeys) {
      KeyRow &k = gCounters.keys[gCounters.nKeys++];
      k.key = c.key;
      k.command = c.command;
      k.count = 1;
      strlcpy(k.first, fName, sizeof k.first);
      newKey = true;
    } else {
      ++gCounters.keysDropped;
    }
  }
  IOLockUnlock(gSmcLock);
  char t[5];
  keyText(c.key, t);
  if (c.ret) {
    if (badCount <= 4)
      smcNote("call from %s (pid %d): structure refused (in %u, out %u bytes)", fName, fPid, unsigned(inBytes), unsigned(outBytes));
    return kIOReturnBadArgument;
  }
  if (newKey)
    smcNote("key '%s' (0x%08x) command %u first asked by %s (pid %d, type %u): %s", t, c.key, c.command, fName, fPid, fType,
            c.result == kSuccess ? "answered" : "not found");
  if (firstMshp)
    smcNote("'MSHP' = 1 (High Power Mode supported) to %s (pid %d, type %u, command %u)", fName, fPid, fType, c.command);
  args->structureOutputSize = uint32_t(outBytes);
  return kIOReturnSuccess;
}
