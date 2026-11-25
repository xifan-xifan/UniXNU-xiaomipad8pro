# 2026-11-26 - gap G6 becomes code: a unit profile from our own GPT heads, and the write tooling that can now use it

Follows [results-2026-11-26-kext-stage-header-shim-gap.md](results-2026-11-26-kext-stage-header-shim-gap.md). Host: this Windows
checkout. The tablet was untouched.

## The problem in one number

`tooling/persist/common.py` carries the reference unit's partition LBAs, and the write paths
(`build-gpt.py`, `maint-session.py`, `build-dual-esp.py`, `make-root-image.sh`'s embedded planner) read those constants. On this
unit `linux_data` starts at 107,512,576 while the recorded value is 71,789,568 - which sits inside our `userdata`
(3,632,128..102,269,695). Running the tooling as published would write a 19.5 GiB root image across Android's `userdata` and
leave `linux_data` untouched. That is what upstream gap G6 ("a second unit must not write before it has a device profile") looks
like in code, and it is now closed here rather than only documented.

## What was added

- `tooling/persist/unit_profile.py` reads a LUN 0 GPT head (and optionally LUN 4's) and prints the geometry this machine should
  write with: entry indices, first/last LBA, the Data reserve positions of both partitions, whether an `esp` partition already
  exists, and - given `--image-bytes` - the actual write plan. It refuses rather than guesses: a unit with no `linux_data`,
  overlapping rootfs/data, an `esp` of another size or entry index, or an `esp` overlapping `linux_data` all exit non-zero with
  the reason. Exit code 2 means DIFFERENT from the reference layout; `--json` writes the profile the tooling can consume.
- `tooling/persist/common.py` grew `apply_unit_profile()`, called at import when `PIANO_UNIT_PROFILE` names a profile. It rebinds
  the places (`ROOTFS_*`, `DATA_*`, `ESP_*`, `GPT_PRIMARY_SHA256`, `BOOT_A_*`) and refuses anything that disagrees about the
  constants the tooling assumes everywhere (`LBA`, `LUN0_BLOCKS`). It deliberately does **not** rebind the tooling's own rules
  (`LBA`, `ESP_BLOCKS`, `RESERVE_BLOCKS`) - a profile says where this unit's partitions are, never what the layout rules are.
  `REFERENCE` keeps the reference unit's values readable after an overlay, and `clear_unit_profile()` restores them for tests.
  With the variable unset the module behaves exactly as before, which the test asserts.
- `tooling/persist/test-unit-profile.py`: 23 host checks over synthetic GPT heads - parsing, our unit's geometry, the reference
  plan landing in our userdata, both reserve refusals, every refusal message, and the profile/constant round trip.
  `python tooling/persist/test-unit-profile.py` -> all checks passed.

Two of the first run's failures were the test's own arithmetic, recorded so nobody re-reads them as module bugs: the refuse
message says "does not end before", and the reserve refusal needs a size that actually crosses `data_reserve_from`
(`(data_reserve_from - data_first + 1) * lba`), not `RESERVE_BLOCKS` measured from the image end.

## Environment limits, stated plainly

The other tests in `tooling/persist/` do not run on this Windows host and were **not** run here: `test-abl-state.py` and
`test-slot-success-crc.py` import `fcntl`; `test-persist.py` shells out to `tablet.sh` (needs `xxd`/`od`);
`test-data-store.py` imports the `unixnu` package and `test-esp-geometry.py`/`test-k273-only.py` need inputs this checkout does
not hold. None of those failures involve the new code path: with `PIANO_UNIT_PROFILE` unset the added code returns before
touching a constant. To be run: the six existing persist tests on a POSIX host.

## Still open before any persistent write

LUN 4's GPT head is still uncollected, so a profile here covers LUN 0 only: `boot_a`, `vbmeta_a`, `dtbo`, `vendor_boot_a` and
`init_boot_a` all live on LUN 4 and their recorded indices/start offsets stay reference values. The head is read-only data
(`dd` of the first sectors out of the maintenance RAM image) and is the next thing to take from the tablet.
