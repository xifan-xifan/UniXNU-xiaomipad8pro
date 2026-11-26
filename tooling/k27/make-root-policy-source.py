#!/usr/bin/env python3
"""Turn a VerifiedRoot policy directory into the C++ source a kernel collection compiles as the root policies.

  make-root-policy-source.py POLICY_DIR --out DIR [--name WINDOW]...

POLICY_DIR holds one directory per root window, each made by the tools/manifest.py script of the VerifiedRoot kit
(through the make-root-policies.py wrapper of UniXNU): manifest.header.bin (VR_HEADER_BYTES + VR_SIGNATURE_BYTES, the
signed envelope without the block table, which travels as the NAME.vroot object of the root image), trusted-key.bin
(the 395-byte owner anchor) and verification.json. Every window must carry the same anchor, because NativeUfs keeps
one key for the set.

The output is root-policies.cpp, which compiles inside the NativeUfs kext (tooling/k27/build-native-ufs.sh with
PIANO_UFS_VERIFIED=1) and defines the three symbols NativeUfs reads under NATIVE_VERIFIED_BLOCK. It is a compiled
translation unit and not hand-written assembly on purpose: the policies array holds pointers, and on arm64e a pointer
of a native kext has to reach the collection as an authenticated fixup. A .quad of a label in assembly yields a plain
rebase, which the collection carries in its linkedit but the kernel never binds, so the first read of the pointer
faulted (docs/results-2026-11-26-k273-verified-root-owner-cache.md).

--name selects the windows and their order (default: every directory of POLICY_DIR with a header). Name them as the
windows of the root manifest: NativeUfs takes the policy whose signed capacity equals that of the window and reads the
hash table from the image object named after the window.
"""
import argparse, hashlib, json, struct, sys
from pathlib import Path

HEADER_BYTES, SIGNATURE_BYTES, KEY_BYTES, BLOCK_BYTES = 160, 384, 395, 4096
ENVELOPE = HEADER_BYTES + SIGNATURE_BYTES
KEY_MAGIC = b"VROOTKEY"
HEADER_MAGIC = b"VROOT01" + bytes(1)


def check_window(window_dir, key):
    header = (window_dir / "manifest.header.bin").read_bytes()
    anchor = (window_dir / "trusted-key.bin").read_bytes()
    name = window_dir.name
    record = json.loads((window_dir / "verification.json").read_text())
    volume = record["volume_uuid"]
    want_image = record["image_sha256"]
    want_blocks = int(record["blocks_verified"])
    if anchor != key:
        raise SystemExit(name + ": its trusted-key.bin is not the anchor of the set")
    if len(header) != ENVELOPE:
        raise SystemExit(f"{name}: manifest.header.bin is {len(header)} bytes, {ENVELOPE} is the header-only envelope")
    if header[:8] != HEADER_MAGIC:
        tail = repr(HEADER_MAGIC)
        raise SystemExit(f"{name}: the envelope does not start with {tail}")
    if struct.unpack_from("<Q", header, 152)[0] < 1:
        raise SystemExit(name + ": the signed generation is not positive")
    if header[40:56] != bytes.fromhex(volume.replace("-", "")):
        raise SystemExit(name + ": the envelope signs another volume UUID than verification.json")
    image_bytes = struct.unpack_from("<Q", header, 24)[0]
    block_count = struct.unpack_from("<Q", header, 32)[0]
    image_sha256 = header[56:88].hex()
    if want_image != image_sha256:
        raise SystemExit(f"{name}: the header signs {image_sha256}, verification.json says {want_image}")
    if want_blocks != block_count:
        raise SystemExit(f"{name}: the header signs {block_count} blocks, verification.json says {want_blocks}")
    if block_count * BLOCK_BYTES < image_bytes or (block_count - 1) * BLOCK_BYTES >= image_bytes:
        raise SystemExit(f"{name}: {block_count} blocks cannot cover {image_bytes} bytes")
    if not record.get("real_c_signature_and_block_verification"):
        raise SystemExit(name + ": verification.json does not record a real signature and block check")
    return {"name": name, "header": header, "image_bytes": image_bytes, "blocks": block_count,
            "image_sha256": image_sha256, "volume_uuid": volume}


