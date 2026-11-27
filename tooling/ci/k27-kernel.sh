#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The k27 native kernel from public sources on a macOS host with Xcode (a GitHub macos-26 runner or a desk Mac): the stages the
# local build has under the k27 tree's out/, made from their sources instead of copied from the maintainer's tree.
#
#   tooling/ci/k27-kernel.sh [STAGE...]      stages, default all, in this order (fakeroot after kdk: installhdrs reads the KDK):
#     tree      the k27 tree (tooling/k27/fetch-k27-tree.sh) at $K27_DIR (default $WORK/k27-tree)
#     xnu       out/k27/xnu: apple-oss-distributions/xnu xnu-12377.121.6 + the tree's patches/xnu-12377 0001-0054 + patches/k27
#               0055-$K27_TREE_TOP (git am)
#     fakeroot  out/x12377/fakeroot (KERNEL_SDKROOT): AvailabilityVersions, libplatform and libdispatch headers, libfirehose_kernel.a
#               BUILT FROM libdispatch's source (UniXNU scripts/build-firehose.sh's xcodebuild target); the kernel's own installhdrs
#               are added by build-k27-piano.sh
#     ld        out/x12377/ldc/ld: Apple's ld-classic (ld64), which links -threaded_starts_section; from $LD_CLASSIC or the toolchain
#     kdk       out/k27/kdk-notb: the headers of a Kernel Debug Kit, from $KDK_DMG (a local .dmg; Apple's download needs an Apple
#               Account, docs/ci.md "Open items"); Tightbeam left out as the local tree does; nothing of it is ever uploaded
#     platform  out/x12377/un/build/platform-12377-arm64e: ArmDTPlatform and ArmCpu at their unixnu.lock pins, compiled with the
#               kernel's own recipes (a first kernel pass gives the recipes, the libraries go into the second)
#     kernel    build-k27-piano.sh with K27_VARIANT=native -> $OUT/kernel-k27-native.macho and its sha256
#   and, not part of the default: verify (the Apple OSS pins still exist upstream; the monthly dependency check).
#   Environment: WORK (default $PWD/out/ci), OUT (default $WORK/out), JOBS, K27_TREE_TOP (default 0066), KDK_DMG, LD_CLASSIC.
#
# Every Apple-licensed SOURCE here is public (apple-oss-distributions, APSL); the only Apple BINARIES are Xcode's (on the runner) and
# the KDK's headers ($KDK_DMG), neither of which leaves the machine. Status (2026-11-25): tree, xnu, fakeroot and ld were dry-run on a
# desk Mac (the xnu source came out with the local kernel source's git tree id, libfirehose_kernel with its machine code); kdk,
# platform and kernel have not run on a hosted runner yet (docs/ci.md).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-$PWD/out/ci}; OUT=${OUT:-$WORK/out}; JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
M=${K27_DIR:-$WORK/k27-tree}; X=$M/out/x12377; TOP=${K27_TREE_TOP:-0066}
mkdir -p "$WORK" "$OUT"
# The tree's iokit/DriverKit Makefile runs GNU sed -i (as build-k27-piano.sh, which puts the same first on PATH)
GSED=$( (brew --prefix gnu-sed 2>/dev/null || echo /opt/homebrew/opt/gnu-sed) )/libexec/gnubin
[ -x "$GSED/sed" ] || { echo "GNU sed is needed (brew install gnu-sed)" >&2; exit 1; }
export PATH=$GSED:$PATH
# public Apple OSS pins of the kernel environment (the same releases the k27 tree's own environment lock records upstream)
XNU_URL=https://github.com/apple-oss-distributions/xnu.git;                XNU_COMMIT=ac9718fb1af618d5ce8678d0dc6e8a58f252216f        # xnu-12377.121.6
DISPATCH_URL=https://github.com/apple-oss-distributions/libdispatch.git;   DISPATCH_COMMIT=701f4d1a24ae9c6863901bbbb22624b7d1b87321   # libdispatch-1542.100.32
PLATFORM_URL=https://github.com/apple-oss-distributions/libplatform.git;   PLATFORM_COMMIT=b7ed7cf5cf7dd12b98672435db2225a860f199d8   # libplatform-375.120.2
AVAIL_URL=https://github.com/apple-oss-distributions/AvailabilityVersions.git; AVAIL_COMMIT=149b1777f3e8c2133042d8e188f72e3adf6a7110 # AvailabilityVersions-157.2
pin() { python3 -c 'import tomllib,sys;print(tomllib.load(open(sys.argv[1],"rb"))["pin"][sys.argv[2]][sys.argv[3]])' "$D/unixnu.lock" "$1" "$2"; }
fetch() {   # fetch URL COMMIT DIR: DIR's HEAD at COMMIT (one commit deep)
  [ "$(git -C "$3" rev-parse HEAD 2>/dev/null)" = "$2" ] && return 0
  rm -rf "$3"; git init -q "$3"; git -C "$3" remote add origin "$1"
  git -C "$3" fetch -q --depth 1 origin "$2" && git -C "$3" checkout -q --detach "$2"
}
say() { printf '== %s\n' "$*"; }

