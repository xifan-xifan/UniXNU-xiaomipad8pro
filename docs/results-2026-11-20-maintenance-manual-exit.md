# Maintenance readback candidate with manual exit (2026-11-20)

USB carrier and the login shell must both survive to suppress the v1.1.0 idle timer. Keepalive only resets that timer. On expiry the watchdog tries PMIC SDAM Fastboot restart, then falls back to ordinary `reboot -f` if the driver is unavailable or `restart2` returns. Login automatically runs the partition map, including `sgdisk -p`.

The derived manual-exit candidate forces maintenance mode in init, omits the idle watchdog, removes ordinary reboot fallback and omits the automatic login map. Its command line retains EFI pstore suppression and requests `panic=0`. Explicit Fastboot exit remains available. The original kernel and DTB match byte for byte. The host verified the Android v2 layout, both edited shell scripts with `sh -n`, CPIO metadata and the unchanged contents of all other archive members. Artifact path and digests are in the deployment manifest.

ADB reported `piano`, `OS3.0.309.0.WPYCNXM`, slot `_a`, and completed Android startup. The candidate has not run on the tablet. ABL write paths, the effective command line, USB disconnect and reconnect, firmware and hardware resets, and complete readback still need qualification before this image can support a write procedure.
