# 2026-11-27 - the shimmed SystemOS root is on the medium: written, read back whole, nothing else touched

Host: this Windows checkout. Session `out/persistent/session-shim-root-20261127d`, unit profile
`out/unit-profile/piano-7ccdd855-shim.json`, steps `maint-boot map linux-data-root check`. Log:
`logs/host-shimroot-session-h.out.txt`. The tablet needed no button press and no new backup.

## What is on the partition now

`out/root-shim/piano-shim-root-26C5035r.img` - 21,100,494,848 bytes, SHA-256
`9e4ef50579b3ee5d443431d76434360ee9b0b02e8dbe443d3422e98532a78356` - occupies blocks 0 through 5,151,487 of
`linux_rootfs` (`sda35`), that is LUN 0 4 KiB LBAs 102269696 to 107421183. This is the root whose SystemOS window carries
the k27 tree's `/sbin/fsck` and `/sbin/mount` shims (`artifacts/ARTIFACTS.md`, 2026-11-27; upstream gap G1b), so
`launchd`'s `mount-phase-1` (`mount -P N`) has something to answer it.

## Proved from the medium before a byte was written

Both readings are the tablet hashing its own medium with the page cache dropped, not the host's word for it:

* blocks 0..4284159 - the 17,547,919,360 bytes the write replaces - hashed `db3629e5da012ce76e64c0f42739ada9541ed8cca5adaab32256fc9b64b34ef7`,
  equal to the host's held copy `out/piano-stock-root-26C5035r.img`. So every byte the write replaces is a byte the host can stream
  back, which is the whole point of `--root-overwrite-holds`: rollback is the same step with that file as `--root-image`.
* blocks 4284160..5151487 - the 867,328 blocks the longer image takes - read zero (`cf1d7c0f6c850e13` over the run), so nothing
  outside the old image's span is displaced.

## The write

Served over the USB link and streamed by `tablet.sh restore` (wget into `dd of=/dev/sda35 bs=4096 conv=notrunc,fsync`), which
returned `restored=root.img` after about 800 s, the link holding roughly 26 MB/s. The tablet then hashed what it had written,
caches dropped: `9e4ef50579b3ee5d443431d76434360ee9b0b02e8dbe443d3422e98532a78356` over 21,100,494,848 bytes - the image's own
digest. That is a read of the medium, not of the page cache.

## Untouched, checked after the write

GPT both copies still the unit's own tables (head `12710135635beee2`, tail `9acf222558a6766c`) and `sgdisk -v` clean; the ESP
region still hashes to `esp-device.img` (`68c28df15bcf55a5`); `boot_a` `7c444a2d6aa930cd`, `vendor_boot_a` `7f766f1d728c39d7`,
`init_boot_a` `56197797f2ec9987`, `boot_b` `ec345e825416210b` all unchanged; the root manifest block reads as the new image's.
Nothing from UFS was mounted at any point.

## What stopped this for an hour was a host bug, not the tablet

The first two attempts stopped at the precondition above, with the host reporting its held copy as `25535e6e230c8e0f` while the
tablet reported `db3629e5da012ce7`. `os.open(path, os.O_RDONLY)` on Windows opens a TEXT handle, and the CRT translation behind
`os.read()` returned 26 bytes for a 2 MiB binary fixture full of `0d 0a` pairs - so `Session.sha_nocache` had never hashed an
image on this host correctly. Fixed in `1e47c0ff` (one no-cache reader, `O_RDONLY_RAW` carrying `O_BINARY`,
`tooling/native/test-nocache-read.py`); with the fix the same function reports `db3629e5...` for the same file, i.e. host and
medium agreed all along. Every earlier Windows-side comparison made by that function failed loudly rather than silently, which
is the only reason no wrong conclusion was recorded from it. `map` also died once printing a hex dump that had decoded to
U+FFFD into a redirected log (cp936); fixed in `3057cd0b`.

## What `check` still refuses, and what it means

`check` got as far as `ESP = esp-device.img (installed.json)` and then stopped: the Data store at LBA 117735999 on `linux_data`
is of seed `8e1a87e1d5888960` (6291456 pages), while the installed ESP resumes seed `7e9bd39b72da851d`. So the ESP on the tablet
and the Data window on the disk are two different installs: with that ESP, boots would keep a store they do not own and run Data
volatile, or not find it. This does not affect the root write or its read-back. It does mean the persistent boot line needs an
ESP of the same seed as the store the 27.3 line is using - which is also the ESP that carries this root's ADT (SystemOS
`A8D7C405`) and the anchor `9cfab48d`, so it has to be built for this root anyway.

## Not verified by this file

Whether `mount-phase-1` now exits 0 (the first RAM boot of `6001bc1c` against this root was launched right after this write:
`logs/native-runs/ram-k273-shimroot-20261127-110401/`); a desktop; GPU, input, network, audio; Data persisting across a reboot;
Android coming back; and switching in either direction. None of those is claimed here.
