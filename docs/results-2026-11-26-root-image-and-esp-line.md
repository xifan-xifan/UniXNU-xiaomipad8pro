# 2026-11-26 - the installed ESP says where the root belongs: linux_rootfs, not linux_data

Follows [results-2026-11-26-ram-boot-v5-device-drop.md](results-2026-11-26-ram-boot-v5-device-drop.md). Host: this Windows machine
and the desk Mac. The tablet was not reachable in this round (it left the USB bus after the RAM boot and has to be given power at
the device), so everything here came from files already on disk.

## The ESP is machine-readable, and it settles the question the write plan got wrong

An ESP of this project carries UniXNU's TOML menu, and each entry's `args` line is what actually decides the layout. Read out of
`out/persistent/esp-device.img` (1,073,741,824 B, sha256 `68c28df15bcf55a589441df37e9c5fd94614e2771c437e955f190e3eb0aab898`, equal
to what `out/persistent/installed.json` recorded on 2026-11-24 23:23):

```
[boot] timeout = 3, default = "macos"
[[entry]] macos       (default)  path = Payloads/kernel.macho   initrd = Payloads/scratch-data.img
    native-ufs-root=1  native-ufs-root-part=linux_rootfs  data-store-part=linux_data
    data-store-lba=117735999      data-persist=2
[[entry]] macos-diag              the same storage facts
[[entry]] fastboot                native-ufs-root=0 data-persist=0     (plus the reset/shutdown/tools actions)
```

