# 2026-11-20 ABL recheck and Android return

The connected `piano` entered ABL Fastboot from Android using `adb reboot bootloader`. ABL reported `is-userspace=no`, current slot `a`, slot A successful and bootable, six retries, and `snapshot-update-status=none`. Partition sizes in bytes were reported as `0x6000000` for both `boot_a` and `vendor_boot_a`, `0x800000` for `init_boot_a`, and `0x73517FB000` for `userdata`.

A normal `fastboot reboot` returned to `OS3.0.309.0.WPYCNXM` in slot `_a` with `sys.boot_completed=1`. No image was loaded or flashed and no backup was made. This exercises the stock ABL-to-Android return, with diagnostic firmware startup, hard reset, and Android/macOS switching still pending. The current slot flags support the conditional reference-source GPT analysis in the [stock ABL audit](results-2026-11-20-stock-abl.md). The Xiaomi implementation still needs verification before loading the diagnostic image.

GitHub's repository API returned main at `2438556c77c90c22c7b76e57eb8b40c44b35f1af`, matching the deployment baseline. The build pins remain unchanged.
