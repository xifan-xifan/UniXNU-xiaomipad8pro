#!/bin/bash
# One command after a NativeUfs/NativeRamBlock or k27 change: native kexts -> KC, the ADT -> the macOS RAM image (FD A) in out/ram, and the
# loop's dump image in out/ram-dump (the same with piano-reset-after=1: it only lets our UEFI read the previous run's log).
set -euo pipefail
# The kext name list is split on whitespace, with pathname expansion disabled.
set -f
D=$(cd "$(dirname "$0")/../.." && pwd); cd "$D"
# shellcheck source=tooling/k27/k27-tree.sh
. "$D/tooling/k27/k27-tree.sh"; K=$PIANO_K27_TREE
mkdir -p out/k27
LIST=$(awk '{sub(/#.*/, ""); if (NF) print $1}' "${PIANO_KEXT_LIST:-configs/k27-native-kexts.txt}")
LIST=${LIST//$'\n'/ }
# PIANO_BASE_SEED / PIANO_BASE_TRUST: the OS line's Data seed and trust cache the stages start from (default out/inputs/: the 27.0.1
# line's; the 27.3 line's are made by tooling/k273/, docs/k273-line.md).
BASE_SEED=${PIANO_BASE_SEED:-out/inputs/scratch-data.img}
BASE_TRUST=${PIANO_BASE_TRUST:-out/inputs/kernel.macho.trustcache}
# PIANO_USBHOST_STACK=1: Apple's USB host stack in the collection for PianoUsbHost level 6 (docs/drivers/usb-host.md H6, path A1):
# PianoUsbHostStandIn (the seven Apple SoC kexts the stack reaches) and the codeless PianoUsbHostStack (its libraries keep AppleUSBXHCI,
# IOUSBHostFamily, the hub, HID, mass storage and USB Ethernet; AppleUSBXHCI's personality on PianoUsbHostNub; the stack's IOResources
# personalities behind the gate resource PianoUsbHostStack, which only piano-usbhost=6 publishes), ARMPlatformStandIn with
# AppleARMPlatform's cable keys. The stack's kexts keep their collection addresses, which leaves no __DATA_CONST gap for PianoTouch
# (485 KiB) after PianoA830 (it links first: PianoTouch imports its lock word) with first-fit placement, so PIANO_USBHOST_STACK_DROP
# (default PianoTouch: inert without piano-touch=N) is left out; with patches/k27-kc/0001 in the k27 tree's KC builder (best-fit
# placement, docs/kc-space.md) it fits, and the default is to drop nothing. Checked after the link: tooling/k27/check-usbhost-stack.py.
# PIANO_USBHOST_A2=1 (docs/drivers/usb-host.md §5.4, path A2) implies the stack and replaces A1's codeless personality with our own
# subclass PianoXhciHost (three overrides of the collection's AppleUSBXHCI; it carries the stack's libraries and the gate, so the
# codeless bundle loses its personality: PianoUsbHostStackA2). Exactly one automatic client on the nub either way
# (check-usbhost-stack.py refuses two). The A2 subclass is matched by piano-usbhost=6 piano-usbhost-a2=1.
# PIANO_KEXT_PROFILE=usbhost-a2 (U1, the first attended real-device USB host session; default: no profile, the list as it is): PIANO_USBHOST_A2=1
# on the WHOLE list (nothing dropped: PianoTouch, PianoCamera and PianoDp stay), i.e. the list's kexts + PianoUsbHostStackA2 +
# PianoXhciHost. The stack bundle's libraries bring Apple's hub (AppleUSBHub), composite, HID (IOUSBHostHIDDevice), mass-storage
# (IOUSBMassStorageDriver + the SCSI/storage families) and USB-Ethernet class drivers, their boot personalities behind the gate resource
# PianoUsbHostStack, which only PianoUsbHost's handoff 6 publishes: inert without piano-usbhost=4 piano-usbhost-handoff=6.
case ${PIANO_KEXT_PROFILE:-} in
  '') ;;
  usbhost-a2) PIANO_USBHOST_A2=1; PIANO_USBHOST_STACK_DROP=; echo "PIANO_KEXT_PROFILE=usbhost-a2: the A2 USB host stack on the whole list" >&2 ;;
  *) echo "PIANO_KEXT_PROFILE=$PIANO_KEXT_PROFILE: unknown (usbhost-a2, or unset)" >&2; exit 1 ;;