stage_verify() {   # not a build stage: every Apple OSS pin above still exists upstream (the monthly dependency check)
  say "verify the Apple OSS pins"
  local t rc=0 pair url commit
  for pair in "$XNU_URL $XNU_COMMIT" "$DISPATCH_URL $DISPATCH_COMMIT" "$PLATFORM_URL $PLATFORM_COMMIT" "$AVAIL_URL $AVAIL_COMMIT"; do
    IFS=' ' read -r url commit <<< "$pair"
    t=$(mktemp -d)
    if git -C "$t" init -q && git -C "$t" fetch -q --depth 1 --filter=blob:none "$url" "$commit"; then echo "ok   $url @ ${commit:0:10}"
    else echo "FAIL $url @ ${commit:0:10}"; rc=1; fi
    rm -rf "$t"
  done
  return $rc
}

stage_tree() { say tree; bash "$D/tooling/k27/fetch-k27-tree.sh" "$M"; }

stage_xnu() {
  say "xnu (xnu-12377.121.6 + patches/xnu-12377 0001-0054 + patches/k27 0055-$TOP)"
  local T=$M/out/k27/xnu
  if [ -f "$T/.piano-series" ] && [ "$(cat "$T/.piano-series")" = "$TOP" ]; then echo "out/k27/xnu is there"; return 0; fi
  mkdir -p "$M/out/k27"; fetch "$XNU_URL" "$XNU_COMMIT" "$T"
  local am=(git -C "$T" -c user.name=k27-tree -c user.email=k27-tree@fetch.invalid am -q --whitespace=nowarn --committer-date-is-author-date)
  "${am[@]}" "$M"/patches/xnu-12377/00*.patch
  local p n
  for p in "$D"/patches/k27/0*.patch; do
    n=$(basename "$p"); n=${n%%-*}
    [ "$((10#$n))" -gt 54 ] && [ "$((10#$n))" -le "$((10#$TOP))" ] || continue
    "${am[@]}" "$p"
  done
  echo "$TOP" > "$T/.piano-series"
  git -C "$T" log --oneline -1
}

