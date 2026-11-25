#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build and verify the native arm64 B-TC camera package. Identity details stay in memory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parent
REPO = SOURCE.parents[2]
TEAM = "VAELT5D9YC"
HOST = "com.unixnu.PianoCamera"
EXT = HOST + ".Extension"
CAPTURE = HOST + "Capture"
GROUP = TEAM + "." + HOST
SERVICE = GROUP + ".Extension.service"
INSTALL = "com.apple.developer.system-extension.install"
GROUPS = "com.apple.security.application-groups"
SANDBOX = "com.apple.security.app-sandbox"
IOKIT = "com.apple.security.temporary-exception.iokit-user-client-class"
TRANSPORT = "com.unixnu.piano-camera.client"
CAMERA = "com.apple.security.device.camera"
ENTITLEMENTS = {
    HOST: {INSTALL: True, GROUPS: [GROUP]},
    EXT: {SANDBOX: True, GROUPS: [GROUP], IOKIT: ["PianoCameraUserClient"], TRANSPORT: True},
    CAPTURE: {CAMERA: True},
}
# Development branches of the 27.3 sysextd certificate requirement.
REQUIREMENT = ("anchor apple generic and (certificate leaf[field.1.2.840.113635.100.6.1.12] exists "
               "or (certificate 1[field.1.2.840.113635.100.6.2.1] exists and "
               "(certificate leaf[field.1.2.840.113635.100.6.1.25.1] exists or "
               "certificate leaf[field.1.2.840.113635.100.6.1.25.2] exists)))")


def quiet(args, label, *, data=None):
    result = subprocess.run(args, input=data, capture_output=True)
    if result.returncode:
        diagnostic = result.stderr.lower()
        reason = (" (keychain interaction unavailable)" if any(
            word in diagnostic for word in (b"interaction", b"errsecinternalcomponent", b"keychain")) else "")
        raise SystemExit(f"{label} failed (exit {result.returncode}){reason}")
    return result


def identity():
    text = quiet(["security", "find-identity", "-v", "-p", "codesigning"], "identity lookup").stdout
    valid = set(re.findall(rb'^\s*\d+\) ([0-9A-Fa-f]{40}) ', text, re.M))
    certs = quiet(["security", "find-certificate", "-a", "-p", "-c", "Apple Development"], "certificate lookup").stdout
    matches = set()
    for pem in re.findall(rb'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', certs, re.S):
        der = quiet(["openssl", "x509", "-outform", "DER"], "certificate decoding", data=pem).stdout
        digest = hashlib.sha1(der).hexdigest().upper().encode()
        if digest not in valid:
            continue
        subject = quiet(["openssl", "x509", "-noout", "-subject", "-nameopt", "RFC2253"],
                        "certificate OU check", data=pem).stdout
        if re.search(rb'(?:^|[,=])OU=' + TEAM.encode() + rb'(?:,|\s*$)', subject):
            matches.add(digest.decode())
    if len(matches) != 1:
        raise SystemExit("Expected one valid Apple Development signing identity for the configured team")
    return matches.pop()


def entries(package):
    app = package / "PianoCamera.app"
    extension = app / "Contents/Library/SystemExtensions" / (EXT + ".systemextension")
    capture = package / "PianoCameraCapture.app"
    return ((EXT, extension, EXT), (HOST, app, "PianoCamera"), (CAPTURE, capture, "PianoCameraCapture"))


def setup(bundle, identifier, executable, version, build):
    contents = bundle / "Contents"
    (contents / "MacOS").mkdir(parents=True, exist_ok=True)
    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleExecutable": executable,
        "CFBundleIdentifier": identifier,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": executable,
        "CFBundlePackageType": "SYSX" if identifier == EXT else "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": build,
        "LSMinimumSystemVersion": "14.0",
    }
    if identifier == EXT:
        info.update(CFBundleDisplayName="Piano Camera Extension",
                    CMIOExtension={"CMIOExtensionMachServiceName": SERVICE},
                    NSSystemExtensionUsageDescription="Publishes the front and rear cameras of the Xiaomi Pad 8 Pro.")
    else:
        info["LSUIElement"] = True
        info["CFBundleDisplayName"] = "Piano Camera" if identifier == HOST else "Piano Camera Capture"
        if identifier == HOST:
            info["NSSystemExtensionUsageDescription"] = "Installs the Piano camera system extension."
        else:
            info["NSCameraUsageDescription"] = "Captures and saves test frames from the Piano cameras."
    (contents / "Info.plist").write_bytes(plistlib.dumps(info))


