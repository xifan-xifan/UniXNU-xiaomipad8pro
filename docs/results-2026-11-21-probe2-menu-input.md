# Probe 2 menu input and HyperOS return — 2026-11-21

After the reported physical button presses, the UEFI menu screenshot matched the pre-input capture byte for byte and still highlighted Restart. The firmware reported `timeout -1 ms, input yes`. The log contained no selection or DarwinBoot entry. The input flag indicates discovery of the protocol. Physical button handling and the XNU handoff remain unverified.

The post-input capture is `D:/Temp/piano-menu-after-key.bmp` (SHA-256 `efb46f856d1b5c70a2f14de3fb047e64fe565ac366ebc5dddce745ba68824919`). The corresponding log is `D:/Temp/piano-menu-after-key.log`.

`fastboot -s piano-uefi reboot` returned OKAY. ADB confirmed `OS3.0.309.0.WPYCNXM`, slot `_a`, and `sys.boot_completed=1`. The Android persistence marker `/data/local/tmp/piano-layout-20261121-check` still hashed to `306a063b9b6d71bb48419efc7cf512c95c789d493a73d178e481a57dc82575f3`.

For the next handoff probe, the image builder supports `PIANO_MENU_DEFAULT=macos-8cpu` and `PIANO_MENU_TIMEOUT=0`. Derive the candidate from the frozen probe and compare the packaged components and boot arguments before testing. Automatic selection does not qualify the device menu controls.
