#!/bin/bash
# AVB-sign RAM images the way the installed Ubuntu set is signed (xiaomipad-8pro-mainline build-persistent-boot.py: avbtool
# add_hash_footer, SHA256_RSA4096, partition "boot", rollback index and properties of boot-profile.json), with the key that
# signed this tablet's boot_a (AVB public key sha1 f5151930dc69791637b2a48505cb175f5f78a522). The private key stays on the
# signing host: the unsigned image goes there, the signed one and the public key come back, the copies there are removed.
#   sign-on-remote.sh IMG...        -> IMG-signed.img next to each, plus out/keys/piano-boot-signing.{avbpubkey,pub.pem} (public halves)
#   sign-on-remote.sh --props-from pack-selector.py's OUT.img.avb.json IMG...
#       the rollback index, the AVB properties and the partition size come from the packed image's own record, because a combined
#       boot_a has to keep the stock image's patch level: that lives only in the stock AVB footer. Refused for another partition,
#       a zero rollback index, no properties, or another partition size than the 96 MiB boot partition.
# Environment: SIGN_HOST (required, user@build-host; "local" signs on this host, for a key this host holds), SIGN_KEY (path to the
# key on that host), AVBTOOL (path to the pinned avbtool.py), AVBTOOL_PIN (its expected SHA-256; the actual one is always printed),
# SIGN_DRY (1: print the avbtool plan and exit, so the option handling is testable without a key or a host).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
PROPS_FROM=
while [ $# -gt 0 ] && [ "${1#--}" != "$1" ]; do
  case "$1" in
    --props-from) [ $# -ge 2 ] || { echo "--props-from needs a file" >&2; exit 2; }; PROPS_FROM=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [ -n "$PROPS_FROM" ]; then
  eval "$(python3 - "$PROPS_FROM" <<'PY'
import json, shlex, sys
rec = json.load(open(sys.argv[1]))["avb"]
errs = []
if rec["partition_name"] != "boot": errs.append(f"props of partition {rec['partition_name']}, want boot")
if rec["partition_size"] != 100663296: errs.append(f"partition_size {rec['partition_size']}, want the boot partition's 100663296")
if not rec["rollback_index"]: errs.append(f"rollback_index {rec['rollback_index']}, a stock image's is never 0")
if not rec["props"]: errs.append("no AVB properties: the stock patch level would be lost")
if errs:
    sys.exit("; ".join(errs))
print("RI=%d" % rec["rollback_index"])
print("PROPS=%s" % shlex.quote(" ".join("--prop " + shlex.quote(k + ":" + v) for k, v in sorted(rec["props"].items()))))
print("PART=%d" % rec["partition_size"])
PY
)"
  [ $# -gt 0 ] || { echo "need at least one image" >&2; exit 2; }
  echo "props-from $PROPS_FROM: rollback_index $RI $PROPS partition $PART"
  if [ "${SIGN_DRY:-0}" = 1 ]; then exit 0; fi
fi
if [ "${SIGN_DRY:-0}" = 1 ]; then exit 0; fi
H=${SIGN_HOST:?set SIGN_HOST to user@build-host (or local)}
K=${SIGN_KEY:?set SIGN_KEY to the signing-key path on SIGN_HOST}
AVB=${AVBTOOL:?set AVBTOOL to the pinned avbtool.py}
PIN=${AVBTOOL_PIN:-e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b}
SUM=$(sha256sum "$AVB" 2>/dev/null | cut -c1-64 || shasum -a 256 "$AVB" | cut -c1-64)
[ "$SUM" = "$PIN" ] || { echo "avbtool sha256 $SUM, not the pinned $PIN (set AVBTOOL_PIN to sign with this one)" >&2; exit 1; }
LOCAL=0; [ "$H" = local ] && LOCAL=1
run() { if [ "$LOCAL" = 1 ]; then bash -c "$1"; else ssh -o BatchMode=yes "$H" "$1"; fi; }
push() { if [ "$LOCAL" = 1 ]; then cp "$1" "$2"; else scp -q "$1" "$H:$2"; fi; }
pull() { if [ "$LOCAL" = 1 ]; then cp "$1" "$2"; else scp -q "$H:$1" "$2"; fi; }
if [ -z "$PROPS_FROM" ]; then
  PROFILE=${PIANO_V110:-$D/../v1.1.0}/release/boot/boot-profile.json
  RI=$(python3 -c "import json;print(json.load(open('$PROFILE'))['boot']['rollback_index'])")
  PROPS=$(python3 -c "import json,shlex;print(' '.join('--prop '+shlex.quote(k+':'+v) for k,v in json.load(open('$PROFILE'))['boot']['avb_properties'].items()))")
fi
R=/tmp/piano-sign.$$
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
  part=${PART:-100663296}
  if [ -z "${PART:-}" ] && [ "$bytes" -gt $((part - 1048576)) ]; then part=$(( (bytes + 2097152 + 4095) / 4096 * 4096 )); fi
  # verify_image finds the partition's image as <partition_name>.img next to it: one directory per image.
  run "mkdir -p $R/$n"
  push "$img" "$R/$n/boot.img"
  run "python3 $R/avbtool.py add_hash_footer --image $R/$n/boot.img --partition_name boot --partition_size $part \
    --algorithm SHA256_RSA4096 --key $K --rollback_index $RI $PROPS && python3 $R/avbtool.py verify_image --image $R/$n/boot.img --key $R/pub.pem >/dev/null"
  pull "$R/$n/boot.img" "$(dirname "$img")/$n-signed.img"
  echo "$n-signed.img partition_size $part sha256 $(sha256sum "$(dirname "$img")/$n-signed.img" | cut -c1-64)"
done