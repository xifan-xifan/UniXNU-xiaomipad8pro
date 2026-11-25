#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the diagnostic bundle with the B-TC development signing identity."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("package_camera", HERE.parent / "extension/package-camera.py")
camera = importlib.util.module_from_spec(spec)
spec.loader.exec_module(camera)
IDENTIFIER = "com.unixnu.PianoCamera.DiagClient"

def build(output):
    app = output / "DiagClient.app"
    binary = app / "Contents/MacOS/DiagClient"
    binary.parent.mkdir(parents=True, exist_ok=False)
    info = {
        "CFBundleIdentifier": IDENTIFIER, "CFBundleExecutable": "DiagClient", "CFBundleName": "DiagClient",
        "CFBundleDisplayName": "DiagClient", "CFBundlePackageType": "APPL", "CFBundleVersion": "1",
        "CFBundleShortVersionString": "1.0", "LSMinimumSystemVersion": "15.0",
        "NSCameraUsageDescription": "Capture test frames to diagnose the Piano camera preview.",
    }
    (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
    command = ["xcrun", "--sdk", "macosx", "clang++", "-arch", "arm64", "-mmacosx-version-min=15.0",
               "-std=c++17", "-O2", "-fobjc-arc", "-Wall", "-Wextra", "-Werror", "-o", str(binary)]
    command.extend(str(HERE / name) for name in ("DiagClient.mm", "FrameEvidence.mm", "Replay.mm"))
    for name in ("AppKit", "AVFoundation", "CoreMedia", "CoreVideo", "CoreImage", "CoreGraphics",
                 "Metal", "IOSurface", "ImageIO"):
        command.extend(("-framework", name))
    subprocess.run(command, check=True)
    rights = {"com.apple.security.device.camera": True}
    with tempfile.TemporaryDirectory(dir=output) as tmp:
        entitlements = Path(tmp) / "entitlements.plist"
        entitlements.write_bytes(plistlib.dumps(rights))
        camera.quiet(["codesign", "--sign", camera.identity(), "--timestamp=none", "--generate-entitlement-der",
                      "--entitlements", str(entitlements), str(app)], "DiagClient signing")
    camera.quiet(["codesign", "--verify", "--deep", "--strict", "-R", "=" + camera.REQUIREMENT, str(app)], "signature verification")
    display = camera.quiet(["codesign", "-dvvv", str(app)], "signing metadata").stderr.decode()
    fields = dict(re.findall(r"^(Identifier|TeamIdentifier|CDHash)=(.+)$", display, re.M))
    assert fields["Identifier"] == IDENTIFIER and fields["TeamIdentifier"] == camera.TEAM
    assert re.fullmatch("[0-9a-f]{40}", fields["CDHash"])
    signed = camera.quiet(["codesign", "-d", "--entitlements", ":-", str(app)], "entitlement check").stdout
    assert plistlib.loads(signed) == rights
    subprocess.run(["/usr/bin/tar", "-czf", str(output / "DiagClient.tar.gz"), "-C", str(output), app.name], check=True)
    fields["ArchiveSHA256"] = hashlib.sha256((output / "DiagClient.tar.gz").read_bytes()).hexdigest()
    fields["ExecutableSHA256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    (output / "artifact.json").write_text(json.dumps(fields, indent=2) + "\n")

if __name__ == "__main__":
    build(Path(sys.argv[1]).resolve())