stage_fakeroot() {
  say "fakeroot (Apple OSS headers, libfirehose_kernel from source)"
  local FR=$X/fakeroot S=$WORK/src KDKROOT_DIR=
  [ -d "$M/out/k27/kdk-notb" ] && KDKROOT_DIR=$M/out/k27/kdk-notb
  if [ -f "$FR/usr/local/lib/kernel/libfirehose_kernel.a" ]; then echo "fakeroot is there"; return 0; fi
  mkdir -p "$S" "$FR/usr/local/include/os" "$FR/AppleInternal" \
    "$FR/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform"
  fetch "$DISPATCH_URL" "$DISPATCH_COMMIT" "$S/libdispatch"
  fetch "$PLATFORM_URL" "$PLATFORM_COMMIT" "$S/libplatform"
  fetch "$AVAIL_URL" "$AVAIL_COMMIT" "$S/AvailabilityVersions"
  make -C "$S/AvailabilityVersions" install -j"$JOBS" OBJROOT="$WORK/build/availability" SYMROOT="$WORK/build/availability" \
    DSTROOT="$FR" > "$WORK/availability.log" 2>&1 || { tail -20 "$WORK/availability.log"; exit 1; }
  cp -R "$S/libplatform/include/." "$FR/usr/local/include/"
  cp -R "$S/libplatform/private/." "$FR/usr/local/include/"
  cp -R "$S/libplatform/private/platform/." "$FR/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform/"
  local h
  for h in "$S"/libdispatch/os/*.h; do
    unifdef -DKERNEL=1 -DOS_FIREHOSE_SPI=1 -DOS_VOUCHER_ACTIVITY_SPI_TYPES=1 -UOS_VOUCHER_ACTIVITY_SPI \
      -o "$FR/usr/local/include/os/$(basename "$h")" "$h" || [ $? -eq 1 ]
  done
  # The kernel headers the firehose target includes come from the tree's installhdrs: done here once (build-k27-piano.sh skips it then),
  # with build-k27-piano.sh's environment.
  KERNEL_SDKROOT=$FR XNU_AVAILABILITY_PL=$FR/usr/local/libexec/availability.pl make -C "$M/out/k27/xnu" installhdrs SDKROOT=macosx ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=BCM2837 KERNEL_CONFIGS=DEVELOPMENT \
    RC_DARWIN_KERNEL_VERSION=27.0.0 RC_ProjectSourceVersion=12377.121.6 MAKEJOBS=-j"$JOBS" \
    OBJROOT="$X/build/xnu/k27-hdr-obj" SYMROOT="$X/build/xnu/k27-hdr-sym" DSTROOT="$FR" \
    ${KDKROOT_DIR:+KDKROOT=$KDKROOT_DIR} \
    > "$WORK/installhdrs.log" 2>&1 || { tail -30 "$WORK/installhdrs.log"; exit 1; }
  touch "$FR/.k27-headers"
  # libfirehose_kernel: UniXNU scripts/build-firehose.sh's macOS path (the libfirehose_kernel target of libdispatch's project, its
  # xcconfig pointed at the fakeroot), arm64e as the kernel links it
  python3 - "$S/libdispatch/xcodeconfig/libfirehose_kernel.xcconfig" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
s = s.replace('$(SDKROOT)/System/Library/Frameworks/Kernel.framework', '$(FAKEROOT_DIR)/System/Library/Frameworks/Kernel.framework')
s = s.replace('$(SDKROOT)/usr/local/include', '$(FAKEROOT_DIR)/usr/local/include')
lines = s.splitlines()
for i, line in enumerate(lines):
    if line.startswith('HEADER_SEARCH_PATHS =') and '$(SDKROOT)/usr/include' not in line:
        lines[i] += ' $(SDKROOT)/usr/include $(FAKEROOT_DIR)/usr/local/include $(XNU_SRC)/EXTERNAL_HEADERS'
p.write_text('\n'.join(lines) + '\n')
PY
  xcodebuild install -project "$S/libdispatch/libdispatch.xcodeproj" -target libfirehose_kernel -configuration Release -sdk iphoneos \
    -jobs "$JOBS" ARCHS=arm64e VALID_ARCHS=arm64e ONLY_ACTIVE_ARCH=NO IPHONEOS_DEPLOYMENT_TARGET=26.0 CODE_SIGNING_ALLOWED=NO \
    GCC_TREAT_WARNINGS_AS_ERRORS=NO OBJROOT="$WORK/build/firehose/obj" SYMROOT="$WORK/build/firehose/sym" DSTROOT="$FR" \
    FAKEROOT_DIR="$FR" XNU_SRC="$M/out/k27/xnu" > "$WORK/firehose.log" 2>&1 || { tail -30 "$WORK/firehose.log"; exit 1; }
  [ -f "$FR/usr/local/lib/kernel/liblibfirehose_kernel.a" ] && cp "$FR/usr/local/lib/kernel/liblibfirehose_kernel.a" "$FR/usr/local/lib/kernel/libfirehose_kernel.a"
  lipo -info "$FR/usr/local/lib/kernel/libfirehose_kernel.a"
  shasum -a 256 "$FR/usr/local/lib/kernel/libfirehose_kernel.a"
}

stage_ld() {
  say ld-classic
  local ld=${LD_CLASSIC:-$(xcrun -f ld-classic 2>/dev/null || true)}
  [ -n "$ld" ] && [ -x "$ld" ] || { echo "no ld-classic: set LD_CLASSIC (the kernel links with -threaded_starts_section, which only ld64 has; docs/ci.md)" >&2; exit 1; }
  mkdir -p "$X/ldc"; ln -sf "$ld" "$X/ldc/ld"; "$X/ldc/ld" -v 2>&1 | head -1
}

stage_kdk() {
  say "kdk (headers only)"
  local K=$M/out/k27/kdk-notb
  if [ -d "$K/System/Library/Frameworks/Kernel.framework" ]; then echo "kdk-notb is there"; return 0; fi
  [ -n "${KDK_DMG:-}" ] && [ -f "$KDK_DMG" ] || { echo "no KDK: set KDK_DMG to a Kernel Debug Kit .dmg (OWNER DECISION: where CI gets it, docs/ci.md)" >&2; exit 1; }
  local mnt=$WORK/kdk-mnt pkgx=$WORK/kdk-pkg
  mkdir -p "$mnt"; hdiutil attach -quiet -nobrowse -readonly -mountpoint "$mnt" "$KDK_DMG"
  trap 'hdiutil detach -quiet "$mnt" || true' RETURN
  rm -rf "$pkgx"; pkgutil --expand-full "$mnt"/*.pkg "$pkgx"
  local payload; payload=$(find "$pkgx" -type d -path '*Payload/Library/Developer/KDKs/*.kdk' | head -1)
  [ -n "$payload" ] || { echo "no KDK payload in $KDK_DMG" >&2; exit 1; }
  mkdir -p "$K"
  # headers and the kernel-support headers only: no kernels, no libraries are kept (nothing Apple-built is linked into ours)
  rsync -a --include='*/' --include='*.h' --include='*.modulemap' --include='*.plist' --exclude='*' "$payload/System" "$payload/usr" "$K/"
  find "$K" -iname '*Tightbeam*' -prune -exec rm -rf {} +
  rm -rf "$pkgx"
}

