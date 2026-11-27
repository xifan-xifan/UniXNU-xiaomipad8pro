# 2026-11-28 (00:00-01:00): the persistent ESP carried from this unit's own installed ESP

Host: Windows (this checkout) and `ssh mac-lan` (`~/Codex/upstream-tip`, synced commit by commit through
`git bundle` + `scp`, since the Mac has no GitHub access). Device `7ccdd855`, HyperOS OS3.0.309.0.WPYCNXM,
root already on `linux_rootfs` (`9e4ef505...`), Data store on `linux_data` (seed `bb6dd1c365a6a827`, resumed
across three reboots). Nothing was written to any partition in this window: the night's only device work was
one RAM boot (`98fefcac...`, the touch-firmware image with this unit's Y flipped), which reset itself to ABL
after 1800 s and left the tablet back on HyperOS on adb at 00:38:40
(`logs/native-runs/ram-k273-a830-yflip-20261128-000632`).

## 1. The ESP built with build-esp.py would not have resumed this unit's Data store

The pair built earlier tonight (recipe `tooling/k273/recipes/esp-k273.sh`, `build-boot.py` + `build-esp.py`)
was rejected on inspection before any write, for two reasons that both come down to the store:

- its `macos` and `macos-diag` entries carried `data-persist=2` but no `data-store-part` / `data-store-lba`,
  and `common.store_args_of` then puts the store on the root's own partition (`linux_rootfs`); this unit's
  root image fills that partition, so a 24 GiB seed has no window there - NativeRamBlock would look for its
  header at a place determined by the seed's size, read a blank window, and initialise a store over the
  installed root (the same failure `common.INSTALLED_STORE_PAGES` warns about, which tonight's build had
  waved away with `PIANO_DATA_STORE_RELAYOUT=1`);
- its record's `store` block has `first_lba_now` 100958783 (the reference unit's window) and no
  `partition`/`first_lba`, so `maint-session.py`'s `esp_store()` sees no bound store at all and the write
  would have been gated on the wrong window.

`store.first_lba_now` 100958783 with this seed's 6,291,649-block window reaches LBA 107250432, i.e. straight
through `linux_rootfs` (102269696-107512575). That is why this was settled before writing rather than on the
tablet.

## 2. The ESP is now built by tooling/k273/build-k273-esp.py, from the ESP on the medium

The installed ESP (`out/persistent/esp-device.img`, `68c28df1...`, profile `daily`, the one on the tablet)
already carries the right line: `native-ufs-root-part=linux_rootfs data-store-part=linux_data
data-store-lba=117735999 data-persist=2`, plus the maintainer's daily driver set (gpu, backlight, keyboard
gestures, power policy, thermal). `build-k273-esp.py` is the tool that carries an entry byte for byte and
swaps only the four payloads, so the recipe now calls it with tonight's kernel collection, ADT, trust cache
and seed, `--root-part linux_rootfs`, `piano-usb-shell=1` in and `piano-usb-hid` out (the shell on the cable
is the only way to read a failing boot), and `--expect-seed/--expect-store-part/--expect-store-lba` from this
unit's own profile. Two builder changes were needed:

- `build-k273-esp.py` refused any ADT whose RAM window differs from the base record's. The base's 8192 MiB is
  its own ADT's; tonight's ADT asks 13310 MiB, which the FD in `boot-piano-macos-persistent-v5.img` bounds at
  13312 MiB and every RAM boot tonight has run. `PIANO_BOOT_FD_RAM_WINDOW=1` (the same statement
  `build-esp.py` already takes for the 8 GiB cap) turns the refusal into a printed note; without it the
  refusal stays, now naming both numbers and what the flag claims. `maint-session.py`'s pair rule against the
  tablet's own `boot_a` is untouched.
- the recipe dropped `--arg droidvm-utc=$(date +%s)`: with it the ESP differed on every rebuild, and the pair
  has to be re-flammable byte for byte. Two builds back to back are now identical.