So the root of this install belongs on **`linux_rootfs`**, and `make-root-image.sh`'s header (which describes the image as written
"from the first block of linux_data") records the reference unit's *first* 27.3 round, not this install. That is the mechanism
behind the empty RAM boot: the ESP mounts a root from a partition this unit's disk has never had anything written to. `data-store-lba
= 117735999` is itself derived from our geometry (`linux_data` last + 1 - 262,144 reserve - 6,291,649 store blocks), which is why
the ESP build was right and only the root target was in question.

## What was added, and the regression it caught

- `tooling/persist/unit_profile.py` now parses that menu (`esp_entries`, `esp_write_target`): every `[[entry]]` block including the
  actions, and `[boot] default`, so the caller names the entry a unit boots instead of trusting whichever block a parser reached
  first. It refuses an ESP with no menu, a default that mounts no UFS root, an entry mounting a root without
  `native-ufs-root-part` (then the partition is a compile-time default the ESP does not record), and a menu whose entries disagree
  about the root partition. Blocks are read to the next NUL within 16 KiB - the image pads a gigabyte with NUL, and a first draft
  that sliced to the end of the last block tried to split that much padding (the fixture keeps both bounds honest).
- `build(..., boot_esp=...)` computes the plan on the partition **the ESP** chose, and bounds it by what is known: if root and
  store share a partition the image may not reach `data-store-lba`; if the ESP says the store is elsewhere, crossing that
  partition's tail reserve is allowed but recorded (`crosses_partition_reserve`, `reserve_blocks_consumed`); with no ESP given the
  tooling's own conservative rule stands and the crossing stays a refusal. `--esp` exposes it, and the ESP's sha256 goes into the
  profile as `boot_esp`, so a write binds to the menu that asked for it.
- `tooling/persist/test-unit-profile.py`: 39 checks, all passing. Two of them found real faults while the code was being written:
  a local variable shadowed the new parameter (the plan silently stayed on `linux_data`), and the first cut of the new branch
  **dropped** the pre-existing "stay under the reserve" refusal when no ESP was named - both are now pinned by a check rather than
  by memory.

## The two root images this unit could be written with

| image | bytes | sha256 | plan on `linux_rootfs` (102,269,696 + 5,242,880 blocks) |
|---|---|---|---|
| `out/piano-stock-root-26C5035r.img` (this machine, 2026-11-22) | 17,547,919,360 | `db3629e5da012ce76e64c0f42739ada9541ed8cca5adaab32256fc9b64b34ef7` | first 102,269,696, last 106,553,855, 4,284,160 blocks, **958,720 blocks free, reserve untouched** |
| `root.img` (the 27.3 line, built on the Mac) | 20,936,916,992 | `e682bd1f9b9c24744b535743cb7f1278be41c310640e744288057826f5e2165b` | first 102,269,696, last 107,381,247, 5,111,552 blocks, 131,328 free but **consumes 130,816 blocks of the partition tail reserve** |

The stock-window image's own manifest block is self-consistent and is what the kernel reads: `SHNGROOT` version 2, 4 windows, and
the declared image size equals the file size. Its windows were verified block by block against the archived public anchors on
2026-11-21 (3,538,432 OS and 712,192 SystemOS blocks, with a one-byte change rejected), see
[results-2026-11-21-stock-root-v2-assembly.md](results-2026-11-21-stock-root-v2-assembly.md). Either image can be written; the
first leaves the geometry clean and is the one the write plan should carry.

## The pairing that is still open, stated as a risk

The payload inside the installed collection is not built here: `~/Codex/k273-esp/inputs/piano-ufs264.kc` (147,095,552 B,
`529265c7839c51fe...`) carries `Darwin Kernel Version 25.5.0: Sun Nov 15 07:00:18 CST 2026; root:ios-obj/DEVELOPMENT_ARM64_BCM2837`,
while the kernels built on this Mac say `Darwin Kernel Version 27.0.0: ...; zhangqi:k27-native-obj/DEVELOPMENT_ARM64_BCM2837`
(27,557,496 B, `12d3088a...`). No 27.3 kernel collection is retained here (the one the docs name, `3fd3ed71...`, is not on either
machine), and building our own collection needs the native kext objects, which is the `storage-abi` gap. So writing a 27.3 root
under this ESP pairs a 27.3 system with a retained 25.5.0 kernel: it is a legitimate experiment that will answer on the console
(`native-ufs-diagnostics=1` is on, so the root stage names itself), but it is not "our own build boots", and it must not be
reported as such either way.

## Next, in order

1. Device back on the bus (power at the device): maintenance RAM image, read-only - the first block of `linux_rootfs` (a stale
   SHNGROOT manifest must be found before writing) and of `linux_data` (the store at 117,735,999 must be the seed `48dbc08d...` one).
2. Write `out/piano-stock-root-26C5035r.img` at `linux_rootfs` first block per `out/unit-profile/piano-7ccdd855-root-on-rootfs.json`
   - whole-region read-back hashed - and boot from disk with the installed ESP, console captured.
3. If the kernel refuses the pairing, that is the answer that the ESP must carry our own collection, which re-opens
   `storage-abi` as the blocker for a from-our-build boot, and the switch/persistence acceptance runs are reported against whatever
   boots, labelled accordingly.
## Same day, after the write path was read: `linux-data-root` and a unit whose root partition was never used

The write step this unit needs already exists - `tooling/persist/maint-session.py`'s `linux-data-root --root-part linux_rootfs
--root-image IMG --root-sha256 HEX`, which streams the image to the partition's first block, hashes it on the host without the page
cache and again on the medium, and changes nothing else. Its gate for `linux_rootfs` was written for the reference unit, where the
partition held the retired 27.0.1 system: it required `rootfs-clear` in the session state, i.e. the space reclaimed after a verified
backup. On a unit where nothing was ever written there, that gate blocks the write for the wrong reason, and no state file can
legitimize it.

The step now accepts the other evidence, read from the medium in the same session: block 0 of `linux_rootfs` equal to a zero block
(`ZERO_BLOCK_SHA256`, the sha256 of 4096 zero bytes). It is the stronger statement of the two - nothing is being overwritten, so
there is nothing to back up - and it is checked against the tablet rather than against history. Anything else in block 0 keeps the
old requirement (backup, then clear) and the precondition it accepted is recorded (`rootfs_write_precondition`: the block-0 hash,
the manifest's declared image size, its magic) so the session log says which way the space was shown to be free. `map` already
printed the same fact (`zero (cleared)`); this makes the write follow what `map` saw.

To be run on the tablet (the module imports `fcntl`, so this Windows checkout cannot exercise it; the session's own fake-tablet test
`test-persist.py` needs a POSIX host): the dry run

```sh
PIANO_UNIT_PROFILE=out/unit-profile/piano-7ccdd855-root-on-rootfs.json \
python3 tooling/persist/maint-session.py --dry-run maint-boot map linux-data-root check maint-exit \
    --root-part linux_rootfs --root-image out/piano-stock-root-26C5035r.img \
    --root-sha256 db3629e5da012ce76e64c0f42739ada9541ed8cca5adaab32256fc9b64b34ef7
```

must end with no `would stop a real run`, and the real run must show `linux_rootfs block 0 is still zero (...)` before any write.