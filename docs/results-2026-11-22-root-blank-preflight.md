# Complete root partition preflight

RAM maintenance identified the unmounted `linux_rootfs` as `sda35`, with a start of 818157568 and size of 41943040 in sysfs 512-byte units. This is inclusive 4 KiB LBAs 102269696 through 107512575. LUN 0 had 510165778432 bytes and 4096-byte logical sectors. No partition holders or active swap were observed.

A foreground read exceeded the connection timeout. A detached direct read completed with status 0 and 5120 full 4 MiB records. The digest `6cb118a8f8b3c19385874297e291dcbcdf3a9837ba1ca7b00ace2491adbff551` matched the independently generated host digest of 20 GiB of zeros.

The same device reappeared in stock ABL and reported active slot A and `slot-successful:a: yes`. A host-driven reboot returned to HyperOS `OS3.0.309.0.WPYCNXM` with `sys.boot_completed=1`.
