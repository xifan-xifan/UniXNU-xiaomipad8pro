#!/bin/bash
# build.sh OUT: the LE test's two host-run binaries, ad-hoc signed arm64: le-peripheral (for the Mac next to the tablet) and
# piano-bt-le-central (probe.m for a pushed run on an image whose trust-cached piano-bt-probe predates --le-read), plus
# piano-bt-le-central-ent with the probe's scoped Bluetooth TCC entitlement (honoured only if the tablet's AMFI/tccd accept it for an
# ad-hoc binary outside the trust cache: the run reports initial_authorization either way).
set -euo pipefail
H=$(cd "$(dirname "$0")/.." && pwd); O=${1:?usage: build.sh OUT}
mkdir -p "$O"; SDK=$(xcrun --sdk macosx --show-sdk-path)
F=(-O2 -Wall -Wextra -Werror -Wno-deprecated-declarations -fstack-protector-strong -fobjc-arc -fblocks -framework Foundation -framework CoreBluetooth)
xcrun --sdk macosx clang -target arm64-apple-macos27.0 -isysroot "$SDK" "${F[@]}" "$H/le-test/le-peripheral.m" -o "$O/le-peripheral"
xcrun --sdk macosx clang -target arm64-apple-macos27.0 -isysroot "$SDK" "${F[@]}" -framework IOBluetooth "$H/probe.m" -o "$O/piano-bt-le-central"
cp "$H/le-test/le-peripheral.command" "$O/le-peripheral.command"
codesign --force --sign - --identifier com.unixnu.piano.bluetooth.le-peripheral "$O/le-peripheral"
codesign --force --sign - --identifier com.unixnu.piano.bluetooth.le-central "$O/piano-bt-le-central"
python3 -c 'import plistlib,sys; sys.stdout.buffer.write(plistlib.dumps({"com.apple.private.security.no-container": True, "com.apple.private.tcc.allow": ["kTCCServiceBluetoothAlways"]}))' > "$O/central-ent.plist"
cp "$O/piano-bt-le-central" "$O/piano-bt-le-central-ent"
codesign --force --sign - --identifier com.unixnu.piano.bluetooth.le-central --entitlements "$O/central-ent.plist" "$O/piano-bt-le-central-ent"
xattr -c "$O"/le-peripheral "$O"/piano-bt-le-central "$O"/piano-bt-le-central-ent "$O"/le-peripheral.command 2>/dev/null || true
shasum -a 256 "$O"/le-peripheral "$O"/piano-bt-le-central "$O"/piano-bt-le-central-ent
