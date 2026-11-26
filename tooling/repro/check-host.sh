#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Can this Mac build the piano images? Read only: it runs version queries and looks for files, it installs and changes nothing.
#
#   tooling/repro/check-host.sh          one line per requirement (ok / WARN / FAIL / GAP), exit 1 when any FAIL
#
# FAIL = the build cannot run here; WARN = untested, may work; GAP = a local-only input this repository does not provide
# (skills/install-piano-macos/SKILL.md "Known gaps"). The verdict on the architecture, with its evidence, is in the same skill:
# an Apple silicon Mac is required (macOS 27 and Xcode 27 ship arm64-only binaries, and the build runs arm64e programs it compiled).
set -uo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
fails=0 warns=0 gaps=0
ok()   { printf 'ok    %s\n' "$*"; }
warn() { printf 'WARN  %s\n' "$*"; warns=$((warns + 1)); }
bad()  { printf 'FAIL  %s\n' "$*"; fails=$((fails + 1)); }
gap()  { printf 'GAP   %s\n' "$*"; gaps=$((gaps + 1)); }
have() { command -v "$1" > /dev/null 2>&1; }
vge()  { # vge A B: version A >= B (dotted numbers)
  python3 -I -c 'import sys; a, b = ([int(x) for x in v.split(".")] for v in sys.argv[1:3]); sys.exit(0 if a >= b else 1)' "$1" "$2" 2> /dev/null; }

# 1 the host
[ "$(uname -s)" = Darwin ] && ok "macOS host" || { bad "not macOS ($(uname -s)): the build needs hdiutil/APFS, codesign, Xcode, compression_tool, aea"; }
ARCH=$(uname -m)
if [ "$(sysctl -n sysctl.proc_translated 2> /dev/null)" = 1 ]; then
  bad "this shell runs under Rosetta (x86_64 on Apple silicon): run the build from a native arm64 shell"
elif [ "$ARCH" = arm64 ]; then ok "Apple silicon (arm64)"
else bad "CPU $ARCH: an Apple silicon Mac is required (macOS 27 and Xcode 27 are arm64-only; the build runs the arm64e programs it compiles)"; fi
OSV=$(sw_vers -productVersion 2> /dev/null || echo 0)
if vge "$OSV" 27.0; then ok "macOS $OSV (tested: 27.2.1)"
elif vge "$OSV" 26.6; then warn "macOS $OSV: untested (tested: 27.2.1); Xcode 27 needs 26.6 or later, the 27.3 APFS images may not mount on older hosts"
else bad "macOS $OSV: Xcode 27 needs macOS 26.6 or later"; fi

# 2 Xcode (not only the Command Line Tools: the macOS SDK with Kernel.framework, xcrun clang/ld for arm64e)
XV=$(xcodebuild -version 2> /dev/null) && XV=$(printf '%s\n' "$XV" | awk 'NR == 1 {print $2}') || XV=
if [ -n "$XV" ]; then
  if vge "$XV" 27.0; then ok "Xcode $XV (tested: 27.2, 27B52)"; else warn "Xcode $XV: untested (tested: 27.2); the k27 kernel's flags and the SDK 27 headers were built with Xcode 27"; fi
  SDK=$(xcrun --sdk macosx --show-sdk-path 2> /dev/null)
  [ -d "$SDK/System/Library/Frameworks/Kernel.framework/Headers" ] && ok "macOS SDK $(xcrun --sdk macosx --show-sdk-version 2> /dev/null) with Kernel.framework" \
    || bad "no Kernel.framework in the macOS SDK ($SDK): install Xcode and run xcode-select -s /Applications/Xcode.app"
  [ -d "$(xcrun --sdk iphoneos --show-sdk-path 2> /dev/null)" ] && ok "iphoneos SDK (the k27 kernel builds with SDKROOT=iphoneos)" \
    || bad "no iphoneos SDK: the k27 kernel's make uses SDKROOT=iphoneos (Xcode's iOS platform component)"
else
  bad "no Xcode (xcodebuild): install Xcode 27 from Apple and run sudo xcode-select -s /Applications/Xcode.app; the Command Line Tools alone lack the SDKs"
fi

# 3 macOS tools the build and the fetch call
for t in /usr/bin/aea /usr/bin/compression_tool /usr/bin/hdiutil /usr/sbin/diskutil /sbin/newfs_apfs /usr/bin/codesign /usr/bin/ditto /usr/sbin/mkfile /usr/bin/curl /usr/bin/git; do
  [ -x "$t" ] && ok "$t" || bad "missing $t"
