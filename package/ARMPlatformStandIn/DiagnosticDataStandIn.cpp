// SPDX-License-Identifier: BSD-2-Clause-Patent
// UniXNU-piano: a stand-in for the diagnostic-data service of a Mac (docs/identity.md section 9), compiled into ARMPlatformStandIn
// (tooling/k27/build-native-standin.sh) and matched on IOResources (tooling/k27/kext/ARMPlatformStandIn.json). No hardware access.
//
// On a Mac, 27.3's AppleDiagnosticDataAccessReadOnly (a subclass of AppleDiagnosticDataAccess) drives the NOR flash's syscfg partition
// and publishes it as AppleDiagnosticDataSysCfg (on the Apple-silicon Mac read for this: 128 KiB, all 0xFF, an erased partition).
// libMobileGestalt's syscfg source reads /chosen syscfg-v2-data first (configs/k273-adt.toml), then that property of the first
// AppleDiagnosticDataAccess(ReadOnly) service, and otherwise waits 5 s for an AppleDiagnosticDataAccessReadOnly to be matched. Daemons
// whose sandbox denies IOKit property reads (studentd, symptomsd-diag) can read neither property; on a Mac the existing service ends
// that wait at once, here nothing did. This class is that service and nothing else: the same erased 24 bytes as /chosen, no user
// client, no other properties; in 27.3's userland only libMobileGestalt names these classes, and nothing in the kernel collection.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>

class AppleDiagnosticDataAccess : public IOService {
    OSDeclareDefaultStructors(AppleDiagnosticDataAccess)
};
OSDefineMetaClassAndStructors(AppleDiagnosticDataAccess, IOService)

class AppleDiagnosticDataAccessReadOnly : public AppleDiagnosticDataAccess {
    OSDeclareDefaultStructors(AppleDiagnosticDataAccessReadOnly)

public:
    bool start(IOService *provider) override;
};
OSDefineMetaClassAndStructors(AppleDiagnosticDataAccessReadOnly, AppleDiagnosticDataAccess)

namespace {
// An erased region in the 24 bytes MobileGestalt reads before the magic: it logs "syscfg is not initialized!" and keeps an empty
// sysconfig dictionary, as on the Mac.
constexpr unsigned char kErased[24] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                       0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
}

bool AppleDiagnosticDataAccessReadOnly::start(IOService *provider)
{
    if (!AppleDiagnosticDataAccess::start(provider)) return false;
    const OSSymbol *key = OSSymbol::withCStringNoCopy("AppleDiagnosticDataSysCfg");
    OSData *erased = key ? OSData::withBytes(kErased, sizeof kErased) : nullptr;
    bool published = false;
    if (erased) {
        published = setProperty(key, erased);
        erased->release();
    }
    if (key) key->release();
    registerService();   // MobileGestalt finds it by class (IOServiceMatching), already matched: no wait
    if (published) {
        IOLog("ARMPlatformStandIn: AppleDiagnosticDataAccessReadOnly registered (a stand-in, no hardware: AppleDiagnosticDataSysCfg %u "
              "bytes, erased)\n", (unsigned)sizeof kErased);
    } else {
        IOLog("ARMPlatformStandIn: AppleDiagnosticDataAccessReadOnly registered without AppleDiagnosticDataSysCfg\n");
    }
    return true;
}
