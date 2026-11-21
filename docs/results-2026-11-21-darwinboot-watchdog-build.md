# DarwinBoot watchdog stop after ExitBootServices

The AArch64 DEBUG CLANGPDB module build compiled and linked the isolated DarwinBoot patch against the staged `8544957` loader. It produced a 26,166-byte FFS at `UbuntuPiano:/tmp/darwinboot-post-ebs.ffs` with SHA-256 `6563739a80454379e850bc9fa5d08bc01fff23c408902e75e338cb9badfbf034`. The build log is at `UbuntuPiano:/tmp/darwinboot-post-ebs.tzsdzOfC/build.log`.

The patch replaces the kernel branch with a cyan and white framebuffer marker and a deliberate stall. The platform must arm the watchdog before invoking the loader. The previous firmware control stalls before loading DarwinBoot, so a new combined FD must preserve the watchdog setup and remove that earlier stall. The module has not been packaged into a RAM image or run on the tablet.