def hex_lines(name, blob, per_line=12):
    out = [f"static const uint8_t {name}[] = {{"]
    for at in range(0, len(blob), per_line):
        words = ", ".join("0x%02x" % b for b in blob[at:at + per_line])
        out.append("    " + words + ",")
    return out + ["};", ""]


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("policy_dir", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--name", action="append", default=[], metavar="WINDOW")
    a = p.parse_args(argv)
    if not a.policy_dir.is_dir():
        raise SystemExit(f"{a.policy_dir}: no such policy directory")
    found = sorted(d.name for d in a.policy_dir.iterdir() if (d / "manifest.header.bin").is_file())
    names = a.name or found
    if not names:
        raise SystemExit(f"{a.policy_dir}: no directory with a manifest.header.bin")
    key = (a.policy_dir / names[0] / "trusted-key.bin").read_bytes()
    if len(key) != KEY_BYTES or not key.startswith(KEY_MAGIC):
        raise SystemExit(names[0] + ": trusted-key.bin is not the anchor of the policy set")
    windows = [check_window(a.policy_dir / name, key) for name in names]
    capacities = [w["image_bytes"] for w in windows]
    if len(set(capacities)) != len(capacities):
        raise SystemExit("two windows carry the same signed capacity; NativeUfs refuses a window with two policies")
    a.out.mkdir(parents=True, exist_ok=True)
    for w in windows:
        (a.out / (w["name"] + ".header-only.bin")).write_bytes(w["header"])
    (a.out / "trusted-key.bin").write_bytes(key)
    key_bytes = ", ".join("0x%02x" % b for b in key)
    lines = [
        "// Generated by tooling/k27/make-root-policy-source.py from a VerifiedRoot policy directory; do not edit.",
        "// The block tables are not here: they travel as the NAME.vroot objects of the root image, and each table is",
        "// checked against the digest committed to by the 544-byte envelope below.",
        "#include <VerifiedBlockPolicy.h>",
        "#include <verified_root.h>",
        "",
    ]
    for w in windows:
        lines += hex_lines("policy_" + w["name"], w["header"])
    count = len(windows)
    # The archived anchor is the key NativeUfs hands vr_open (VROOTKEY + RSA-3072 modulus + 65537), so it is emitted
    # once, under the name NativeUfs.cpp declares inside extern "C". A C++ namespace-scope const without a prior
    # extern declaration has internal linkage, and the symbol the kernel collection then links is missing.
    lines += ["extern \"C\" {", "extern const uint8_t verified_root_key[VR_KEY_BYTES];",
              "const uint8_t verified_root_key[VR_KEY_BYTES] = {", "    " + key_bytes + ",", "};", "}", "",
              f"const NativeVerifiedBlockPolicy verified_root_policies[{count}] = {{"]
    for w in windows:
        label, volume = w["name"], w["volume_uuid"]
        blocks, size = w["blocks"], w["image_bytes"]
        lines.append(f"    {{ policy_{label}, {ENVELOPE} }},   // {volume}, {blocks} blocks over {size} bytes")
    lines += ["};", "", f"const uint64_t verified_root_policy_count = {count};", ""]
    source = a.out / "root-policies.cpp"
    source.write_text("\n".join(lines) + "\n")
    for w in windows:
        label, volume = w["name"], w["volume_uuid"]
        blocks, size = w["blocks"], w["image_bytes"]
        digest = w["image_sha256"][:16]
        print(f"{label}: {blocks} blocks over {size} bytes, image {digest}, volume {volume}")
    anchor = hashlib.sha256(key).hexdigest()[:16]
    print(f"{source}: {source.stat().st_size} bytes, anchor sha256 {anchor}, {count} policies "
          f"(manifestBytes {ENVELOPE}, header-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
