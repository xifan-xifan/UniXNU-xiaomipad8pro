# 2026-11-25: switch tool and slot-success audit (host side)

The v2 plan asks for the exact source of the computer-side boot-set swap tool, and for the slot-success bit and retry
count to be verified before switching is trusted. This note records what the local copies actually do; the device-side
`getvar` reads still have to run on the tablet.

## The swap tool

`out/tmp-upstream-mainline/tools/piano-switch.py`, 3,184 bytes, SHA-256
`a3d3cb88fbbf4ab37b107de50009b9cd89b43fb5b2488cf492ed3f14ae7319e6`. It comes from the Ubuntu release tree
(xiaomipad-8pro-mainline `tools/`), which is the same tree `docs/persistent-boot.md` section 11 names for switching.

- Both `--images` and `--rollback` must contain `boot.img`, `init_boot.img`, `vendor_boot.img` and `SHA256SUMS`; every digest
  is checked against both the file and the manifest before any write.
- It refuses to run unless `product: piano`, `unlocked: yes`, `current-slot: a`, `is-userspace: no`, and each `*_a`
  `partition-size` equals the image sizes on both sides.
- It flashes `init_boot_a`, `vendor_boot_a`, then `boot_a` (kernel last). On any failure it re-flashes the checked rollback
  triplet before raising.
- It never calls `set_active`, and never writes `userdata`, `super` or `metadata`.

## Slot success and retry count

`tooling/persist/maint-session.py` keeps the ABL expectations in `ABL_EXPECT` and reads `slot-retry-count:a` with
`slot-battery-voltage` in `ABL_SHOW`. `abl-final` requires `current-slot: a`, `slot-successful:a: yes`,
`slot-unbootable:a: no`, and an integer retry count 0-7 that is unchanged from before the reboot.

`slot-success` exists for the case `abl-check` reports `slot-successful:a: no`: it sets bit 54 of `boot_a`'s GPT entry only,
after a verified backup of LUN 4's GPT, writes the entry directly, re-reads it, and requires the read-back to differ from the
backup only in that attribute field and the two CRCs. It never uses `set_active`. This is needed because flashing `boot_a`
clears bit 54 and macOS does not set it.

## Still to verify on device

`fastboot getvar slot-successful:a / slot-unbootable:a / slot-retry-count:a / current-slot` on the live bootloader, and the
same values after one switch round trip. Nothing here touched the tablet.