def build_package(package, version, build):
    signing_hash = identity()
    env = dict(os.environ, DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer")
    frameworks = {
        HOST: ("Foundation", "SystemExtensions"),
        EXT: ("Foundation", "CoreMediaIO", "CoreMedia", "CoreVideo", "IOSurface", "IOKit", "CoreGraphics"),
        CAPTURE: ("AppKit", "AVFoundation", "CoreMedia", "CoreVideo", "CoreImage", "CoreGraphics"),
    }
    with tempfile.TemporaryDirectory() as temp:
        rights = Path(temp) / "entitlements.plist"
        for identifier, bundle, executable in entries(package):
            setup(bundle, identifier, executable, version, build)
            cmd = ["xcrun", "--sdk", "macosx", "clang", "-arch", "arm64", "-mmacosx-version-min=14.0",
                   "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fobjc-arc",
                   "-o", str(bundle / "Contents/MacOS" / executable), str(SOURCE / f"{executable if identifier != EXT else 'PianoCameraExtension'}.m")]
            if identifier == HOST:
                cmd[-1] = str(SOURCE / "PianoCameraApp.m")
            for framework in frameworks[identifier]:
                cmd.extend(("-framework", framework))
            subprocess.run(cmd, env=env, check=True)
            rights.write_bytes(plistlib.dumps(ENTITLEMENTS[identifier]))
            cmd = ["codesign", "--force", "--sign", signing_hash, "--timestamp=none", "--generate-entitlement-der",
                   "--identifier", identifier, "--entitlements", str(rights)]
            if identifier == CAPTURE:
                cmd.extend(("--options", "runtime"))
            quiet([*cmd, str(bundle)], f"{identifier} signing")


def has_der(data):
    # LC_CODE_SIGNATURE -> EmbeddedSignature superblob -> DER entitlements slot.
    _, cpu, subtype, _, cmds, _, _, _ = struct.unpack_from("<8I", data)
    if cpu != 0x100000c or subtype & 0xff != 0:
        return False
    offset = 32
    for _ in range(cmds):
        kind, length = struct.unpack_from("<2I", data, offset)
        if kind == 0x1d:
            start, size = struct.unpack_from("<2I", data, offset + 8)
            magic, total, count = struct.unpack_from(">3I", data, start)
            if magic != 0xfade0cc0 or total > size:
                return False
            for index in range(count):
                slot, child = struct.unpack_from(">2I", data, start + 12 + index * 8)
                if slot == 7:
                    magic, length = struct.unpack_from(">2I", data, start + child)
                    return magic == 0xfade7172 and 8 < length <= total - child
            return False
        offset += length
    return False


def verify_package(package):
    manifest = []
    for identifier, bundle, executable in entries(package):
        label = str(bundle)
        quiet(["codesign", "--verify", "--deep", "--strict", "-R", "=" + REQUIREMENT, label], f"{identifier} signature verification")
        display = quiet(["codesign", "-dvvv", "--arch", "arm64", label], f"{identifier} signing metadata").stderr.decode()
        fields = dict(re.findall(r"^(Identifier|TeamIdentifier|CDHash|Format|Hash type)=(.+)$", display, re.M))
        check = lambda condition: None if condition else fail(identifier)
        check(fields.get("Identifier") == identifier and fields.get("TeamIdentifier") == TEAM
              and re.fullmatch("[0-9a-f]{40}", fields.get("CDHash", "")))
        rights = quiet(["codesign", "-d", "--arch", "arm64", "--entitlements", ":-", label],
                       f"{identifier} signed entitlements").stdout
        check(plistlib.loads(rights) == ENTITLEMENTS[identifier])
        info = plistlib.loads((bundle / "Contents/Info.plist").read_bytes())
        check(info.get("CFBundleIdentifier") == identifier and info.get("CFBundleExecutable") == executable
              and info.get("CFBundleShortVersionString") and info.get("CFBundleVersion"))
        if identifier == EXT:
            check(info.get("CFBundlePackageType") == "SYSX" and "NSCameraUsageDescription" not in info
                  and info.get("CMIOExtension", {}).get("CMIOExtensionMachServiceName") == SERVICE
                  and SERVICE.startswith(GROUP + "."))
        if identifier == CAPTURE:
            check(info.get("NSCameraUsageDescription"))
        binary = bundle / "Contents/MacOS" / executable
        data = binary.read_bytes()
        check(data[:4] == b"\xcf\xfa\xed\xfe" and has_der(data))
        manifest.append({"identifier": identifier, "team": TEAM, "arch": "arm64", "cdhash": fields["CDHash"],
                         "sha256": hashlib.sha256(data).hexdigest(), "path": binary.relative_to(package).as_posix()})
    return manifest


def fail(identifier):
    raise SystemExit(f"{identifier} failed the signing, bundle or entitlement contract")


def source_commit():
    return subprocess.check_output(["git", "-C", REPO, "rev-parse", "HEAD"], text=True).strip()


def package_record(package):
    return {"source_commit": source_commit(), "executables": verify_package(package)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--build", default="2")
    args = parser.parse_args()
    package = args.package.resolve()
    if args.verify:
        manifest = package_record(package)
        if manifest != json.loads((package / "cdhashes.json").read_text()):
            raise SystemExit("Signed executables differ from the package manifest")
        print(f"Verified {len(manifest['executables'])} final arm64 CDHashes")
        return
    dirty = subprocess.check_output(["git", "-C", REPO, "status", "--porcelain", "--untracked-files=no",
                                     "--", "tooling/camera/extension", "package/PianoCamera"], text=True)
    if dirty:
        raise SystemExit("Commit camera sources before packaging")
    package.parent.mkdir(parents=True, exist_ok=True)
    if package.exists():
        raise SystemExit("Package destination already exists")
    with tempfile.TemporaryDirectory(prefix=".camera-", dir=package.parent) as temp:
        working = Path(temp) / "package"
        build_package(working, "1.1", args.build)
        manifest = package_record(working)
        (working / "cdhashes.json").write_text(json.dumps(manifest, indent=2) + "\n")
        working.rename(package)
    print(package / "cdhashes.json")


if __name__ == "__main__":
    main()
