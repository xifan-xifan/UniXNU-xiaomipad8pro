# 2026-11-25: slot A was left on the macOS boot set; the HyperOS triplet was restored and re-verified

## Symptom

A cold boot landed in ABL fastboot and HyperOS did not come up. Every `fastboot boot`, including the 8 MiB reader,
returned `Sending OKAY` then `Booting FAILED (Failed to load/authenticate boot image: Load Error)`, so the tablet could
not be reached at all. The first `getvar` read explains why both symptoms are one fault:

| field | before | after `--set-active=a` |
| --- | --- | --- |
| `slot-successful:a` | no | no |
| `slot-unbootable:a` | yes | **no** |
| `slot-retry-count:a` | 0 | 7 |
| `current-slot` | a | a |

`fastboot --set-active=a` (`out/pt33/platform-tools/fastboot.exe`, 33.0.3; 37.0.1 behaves the same) cleared the flag and
RAM images booted again. This is the consequence `WRITE-PLAN.md` and [the stock ABL result](results-2026-11-20-stock-abl.md)
predicted: exhausting the retries makes ABL mark the slot unbootable, and `ClearUnbootable()` also touches the primary and
backup GPT. The platform-tools version was excluded as a cause by testing both.

## What was actually on the tablet (maintenance RAM image, read-only)

`out/inputs/v110/release/boot/boot-ram-maintenance.img` (49,180,672 B, `54001a50...`) booted and gave a root shell over the
USB NCM link at `192.168.88.2:2323` (host side `192.168.88.10`, through `tooling/native/maint-shell.py`). The candidate GPT
is intact: `userdata` ends at LBA 102,269,695, then `linux_rootfs` 20 GiB, `linux_data` 64 GiB and `esp` 1 GiB, all on
4096-byte sectors. The boot partitions are on LUN 4 (`sde`): `boot_a` = `sde13`, `vbmeta_a` = `sde16`, `dtbo_a` = `sde17`,
`vendor_boot_a` = `sde23`, `init_boot_a` = `sde28`.

| partition | device SHA-256 | stock backup SHA-256 | match |
| --- | --- | --- | --- |
| `boot_a` | `0d52dd5e2c6304918b2b6836ff42e63600b1e5811aa96100f390b74fe4a76728` | `7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2` | no |
| `vendor_boot_a` | `7cbb06b9eddb8051c201d7a1f672620a5440283f0d5edbf0ace9270c05a60174` | `7f766f1d728c39d779a3fb7cf0ac1310dac4306cd3b37a98a0143dd4fca5bd23` | no |
| `init_boot_a` | `96057eb263fbe09284ba8bbe888128dbc0b36525dec4b01da918dcb7c6fe10ef` | `56197797f2ec99870a4e42b7a24bbf88407e4409615aba61f401182f13ea5cdc` | no |
| `vbmeta_a` | `84877c31822c8ee9245d7073a2339cbb3f11e354d7a2a54b2bdf4dcc8734b344` | same | yes |
| `dtbo_a` | `8557ce28f3c04677bca93e70fe6c736a90399a8a2fef55a495104c2cfeb660e9` | no backup taken | - |

The three differing hashes identify the write exactly. `boot_a` is byte-identical to
`out/persistent/boot-piano-macos-persistent-v4-avb.img` (`0d52dd5e...`), and `vendor_boot_a` plus `init_boot_a` are the
Ubuntu v1.1.0 images named in [the switch tool audit](results-2026-11-25-switch-tool-audit.md). Slot A therefore held the
**macOS boot set**, installed with `tools/piano-switch.py` (`a3d3cb88...`): that tool flashes `init_boot_a`, then
`vendor_boot_a`, then `boot_a`, and it re-flashes the rollback triplet only when a flash itself fails, not when the other
system fails to boot. `out/persistent/session/state.json` still recorded the pre-switch hashes, so that file is stale and
the device is the only authority on installed state.

HyperOS was not damaged. `vbmeta_a` never changed, slot B was never written, and `super`, `userdata` and `metadata` were
untouched. Android simply had no boot chain on the active slot.

## Restore

`fastboot flash` of the three verified host backups in `out/backup-20261125/`, hashed on this host before writing
(`boot_a.img` `7c444a2d...` 100,663,296 B, `vendor_boot_a.img` `7f766f1d...` 100,663,296 B,
`init_boot_a.img` `56197797...` 8,388,608 B), then `--set-active=a` and `reboot`. The upstream tool flashes kernel-last
(`init_boot_a`, `vendor_boot_a`, `boot_a`); here the order was `boot_a`, `vendor_boot_a`, `init_boot_a` with no boot
attempt between them, which gives the same final state but does not match the tool, so future round trips use the tool order.

## Verification

- Readback over the maintenance image after writing: `boot_a` `7c444a2d...`, `vendor_boot_a` `7f766f1d...`,
  `init_boot_a` `56197797...`, `vbmeta_a` `84877c31...`, `dtbo_a` `8557ce28...`. The first three equal the stock backups
  byte for byte.
- HyperOS came up on slot A: `ro.build.version.incremental` and `ro.mi.os.version.incremental` are both
  `OS3.0.309.0.WPYCNXM`, `sys.boot_completed=1`, `/data` is F2FS on `dm-56` with `inlinecrypt` (376 GiB, consistent with the
  shortened userdata) and `/metadata` is F2FS on `sda20`. Those are the same mounts as
  [the 2026-11-21 candidate baseline](results-2026-11-21-candidate-hyperos.md).
- Slot state read from ABL after that Android boot: `current-slot a`, `slot-successful:a yes`, `slot-unbootable:a no`,
  `slot-retry-count:a 6`. Android rewrote the success bit, which the macOS set cannot do.

## The rule this establishes for unattended work

Flashing `boot_a` clears the legacy bootable attribute (bit 54) and macOS never sets it, so while the macOS set is installed
`slot-successful:a` stays `no` and every boot attempt that does not return to Android burns a retry. When the count reaches
zero ABL marks slot A unbootable and then refuses **all** RAM images with `Load Error`, which is how the tablet became
unreachable rather than merely failing one experiment. Until macOS completes a boot and hands control back on its own,
slot A keeps the Android triplet and macOS experiments run only through `fastboot boot` of a RAM image.
`fastboot --set-active=a` is the recovery for the unbootable state and costs no data.

Two follow-ups this leaves open. The swap tool has no path back from a macOS set that never boots, so an unattended
round trip needs a host-side guard: refuse to leave the macOS set installed unless the previous macOS boot reported
success, and re-read `slot-retry-count:a` after every switch. And `out/persistent/session/state.json` needs to be rewritten
from the device after any switch, because it currently disagrees with the tablet.