esac
# PIANO_KC_ONLY=1: stop after the collection and its checks (out/k27/kc-k27-native.bin, build-kc.log); no seed staging, no image. For
# host checks of a kext list (e.g. a profile) without a boot image.
KC_ONLY=${PIANO_KC_ONLY:-0}
if [ "${PIANO_USBHOST_A2:-0}" = 1 ]; then
  export PIANO_USBHOST_A2
  PIANO_USBHOST_STACK=1
fi
if [ "${PIANO_USBHOST_STACK:-0}" = 1 ]; then
  export PIANO_USBHOST_STACK
  DROP_DEFAULT=PianoTouch
  if grep -q KC_NATIVE_PLACEMENT "$K/scripts/kc-build.py" && [ "${KC_NATIVE_PLACEMENT:-best-fit}" = best-fit ]; then DROP_DEFAULT=; fi
  KEPT=
  for k in $LIST; do case " ${PIANO_USBHOST_STACK_DROP-$DROP_DEFAULT} " in *" $k "*) echo "PIANO_USBHOST_STACK=1: $k left out" >&2 ;; *) KEPT="$KEPT $k" ;; esac; done
  if [ "${PIANO_USBHOST_A2:-0}" = 1 ]; then
    LIST="$KEPT PianoUsbHostStackA2 PianoXhciHost"
  else
    LIST="$KEPT PianoUsbHostStack"
  fi
fi
BT_STACK_CHECK=0
for k in $LIST; do   # the kexts' objects: tooling/k27/build-native-<name>.sh
  [ "$k" != PianoBluetooth ] || BT_STACK_CHECK=1
  case $k in PianoUsbHostStack|PianoUsbHostStackA2) continue ;; esac   # codeless (a plist entry only): nothing to build
  case $k in ARMPlatformStandIn) b=standin ;; NativeUfs) b=ufs ;; NativeRamBlock) b=ram-block ;; NativeGopDisplay) b=gop-display ;; PianoDwc3) b=piano-dwc3 ;; PianoInput) b=piano-input ;; PianoNullEthernet) b=null-ethernet ;; PianoPower) b=piano-power ;; NativeSoftGpu) b=soft-gpu ;; PianoA830) b=piano-a830 ;; PianoBacklight) b=piano-backlight ;; PianoKeyboard) b=piano-keyboard ;; PianoWifi) b=piano-wifi ;; PianoWifiEthernet) b=piano-wifi-ethernet ;; PianoAudio) b=piano-audio ;; PianoTouch) b=piano-touch ;; PianoThermal) b=piano-thermal ;; PianoUsbHost) b=piano-usbhost ;; PianoDpu) b=piano-dpu ;; PianoDp) b=piano-dp ;; PianoVideo) b=piano-video ;; PianoTorch) b=piano-torch ;; PianoIR) b=piano-ir ;; PianoCamera) b=piano-camera ;; PianoSensors) b=piano-sensors ;; PianoBluetooth) b=piano-bluetooth ;; PianoPolicy) b=piano-policy ;; PianoUsbHostStandIn) b=usbhost-standin ;; PianoXhciHost) b=piano-xhcihost ;; SoftwareAES) b=software-aes ;; *) b= ;; esac
  [ -n "$b" ] || { echo "no build script for $k" >&2; exit 1; }
  tooling/k27/build-native-$b.sh > "out/k27/build-native-$b.log" 2>&1 || { tail -20 "out/k27/build-native-$b.log" >&2; exit 1; }
done
# PIANO_STOP_AFTER=kexts: the kexts' objects only (out/native-kexts; CI's tooling/ci/build-kexts.sh), no collection, ADT or image.
if [ "${PIANO_STOP_AFTER:-}" = kexts ]; then echo "kexts built (out/native-kexts), PIANO_STOP_AFTER=kexts"; exit 0; fi
# A descriptor of a kext the list already links (e.g. PIANO_EXTRA_KEXTS=.../PianoA830.json from before PianoA830 was listed) is not
# linked twice.
EXTRA=${PIANO_EXTRA_KEXTS:-}
EXTRA_PATHS=()
if [ -f "$EXTRA" ]; then
  EXTRA_PATHS=("$EXTRA")
