# UEFI menu and USB diagnostics (2026-11-21)

The isolated DEBUG build restored `PianoFastboot.c` to the library sources and `PianoFastbootStart()` to `DeviceBootManagerAfterConsole()`. ESP, Android, and removable-media dispatch stayed disabled. The build and Ubuntu reference verifier passed. The FD SHA-256 is `970c7ae888c319323ec7d2d66f01f5d82e618f8bcc8d8344167dac71cec387f2`; the RAM image SHA-256 is `12802a1c97e75aa473090e3012b8c79592b880454da649c5e990d0a0e598135f`.

Before boot, ABL reported userspace=no, slot A successful and bootable, retry count 6, and snapshot status none. After `fastboot boot` the tablet enumerated as `piano-uefi`. The retrieved RAM log confirms `UniXNU.efi` started, initialized a 3200x2136 display and showed the four-entry built-in menu. A 20,505,654-byte framebuffer BMP was retrieved successfully. The firmware reboot command returned the tablet to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, `sys.boot_completed=1`. No persistent write command was issued. Postboot ABL retry and success flags were not independently rechecked in this run.

The build and verifier are retained under `/tmp/piano-bds-usb.SIjaLO/` in UbuntuPiano. The local `piano-bds-usb-debug.log` SHA-256 is `16eb94b7ea721c6db6ee6b292f474647ff1c317c038f4229df96e47f02e19acc`; `piano-bds-usb-screen.bmp` SHA-256 is `4425d2cf6ba006a75e05c081f803b6c5a36630d6501cf4e68c899edf8f75e263`. Both are in the Windows temporary directory.

The built-in menu did not load macOS. Carry the USB observation path into the next native candidate and qualify the actual payload selection before using it for an XNU handoff test.
