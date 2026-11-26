#!/bin/bash
# Build package/NativeUfs (ShengUfs b11de87 + the SM8750/piano deltas) as a k27 native kext (objects for tooling/k27/kext/NativeUfs.json).
#   tooling/k27/build-native-ufs.sh        environment: PIANO_K27_TREE (the k27 tree, read only), UNIXNU_RESEARCH, K27_OBJ
# Objects: out/native-kexts/NativeUfs.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
R=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}
INC=$(mktemp -d); trap 'rm -rf "$INC"' EXIT
ln -s "$R/UniXNU-sm8750-common/kits/Sm8750Platform" "$INC/Sm8750Platform"
# PIANO_UFS_VERIFIED=1 (docs/drivers/ufs.md, the owner root path): the verified build instead. Every served block is then
# checked against the window's VerifiedRoot table and the window is published only after vr_open authenticated the owner-signed
# header, so the kernel's owner chain (kern_exec.c native_owner_cache_prepare) finds an authorized root. It needs the kit's two
# C sources (from UNIXNU_RESEARCH), the tree's VerifiedBlockPolicy.h and the policy source of these very windows,
# which the caller passes as PIANO_ROOT_POLICY_SOURCE. It is a
# compiled translation unit, not a pre-assembled object: the pointers of the policies array only reach the
# collection as authenticated fixups when clang emits them.
if [ "${PIANO_UFS_VERIFIED:-0}" = 1 ]; then
  V=${UNIXNU_RESEARCH:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/unixnu-research}/UniXNU/kits/VerifiedRoot
  [ -f "$V/src/verified_root.c" ] || { echo "no $V/src/verified_root.c (git submodule update --init kits/VerifiedRoot)" >&2; exit 1; }
  [ -f "${PIANO_ROOT_POLICY_SOURCE:-}" ] || { echo "PIANO_ROOT_POLICY_SOURCE must name the generated policy source (tooling/k27/make-root-policy-source.py)" >&2; exit 1; }
  python3 "$D/tooling/k27/build-native-kext.py" NativeUfs \
    "$D/package/NativeUfs/NativeUfs.cpp" "$D/tooling/k27/NativeKextGlue.cpp" "$V/src/verified_root.c" "$V/adapters/xnu_crypto.c" "$D/tooling/k27/BootProgressPoll.c" "$PIANO_ROOT_POLICY_SOURCE" \
    -I "$D/package/NativeUfs" -I "$INC" -I "$K/out/x12377/storage-abi/include" -I "$K/out/x12377/xnu-m/osfmk" \
    -I "$K/overlay/droidvm/drivers/NativeVirtioBlock" -I "$V/include" \
    -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 -D NATIVE_UFS_SM8750=1 -D 'UFS_ROOT_PARTITION="linux_rootfs"' -D NATIVE_UFS_POLL_ONLY=1 \
    -D NATIVE_VERIFIED_BLOCK=1 --obj "${K27_OBJ:-k27-native-obj}"
  exit $?
fi
# The development build (NATIVE_UFS_UNVERIFIED_ROOT) needs neither VerifiedRoot nor NativeVirtioBlock's VerifiedBlockPolicy.h.
python3 "$D/tooling/k27/build-native-kext.py" NativeUfs "$D/package/NativeUfs/NativeUfs.cpp" "$D/tooling/k27/NativeKextGlue.cpp" \
  -I "$D/package/NativeUfs" -I "$INC" -I "$K/out/x12377/storage-abi/include" -I "$K/out/x12377/xnu-m/osfmk" \
  -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 -D NATIVE_UFS_SM8750=1 -D 'UFS_ROOT_PARTITION="linux_rootfs"' -D NATIVE_UFS_POLL_ONLY=1 -D NATIVE_UFS_UNVERIFIED_ROOT=1 \
  --obj "${K27_OBJ:-k27-native-obj}"
