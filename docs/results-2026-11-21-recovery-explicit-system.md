# Explicit system reboot from stock Recovery (2026-11-21)

While the tablet was in stock Recovery on the original GPT, `adb -s 7ccdd855 reboot system` returned exit code 0 and USB disconnected. The same tablet re-enumerated in ADB Recovery on a new transport ID. The explicit system target did not provide an unattended return path. The on-device Reboot to System action is pending.

The frozen candidate and rollback fragment pairs were staged separately on the host and each passed the GPT pair checker. No partition write or wipe was issued.
