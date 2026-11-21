# Unattended return from the DarwinBoot diagnostic image

Stock ABL accepted the temporary image after confirming product piano, successful and bootable slot A, six retries and no snapshot. The USB observer sampled `piano-uefi` at 10:12:37 UTC on November 21, 2026. That interface disappeared by 10:12:43 UTC and ADB returned at 10:13:38 UTC without a host reboot command. HyperOS reported `OS3.0.309.0.WPYCNXM`, `_a`, boot complete and boot reason `bootloader`. The Android marker retained SHA-256 `306a063b9b6d71bb48419efc7cf512c95c789d493a73d178e481a57dc82575f3`. The observer log is `out/usb-observer-darwinboot-stop.log`.

No framebuffer capture or loader log confirmed the cyan and white marker in this run. The automatic Android return is verified. Reaching the DarwinBoot stop and exiting boot services remain unverified for this candidate. Capture a loader witness before treating this as a successful handoff control.

The combined build log is `UbuntuPiano:/tmp/darwinboot-post-ebs.tzsdzOfC/combined.log`. The packer verified that only the FD and v2 ID differ from the retained auto8 image.
