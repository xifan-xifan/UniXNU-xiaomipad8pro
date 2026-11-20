# 4 KiB userdata geometry host check (2026-11-21)

UbuntuPiano created a sparse 404,019,478,528-byte userdata file and attached it as a loop block device with 4096-byte sectors. `blockdev` reported 4 KiB logical sectors and the exact proposed byte capacity. `mkfs.f2fs -t 0 -O encrypt` formatted the device using F2FS-tools 1.16.0, then `fsck.f2fs --dry-run` completed with all listed checks passing and exit status zero.

This verifies the candidate capacity can hold a basic encrypted-feature F2FS geometry on a host block device with 4 KiB sectors. It does not exercise Xiaomi Recovery, metadata encryption key creation, `cust.img` hooks, Android checkpoint behavior, the tablet's UFS driver, or a HyperOS boot. The disposable sparse fixture remains at `/tmp/piano-f2fs-4k.q22MOm/userdata.img` on UbuntuPiano; it is not a device image.
