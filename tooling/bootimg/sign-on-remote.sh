#!/bin/bash
# AVB-sign RAM images the way the installed Ubuntu set is signed (xiaomipad-8pro-mainline build-persistent-boot.py: avbtool
# add_hash_footer, SHA256_RSA4096, partition "boot", rollback index and properties of boot-profile.json), with the key that
# signed this tablet's boot_a (AVB public key sha1 f5151930dc69791637b2a48505cb175f5f78a522). The private key never leaves
# the signing host: the unsigned image goes there, the signed one and the public key come back, the copies there are removed.
#   sign-on-remote.sh IMG...        -> IMG-signed.img next to each, plus out/keys/piano-boot-signing.{avbpubkey,pub.pem} (public halves)
#   sign-on-remote.sh --props-from IMG.avb.json IMG...
#                                   the rollback index, properties and partition size from that file instead of boot-profile.json:
#                                   tooling/switch/pack-selector.py writes it from the STOCK boot.img's AVB footer (the combined boot_a
#                                   must carry the stock boot patch level). Refused unless the file names partition boot.
# Environment: SIGN_HOST (default PIANO_SIGN_HOST from ~/.config/piano/env), SIGN_KEY (the key path on that host, default PIANO_SIGN_KEY
# from the same file). SIGN_HOST=local signs on this host, for a signing key this host holds (docs: no remote host here).
# SIGN_DRY=1 needs neither: it prints the AVB parameters and exits.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
# shellcheck source=/dev/null
[ -f "$HOME/.config/piano/env" ] && . "$HOME/.config/piano/env"
FIXED_PART=
if [ "${1:-}" = --props-from ]; then
  PF=${2:?--props-from needs a FILE.avb.json}; shift 2
  python3 -I - "$PF" <<'PY' || { echo "--props-from: $PF is not a boot-partition props file" >&2; exit 1; }
import json, sys
a = json.load(open(sys.argv[1]))['avb']
ok = a['partition_name'] == 'boot' and int(a['partition_size']) == 100663296 and int(a['rollback_index']) > 0 and a['props']
sys.exit(0 if ok and all(isinstance(k, str) and isinstance(v, str) for k, v in a['props'].items()) else 1)
PY
  RI=$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['avb']['rollback_index'])" "$PF")
  PROPS=$(python3 -I -c "import json,shlex,sys;print(' '.join('--prop '+shlex.quote(k+':'+v) for k,v in sorted(json.load(open(sys.argv[1]))['avb']['props'].items())))" "$PF")
  FIXED_PART=100663296
else
  PROFILE=${PIANO_V110:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/v1.1.0}/release/boot/boot-profile.json
  RI=$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['boot']['rollback_index'])" "$PROFILE")
  PROPS=$(python3 -I -c "import json,shlex,sys;print(' '.join('--prop '+shlex.quote(k+':'+v) for k,v in json.load(open(sys.argv[1]))['boot']['avb_properties'].items()))" "$PROFILE")
fi
[ "${SIGN_DRY:-0}" = 1 ] && { echo "SIGN_DRY rollback_index $RI props $PROPS partition ${FIXED_PART:-auto}"; exit 0; }
H=${SIGN_HOST:-${PIANO_SIGN_HOST:?set PIANO_SIGN_HOST (user@host of the signing host) in ~/.config/piano/env}}
K=${SIGN_KEY:-${PIANO_SIGN_KEY:?set PIANO_SIGN_KEY (the signing-key path on the signing host) in ~/.config/piano/env}}
V110=${PIANO_V110:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/v1.1.0}
AVB=${AVBTOOL:-$V110/upstream/xiaomipad-8pro-mainline-1.1.0/tools/local/aosp-avb/avbtool.py}
[ "$(shasum -a 256 "$AVB" | cut -c1-64)" = e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b ] || { echo "not the pinned avbtool 1.3.0" >&2; exit 1; }
R=/tmp/piano-sign.$$
# One host, two ways to reach it: over ssh, or this host when SIGN_HOST=local (the key never leaves either way).
if [ "$H" = local ]; then
  run() { bash -c "$*"; }
  push() { cp "$1" "$2"; }
  pull() { cp "$1" "$2"; }
else
  run() { ssh -o BatchMode=yes "$H" "$*"; }
  push() { scp -q "$1" "$H:$2"; }
  pull() { scp -q "$H:$1" "$2"; }
fi
run "mkdir -p $R"
trap 'run "rm -rf $R"' EXIT
push "$AVB" "$R/avbtool.py"
mkdir -p "$D/out/keys"
run "python3 $R/avbtool.py extract_public_key --key $K --output $R/pub.avbpubkey && openssl rsa -in $K -pubout -out $R/pub.pem 2>/dev/null"
pull "$R/pub.avbpubkey" "$D/out/keys/piano-boot-signing.avbpubkey"
pull "$R/pub.pem" "$D/out/keys/piano-boot-signing.pub.pem"
for img in "$@"; do
  n=$(basename "$img" .img)
  bytes=$(wc -c < "$img" | tr -d ' ')
  # The boot partition's 96 MiB when the image fits (the Ubuntu set's capacity), else just above the image: RAM boot only.
  part=100663296; [ "$bytes" -gt $((part - 1048576)) ] && part=$(( (bytes + 2097152 + 4095) / 4096 * 4096 ))
  # A flashable image (--props-from): exactly the boot partition, never a RAM-only size.
  [ -n "$FIXED_PART" ] && { [ "$part" = "$FIXED_PART" ] || { echo "$img does not fit the boot partition" >&2; exit 1; }; }
  # verify_image finds the partition's image as <partition_name>.img next to it: one directory per image.
  run "mkdir -p $R/$n"
  push "$img" "$R/$n/boot.img"
  run "python3 $R/avbtool.py add_hash_footer --image $R/$n/boot.img --partition_name boot --partition_size $part \
    --algorithm SHA256_RSA4096 --key $K --rollback_index $RI $PROPS && python3 $R/avbtool.py verify_image --image $R/$n/boot.img --key $R/pub.pem >/dev/null"
  pull "$R/$n/boot.img" "$(dirname "$img")/$n-signed.img"
  echo "$n-signed.img partition_size $part sha256 $(shasum -a 256 "$(dirname "$img")/$n-signed.img" | cut -c1-64)"
done
