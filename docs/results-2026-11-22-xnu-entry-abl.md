# XNU entry trial with pre-Android CVP capture

The temporary image combined the retained DarwinEnter loader with the ABL watchdog route. The host verified that only the firmware and Android v2 boot ID changed. Stock ABL reported slot A successful and bootable, six retries, and no snapshot update.

The tablet exposed firmware Fastboot and then returned to stock ABL automatically. The reader captured all 7,340,032 CVP bytes before Android started. The XNU log decoder reported signature `0x1106` instead of an XNU log. The loader used in this trial omits the EBS checkpoint writer, so the absence of those records is expected. The reader rebooted to completed HyperOS OS3.0.309.0.WPYCNXM on slot A. Evidence is in `out/xnu-entry-live/`.

The last executed instruction remains unknown. Preserve the EBS mailbox writer while restoring the DarwinEnter call so the next capture can locate the failure relative to EBS. A separate positive witness from kernel code is still required.