done
if PV=$(python3 -I -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2> /dev/null) && vge "$PV" 3.11; then ok "python3 $PV"
else bad "python3 3.11 or later is needed (found ${PV:-none})"; fi

# 4 Homebrew packages, at the Apple-silicon prefix /opt/homebrew that several scripts name (PYTHON315, LLVM_NM, LLVM_BIN override some)
[ -d /opt/homebrew ] && ok "Homebrew at /opt/homebrew" || bad "no /opt/homebrew (Apple silicon Homebrew): tooling/k27, tooling/metal and the k27 kernel script use it"
for spec in "python@3.15:/opt/homebrew/opt/python@3.15/bin/python3.15:the Turnip meson venv (PYTHON315)" \
            "llvm:/opt/homebrew/opt/llvm/bin/llvm-nm:llvm-nm and the GOP display build (LLVM_NM, LLVM_BIN)" \
            "gnu-sed:/opt/homebrew/opt/gnu-sed/libexec/gnubin/sed:the k27 kernel's GNU-style sed -i" \
            "bison:/opt/homebrew/opt/bison/bin/bison:Mesa's parsers (Turnip)" \
            "mtools:/opt/homebrew/bin/mformat:the FAT32 ESP (mformat, mcopy, minfo)" \
            "dtc:/opt/homebrew/bin/dtc:device trees" \
            "zstd:/opt/homebrew/bin/zstd:release archives"; do
  IFS=: read -r pkg path why <<< "$spec"
  [ -x "$path" ] && ok "$pkg ($why)" || bad "brew install $pkg: $path missing ($why)"
done
have ipsw && ok "ipsw $(ipsw version 2> /dev/null | awk 'NR == 1 {sub(",", "", $2); print $2}') (the AEA key fetch)" \
  || bad "no ipsw: brew install blacktop/tap/ipsw (the AEA key fetch of tooling/k273/decrypt-aea.py)"
have fastboot && ok "fastboot ($(command -v fastboot))" || warn "no fastboot: brew install --cask android-platform-tools (needed for the tablet steps, not the build)"

# 5 disk: inputs ~32 GB at the peak, the root image 21 GB + SystemOS 6 GB, the k27 tree ~3 GB, builds and images ~20 GB
FREE=$(df -k "$D" | awk 'NR == 2 {print int($4 / 1048576)}')
[ "${FREE:-0}" -ge 90 ] && ok "${FREE} GiB free on the checkout's volume" || warn "${FREE:-?} GiB free: plan for about 90 GiB (IPSW inputs, root image, builds)"

# 6 the local-only inputs (Known gaps)
K27=$(. "$D/tooling/k27/k27-tree.sh" 2> /dev/null && echo "$PIANO_K27_TREE")
if [ -n "$K27" ] && [ -x "$K27/build-k27-piano.sh" ]; then ok "k27 tree at PIANO_K27_TREE"
  [ -x "$K27/out/x12377/ldc/ld" ] && ok "ld-classic in the k27 tree (out/x12377/ldc/ld)" || gap "no out/x12377/ldc/ld in the k27 tree: the kernel links with ld-classic (ld64-957 of an older Command Line Tools)"
else gap "PIANO_K27_TREE: the k27 kernel tree (xnu-12377 + the baseline series) is local-only"; fi
[ -f "$D/out/inputs/kernel.macho.trustcache" ] && ok "base trust bundle out/inputs/kernel.macho.trustcache" \
  || gap "out/inputs/kernel.macho.trustcache: the 27.0.1 line's trust bundle whose own modules make-trust.py keeps (from the k27 tree)"
[ -f "$D/out/inputs/scratch-data.img" ] || [ -n "${K273_SEED:-}" ] && ok "a Data seed (out/inputs/scratch-data.img or K273_SEED)" \
  || gap "no Data seed (out/inputs/scratch-data.img or K273_SEED): the macOS Data volume the RAM images start from"
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
[ -d "$R/UniXNU" ] && [ -d "$R/Mu-Silicium-piano" ] && ok "UNIXNU_RESEARCH checkouts ($R)" \
  || gap "UNIXNU_RESEARCH ($R): the unixnu.lock checkouts the UEFI FD is built from (public; clone them at their pins)"

echo "summary: $fails FAIL, $warns WARN, $gaps GAP"
[ "$fails" = 0 ]