else
  read -r -a EXTRA_PATHS <<< "${EXTRA//$'\n'/ }"
fi
EXTRA_NATIVE=()
for e in ${EXTRA_PATHS[@]+"${EXTRA_PATHS[@]}"}; do
  name=${e##*/}
  name=${name%.json}
  case " $LIST " in
    *" $name "*) echo "PIANO_EXTRA_KEXTS: $name is in the kext list already" >&2; continue ;;
  esac
  case $e in /*) ;; *) e=$D/$e ;; esac
  EXTRA_NATIVE+=("$e")
done
# PIANO_GPU=N (default 0): piano-gpu=N and piano-gpu-delay=${PIANO_GPU_DELAY:-20} in the images' arguments (docs/drivers/gpu.md §6).
# PianoA830 is in the kext list and inert without them. A GPU session also needs the NativeAdrenoMetal plugin in the Data seed (the
# userspace side's staging) and NativeSoftGpu out of the list; while the default seed lacks the plugin this default stays 0 (an
# accelerator whose MetalPluginName bundle is missing sends WindowServer back into the crash loop of the VM-era stub).
if [ "${PIANO_GPU:-0}" != 0 ]; then
  export PIANO_EXTRA_ARGS="piano-gpu=$PIANO_GPU piano-gpu-delay=${PIANO_GPU_DELAY:-20} ${PIANO_EXTRA_ARGS:-}"
fi
if [ "$KC_ONLY" != 1 ]; then
# PIANO_VMM_PRESENT=0|1: the ADT's /defaults vmm-present for this build (else configs/k27-adt.toml's; docs/drivers/soft-metal.md §9).
# PIANO_SENSOR_PACK=PACK (docs/drivers/sensors.md §5b): the RAM images carry this unit's sensor pack for PianoSensors (DarwinBoot's
# addon slot, build-ram-images.py); their ADT then has the GpuAddon slot and is made as out/adt/piano-k27-addon.adt, so the ADT an ESP
# takes (out/adt/piano-k27.adt) never has it (build-esp.py refuses one that does).
ADT_OUT=out/adt/piano-k27.adt
if [ -n "${PIANO_SENSOR_PACK:-}" ]; then ADT_OUT=out/adt/piano-k27-addon.adt; export PIANO_SENSOR_PACK; fi
# PIANO_RAM_WINDOW=BYTES: XNU's RAM window for this build (else configs/k27-adt.toml's owned_memory_size, 13 GiB less 2 MiB, which
# needs an FD with firmware/patches/darwinboot/0001: tooling/firmware/build-fd.sh; 0x200000000 for an older FD).
python3 tooling/adt/make-piano-adt.py "$ADT_OUT" --unique-chip-id "${UNIQUE_CHIP_ID:-47554e5941480001}" --data --trust \
  ${PIANO_SENSOR_PACK:+--addon} ${PIANO_VMM_PRESENT:+--vmm-present "$PIANO_VMM_PRESENT"} ${PIANO_RAM_WINDOW:+--ram-window "$PIANO_RAM_WINDOW"} > /dev/null
# PIANO_IDENTITY=vma2|piano|k114c (docs/identity.md): the machine identity of this build's images. vma2 (the default) is
# configs/k27-adt.toml's VirtualMac2,1; another is configs/k27-identity/NAME.toml applied by tooling/adt/set-identity.py to a copy,
# out/adt/piano-k27-id-NAME.adt (the images in out/ram take it), so out/adt/piano-k27.adt, the one an ESP takes, keeps vma2
# (build-esp.py also refuses an ADT with an identity mark unless PIANO_IDENTITY_INSTALL=1).
if [ "${PIANO_IDENTITY:-vma2}" != vma2 ]; then
  ID_ADT=out/adt/piano-k27-id-$PIANO_IDENTITY.adt
  [ -z "${PIANO_SENSOR_PACK:-}" ] || ID_ADT=out/adt/piano-k27-addon-id-$PIANO_IDENTITY.adt
  python3 tooling/adt/set-identity.py "$ADT_OUT" "$ID_ADT" --identity "$PIANO_IDENTITY" | head -1
  ADT_OUT=$ID_ADT
fi
# The software Metal path (docs/drivers/soft-metal.md): when NativeSoftGpu is linked (the list or PIANO_EXTRA_KEXTS; PIANO_SOFT_METAL=0
# leaves this out, =1 forces it), the NativeSoftMetal plugin is built and staged into a copy of the Data seed and the trust cache
# (tooling/k27/stage-soft-metal.py, out/soft-metal), which the image then takes unless PIANO_SEED / PIANO_TRUST name others. The staging
# is redone only when its inputs change (out/soft-metal/stamp): a new seed is a new seed identity, which the installed Data store was not
# made for (docs/data-store.md: the store guard keeps that store and runs Data volatile; build-ram-images.py gives such an image
# data-persist=0 unless PIANO_DATA_PERSIST says otherwise). PIANO_SOFT_METAL_LOG=PREDICATE adds the error/fault log loop
# (stage-soft-metal.py --log-errors) to the seed's boot job.
# Reduce transparency is staged too (clients draw no backdrop layers, which Core Animation's software renderer asserts on):
# PIANO_REDUCE_TRANSPARENCY=1 (default) writes it through the seed's boot job, =files writes it into the seed itself root:wheel
# (sudo; for a seed with no boot job), =0 leaves it out.
SOFT_METAL=${PIANO_SOFT_METAL:-}
if [ -z "$SOFT_METAL" ]; then
  case " $LIST ${PIANO_EXTRA_KEXTS:-} " in *NativeSoftGpu*) SOFT_METAL=1 ;; *) SOFT_METAL=0 ;; esac
fi
if [ "$SOFT_METAL" = 1 ]; then
  tooling/k27/build-soft-metal-plugin.sh > out/k27/build-soft-metal-plugin.log 2>&1 || { grep -E "error" out/k27/build-soft-metal-plugin.log | head; exit 1; }
  STAMP=$( { shasum -a 256 "$BASE_SEED" "$BASE_TRUST" tooling/k27/stage-soft-metal.py
             find out/soft-metal/NativeSoftMetal.bundle -type f | sort | xargs shasum -a 256; echo "log ${PIANO_SOFT_METAL_LOG:-} rt ${PIANO_REDUCE_TRANSPARENCY:-1}"; } | shasum -a 256 | cut -c1-16)
  if [ "$(cat out/soft-metal/stamp 2>/dev/null)" != "$STAMP" ] || [ ! -f out/soft-metal/scratch-data.img ]; then
    # 1 (default): the seed's boot job writes it, which needs org.droidvm.run.0.plist in the seed.
    # files: stage-soft-metal.py writes it into the seed root:wheel instead (sudo), for a seed without that job - the 27.3
    # verified-root seed (out/build-softmetal-8748.sh) is one of those. 0: stage nothing.
    RT=
    case "${PIANO_REDUCE_TRANSPARENCY:-1}" in
      0) ;;
      files) RT=--reduce-transparency-files ;;
      *) RT=--reduce-transparency ;;
    esac
    python3 tooling/k27/stage-soft-metal.py --seed "$BASE_SEED" --trust "$BASE_TRUST" ${PIANO_SOFT_METAL_LOG:+--log-errors "$PIANO_SOFT_METAL_LOG"} $RT > out/k27/stage-soft-metal.log 2>&1 \
      || { tail -5 out/k27/stage-soft-metal.log; exit 1; }
    echo "$STAMP" > out/soft-metal/stamp
  fi
  PIANO_SEED=${PIANO_SEED:-out/soft-metal/scratch-data.img}
  PIANO_TRUST=${PIANO_TRUST:-out/soft-metal/kernel.macho.trustcache}
fi
# The A830 GPU path (docs/drivers/gpu-userspace.md): when PianoA830 is linked (the list or PIANO_EXTRA_KEXTS; PIANO_A830_METAL=0
# leaves this out, =1 forces it), the VM's NativeAdrenoMetal.bundle with Turnip rebuilt on Adreno830's ABI v2
# (tooling/metal/build-turnip-a830.sh) is staged into a copy of the Data seed and the trust cache (out/turnip-a830/stage), which the
# image then takes unless PIANO_SEED / PIANO_TRUST name others; redone only when its inputs change (out/turnip-a830/stage/stamp).
# PIANO_A830_TU_DEBUG (default sysmem: no GMEM for the first bring-up) is staged as /private/var/ngpu/debug/tu-debug, Turnip's
# TU_DEBUG in every Metal process without one; PIANO_A830_TU_DEBUG= stages none. NativeSoftGpu and PianoA830 are exclusive: Metal
# takes the first accelerator it finds, and SkyLight the first device's port.
A830_METAL=${PIANO_A830_METAL:-}
if [ -z "$A830_METAL" ]; then
  case " $LIST ${PIANO_EXTRA_KEXTS:-} " in *PianoA830*) A830_METAL=1 ;; *) A830_METAL=0 ;; esac
fi
if [ "$A830_METAL" = 1 ]; then
  case "$SOFT_METAL $LIST ${PIANO_EXTRA_KEXTS:-} " in 1*|*NativeSoftGpu*)
    echo "NativeSoftGpu (or PIANO_SOFT_METAL=1) and PianoA830 together: Metal takes the first accelerator; leave NativeSoftGpu out" >&2
    exit 1 ;; esac
  tooling/metal/build-turnip-a830.sh > out/k27/build-turnip-a830.log 2>&1 \
    || { grep -E "error|FAILED|^no |not ABI" out/k27/build-turnip-a830.log | head; exit 1; }
  A830_TU_DEBUG=${PIANO_A830_TU_DEBUG-sysmem}
  # PIANO_A830_LOG=PREDICATE: the seed's boot job also prints the os_log errors and faults of the processes PREDICATE names
  # (stage-soft-metal.py --log-errors, as PIANO_SOFT_METAL_LOG): a WindowServer, Dock or Finder that aborts after the login says why
  # there (SkyLight, Core Animation, Metal), not on stderr. Off by default; a changed predicate is a new seed.
  STAMP=$( { shasum -a 256 "$BASE_SEED" "$BASE_TRUST" tooling/k27/stage-soft-metal.py
             find out/turnip-a830/NativeAdrenoMetal.bundle -type f | sort | xargs shasum -a 256; echo "tu-debug $A830_TU_DEBUG"
             if [ -n "${PIANO_A830_LOG:-}" ]; then echo "log $PIANO_A830_LOG"; fi; } | shasum -a 256 | cut -c1-16)
  if [ "$(cat out/turnip-a830/stage/stamp 2>/dev/null)" != "$STAMP" ] || [ ! -f out/turnip-a830/stage/scratch-data.img ]; then
    FILES=()
    if [ -n "$A830_TU_DEBUG" ]; then
      printf '%s\n' "$A830_TU_DEBUG" > out/turnip-a830/tu-debug
      FILES=(--file out/turnip-a830/tu-debug:private/var/ngpu/debug/tu-debug)
    fi
    python3 tooling/k27/stage-soft-metal.py --seed "$BASE_SEED" --trust "$BASE_TRUST" --out out/turnip-a830/stage ${PIANO_A830_LOG:+--log-errors "$PIANO_A830_LOG"} \
      ${FILES[@]+"${FILES[@]}"} out/turnip-a830/NativeAdrenoMetal.bundle \
      > out/k27/stage-turnip-a830.log 2>&1 || { tail -5 out/k27/stage-turnip-a830.log; exit 1; }
    echo "$STAMP" > out/turnip-a830/stage/stamp
  fi
  PIANO_SEED=${PIANO_SEED:-out/turnip-a830/stage/scratch-data.img}
  PIANO_TRUST=${PIANO_TRUST:-out/turnip-a830/stage/kernel.macho.trustcache}
fi
fi
# The descriptors with their objects' absolute place (@OUT@ = this repository's out/), plus PIANO_EXTRA_KEXTS (JSON paths) for a test.
mkdir -p out/k27/kext
# Escape the path for a JSON string, then for the sed replacement.
OUT_JSON=$(python3 -c 'import json, sys; print(json.dumps(sys.argv[1])[1:-1])' "$D/out")
OUT_SED=$(printf '%s' "$OUT_JSON" | sed 's/[\\&#]/\\&/g')
DESCRIPTORS=()
for k in $LIST; do sed "s#@OUT@#$OUT_SED#g" "tooling/k27/kext/$k.json" > "out/k27/kext/$k.json"; DESCRIPTORS+=("$D/out/k27/kext/$k.json"); done
# When an explicit USB host profile owns the shared consumer libraries, retain
# its original personalities and gate checks. Bluetooth owns only its Family.
if [ "${PIANO_USBHOST_STACK:-0}" = 1 ]; then
  python3 - out/k27/kext/PianoBluetooth.json <<'PY'
import json, sys
from pathlib import Path
p = Path(sys.argv[1])
if p.is_file():
    d = json.loads(p.read_text())
    d['patch_personalities'] = {k: v for k, v in d.get('patch_personalities', {}).items()
                              if k == 'com.apple.iokit.IOBluetoothFamily'}
    p.write_text(json.dumps(d, indent=2) + '\n')
PY
fi
NATIVE='' tooling/kc/build-kc-k27.sh "${K27_KERNEL:-$K/out/k27/kernel-k27-0059-native.macho}" out/k27/kc-k27-native.bin \
  ${DESCRIPTORS[@]+"${DESCRIPTORS[@]}"} ${EXTRA_NATIVE[@]+"${EXTRA_NATIVE[@]}"} > out/k27/build-kc.log 2>&1 || { tail -5 out/k27/build-kc.log; exit 1; }
# The Bluetooth daemon requires the real consumer UC, while its hardware dependencies
# must stay inert on this board. Check the linked metadata, not just the descriptor.
if [ "$BT_STACK_CHECK" = 1 ]; then
  python3 tooling/k27/check-bluetooth-stack.py out/k27/kc-k27-native.bin >&2 || exit 1
fi
# Apple's USB host stack (PIANO_USBHOST_STACK=1) must be inert without piano-usbhost=6: gated personalities only.
if [ "${PIANO_USBHOST_STACK:-0}" = 1 ]; then
  python3 tooling/k27/check-usbhost-stack.py out/k27/kc-k27-native.bin >&2 || { rm -f out/k27/kc-k27-native.bin; exit 1; }
fi
if [ "$KC_ONLY" = 1 ]; then echo "PIANO_KC_ONLY=1: out/k27/kc-k27-native.bin ($(wc -c < out/k27/kc-k27-native.bin | tr -d ' ') bytes), no image" >&2; exit 0; fi
# PIANO_TRUST / PIANO_SEED: another trust cache and Data seed for a test (e.g. tooling/k27/stage-soft-metal.py's, with a staged plugin).
# The Data store (docs/data-store.md): out/ram opens it (data-persist=2) only when its seed is the installed one (PIANO_INSTALLED_SEED in
# ~/.config/piano/env), else Data is volatile; build-ram-images.py prints which on stderr. PIANO_DATA_PERSIST=0|1|2 overrides it,
# PIANO_DATA_STORE_REPLACE=1|<8 hex> and PIANO_DATA_STORE_FLATTEN=1 add data-store-replace / data-store-flatten=1.
COMMON=(--fd out/fd/piano-uefi-A-bundled.fd --fd-name A-bundled --kc out/k27/kc-k27-native.bin --trust "${PIANO_TRUST:-$BASE_TRUST}"
        --seed "${PIANO_SEED:-$BASE_SEED}" --adt "${PIANO_ADT:-$ADT_OUT}" --utc "$(date +%s)")
# The dump image only lets our UEFI read the previous run's log; its XNU never touches the Data store (data-persist=0), so a dump
# image with another seed cannot reset the store of the installed system (docs/persistent-boot.md).
PIANO_RESET_AFTER=1 PIANO_DATA_PERSIST=0 env -u PIANO_DATA_STORE_REPLACE -u PIANO_DATA_STORE_FLATTEN python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram-dump > out/k27/build-ram-dump.log 2>&1 &
DUMP_PID=$!
# RAM16G R1 uses an opt-in placeholder in the primary RAM payload.
if [ "${PIANO_EXTRA_RAM:-0}" = 1 ]; then COMMON+=(--extra-ram); fi
RAM_STATUS=0
python3 tooling/bootimg/build-ram-images.py "${COMMON[@]}" --out out/ram | tail -1 || RAM_STATUS=$?
wait "$DUMP_PID" || { tail -3 out/k27/build-ram-dump.log >&2; exit 1; }
exit "$RAM_STATUS"