stage_platform() {
  say "platform (ArmDTPlatform, ArmCpu)"
  local P=$X/un/build/platform-12377-arm64e S=$WORK/src name
  for name in ArmDTPlatform ArmCpu; do
    fetch "$(pin "$name" git)" "$(pin "$name" commit)" "$S/$name"
    mkdir -p "$P/$name"
  done
  # First pass: the kernel's objects without the platform libraries (the link fails; the objects and their recipes are what is needed).
  K27_VARIANT=native K27_EXTRA_LD='' bash "$M/build-k27-piano.sh" > "$WORK/kernel-pass1.log" 2>&1 || true
  local recipes=$X/build/xnu/k27-native-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json
  [ -f "$recipes" ] || { tail -30 "$WORK/kernel-pass1.log"; echo "no kernel compile recipes after the first pass" >&2; exit 1; }
  for name in ArmDTPlatform ArmCpu; do
    local src; src=$(find "$S/$name" -name "$name.cpp" | head -1)
    PIANO_K27_TREE=$M python3 "$D/tooling/k27/build-native-kext.py" "$name" "$src" --out "$WORK/platform/$name" -I "$(dirname "$src")"
    xcrun libtool -static -o "$P/$name/lib$name.a" "$WORK/platform/$name"/*.o
  done
}

stage_kernel() {
  say "kernel (K27_VARIANT=native)"
  K27_VARIANT=native JOBS=$JOBS bash "$M/build-k27-piano.sh" > "$WORK/kernel.log" 2>&1 || { grep -E 'error:|Undefined' "$WORK/kernel.log" | head -30; exit 1; }
  local k=$X/build/xnu/k27-native-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped candidate
  if [ ! -f "$k" ]; then
    for candidate in "$k"*; do
      if [ -f "$candidate" ]; then k=$candidate; break; fi
    done
  fi
  [ -f "$k" ] || { echo "no unstripped kernel in ${k%/*}" >&2; exit 1; }
  cp "$k" "$OUT/kernel-k27-native.macho"
  cp "$k" "$M/out/k27/kernel-k27-$TOP-native.macho"   # the name tooling/k273/prepare-k27-tree.sh measures (TAG = K27_TREE_TOP)
  ( cd "$OUT" && shasum -a 256 kernel-k27-native.macho > kernel-k27-native.macho.sha256 && cat kernel-k27-native.macho.sha256 )
}

STAGES=${*:-tree xnu ld kdk fakeroot platform kernel}
for s in $STAGES; do "stage_$s"; done
