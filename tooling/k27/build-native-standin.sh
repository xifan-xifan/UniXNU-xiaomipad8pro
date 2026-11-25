#!/bin/bash
# Build package/ARMPlatformStandIn (AppleARMPlatform's classes for the vma2 collection's IOMobileGraphicsFamily, see its UPSTREAM) as a
# k27 native kext: objects in out/native-kexts/ARMPlatformStandIn for tooling/k27/kext/ARMPlatformStandIn.json.
# BluetoothFamily also links the USB consumer libraries. Reuse the reviewed USB
# metaclass stand-ins and cable keys here; these classes have no hardware objects.
# DiagnosticDataStandIn.cpp: the diagnostic-data service MobileGestalt waits 5 s for in iokit-restricted sandboxes (docs/identity.md
# section 9); the one class here with an object (matched on IOResources), still no hardware.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
python3 "$D/tooling/k27/build-native-kext.py" ARMPlatformStandIn "$D/package/ARMPlatformStandIn/ARMPlatformStandIn.cpp" \
  "$D/package/ARMPlatformStandIn/UsbCableKeys.cpp" "$D/package/PianoUsbHostStandIn/PianoUsbHostStandIn.cpp" \
  "$D/package/ARMPlatformStandIn/DiagnosticDataStandIn.cpp" \
  "$D/tooling/k27/NativeKextGlue.cpp" --obj "${K27_OBJ:-k27-native-obj}"
