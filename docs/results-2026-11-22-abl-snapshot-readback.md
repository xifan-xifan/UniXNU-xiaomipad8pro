# ABL slot and snapshot readback (2026-11-22)

Using Android's normal `adb reboot fastboot` transition, read-only fastboot queries reported `is-userspace: yes` and `snapshot-update-status: none`. The tablet then entered ABL Fastboot for read-only queries: `is-userspace: no`, snapshot status `none`, current slot `a`, slot A successful `yes`, unbootable `no`, retry count `6`; slot B successful `yes`, unbootable `yes`, retry count `6`. No slot flags or partitions were changed. A normal Fastboot reboot returned the tablet to HyperOS OS3.0.309.0.WPYCNXM on `_a`, with `sys.boot_completed=1`.

On UbuntuPiano (Python 3.12), `tooling/persist/test-data-store.py` completed with zero failures and `tooling/persist/test-abl-state.py` passed. These are host-side checks. This readback does not establish native Darwin success-slot updates or recovery after XNU entry.