Result (built twice, identical, `cmp` clean):

```
out/persistent/esp-7ccdd855-k273.img  1073741824 bytes  sha256 5626a41110f5477b0d3b5a8a36742d24f5989e22195d6a92743e412128379ce6
  from esp-device.img: macos (968 bytes, byte for byte), store on linux_data LBA 117735999-124027647 bound,
  seed bb6dd1c365a6a827, entries [macos, macos-diag, fastboot] + actions
out/persistent-k273/boot-piano-macos-persistent-v5.img  3301376 bytes  sha256 b267c758d0659c9975d71bd06549d4800ee98ad2390c8b8092c4648b075077bd
```

Recipe: `tooling/k273/recipes/esp-k273.sh`; build logs `out/persistent-k273-build{5,6,7,8}.log` on the Mac.

## 3. What maint-session.py now says about it (dry run)

```
  check the store at LBA 117735999 on linux_data (a 6291649-block window at NativeUfs's place in linux_data as
        mapped) is valid, of seed bb6dd1c365a6a827, bound to LBA 117735999 (then this ESP resumes it)
  FAIL  the pair on the tablet: boot_a unknown contents (7c444a2d6aa930cd) ... its DarwinBoot's bound is
        unknown MiB -> that pair never boots: boot_a first (boot-a with an FD that takes it) before esp-final
```

The bound-store path is active and asks for exactly the store that is on the medium. The remaining stop is
the tool's own ordering rule: with `boot_a` still the stock Android image (read from the tablet as
`7c444a2d...`, which equals the verified backup `out/backup-20261125/boot_a`), no ESP can be `esp-final`, so
the write order is `boot-a` then `esp-final`.

The next gate, found while preparing that session and not yet resolved: step `boot-a` writes
`signed_boot_image()`, which wants `OUT/<name>` to be exactly the 100,663,296-byte partition-sized image whose
SHA-256 the boot record's `signed` ledger names - that ledger is filled by `build-boot.py --verify` after
`tooling/persist/sign.sh` signs the v4 container with the tablet's AVB key (`PUBKEY_SHA1
f5151930dc69791637b2a48505cb175f5f78a522`). The signing host is not ours (G7), and `boot-a` also refuses
`slot-success` (retired; `slot-success-crc` is the single-bit write). So the `boot_a` half needs either a
padded-unsigned image that this unlocked ABL accepts, or the signed route. Upstream's own install skill is the
place to settle it, and it is the next read. Host-side, not device: no boot was attempted with either image.

## 4. Two smaller things

- `gop-shot` works on the touch build (`source=flip format=BGRA layout=8-bit`, 6,490,117 bytes) - the latched
  flip window now reads through it. Pulling a PNG over the cable failed the same way four times at 22 KB,
  16 KB and 3 KB pieces alike, so `tooling/native/piano-fetch.ps1` silently mangles the base64 on this unit and
  its `[^A-Za-z0-9+/=]` strip turns that into a decode error instead of a failure: it needs a per-chunk
  checksum and a retry. Until then a panel image is not trustworthy evidence over this pipe.
- The self-reset left a pstore mailbox of 7,340,032 bytes with 7,004,524 nonzero (`run-console-ram-k273.ps1`),
  staged before the reader boot; it has not been decoded since the console capture already carries the boot.

## 5. The touch orientation is proven on the glass (owner, 01:0x)

The owner's "Y reversed" report was about `c0c1e2af...` (touch firmware, no flip). On `98fefcac...` - the same image with
`PIANO_TOUCH_INVERT_Y=1` stamped at build time, whose console read `orientation: flip X 0, Y 1` - the owner reports both
axes correct. So this BOE unit of `7ccdd855` runs Y reversed against the panel default, exactly what `docs/drivers/touch.md`
sections 5 and 7 record for piano's per-device `invert_y`, and the recipe's compile-time stamp is the right place for it.
Device-proven, not maintainer-reported.
