# Live candidate layout recheck

Android ADB reported completed boot on slot A with OS3.0.309.0.WPYCNXM and file encryption. Live by-name links identify userdata as sda34, linux_rootfs as sda35, linux_data as sda36 and esp as sda37. `/data` reports 394548224 KiB, matching the shortened-layout trial. The Android shell cannot read the raw sda34 geometry from sysfs. This identifies the visible candidate layout without confirming the GPT bytes or the contents of the native volumes.

The watchdog-enter no-poll payload ADT appends no boot arguments. The default eight-CPU entry selects `native-ufs-probe=2`, `data-persist=0`, a 7 MiB RAM console at `0x9cb00000`, and `piano-reset-after=90`. This readback does not establish XNU execution or reset retention.

The follow-up boot into stock ABL reported `is-userspace=no`, `snapshot-update-status=none`, slot A successful and bootable, and six retries. ABL reported partition sizes in hex: userdata `5E11700000`, linux_rootfs `500000000` (20 GiB), linux_data `1000000000` (64 GiB), and esp `40000000` (1 GiB). Host-driven reboot returned to completed HyperOS OS3.0.309.0.WPYCNXM. This checks ABL partition discovery and capacity; it does not read the GPT headers or write any native payload.
