# 2026-11-26 - the root image builds here, and its write plan is the reference unit's (hazard)

Follows [results-2026-11-26-mac-build-host-unblocked.md](results-2026-11-26-mac-build-host-unblocked.md). Built on the desk Mac
from upstream `d70c16ce`; the tablet was only read (`ls /dev/block/by-name/`), never written or rebooted.

## Every Apple input for the 27.3 line is fetched from Apple and verified here

`python3 -I tooling/repro/fetch-ipsw.py --remove-aea` ended with
`PASS: every pinned Apple input is present and equal to its pin` (14.0 GB fetched of the 27,206,093,542-byte IPSW, the two AEA
images decrypted through Apple's key service). Among the verified pins: `kernelcache.release.mac17j` `66bbe2fa3edf...`,
App cryptex `797bb1e07e35...`, SystemOS cryptex `00f80636bede...`, OS cryptex `b7dbb5c6c3ee...` decrypted to
`40d8282eb8650da9b4d7437a981764c48d47ee4bdc5b8b362248f25ecbce0e70`, and the KC base `kc/kc273-mac17j.bin`
124,518,400 bytes `d1003b5143ff...`.

## SystemOS and the root image, both built here

| Artifact | Bytes | SHA-256 |
|---|---|---|
| `out/inputs/k273/root/systemos.img` (`make-systemos.sh`, sudo for `newfs_apfs`/`enableOwnership`/`ditto`) | 6,442,450,944 | `1947ac49e4c86186856a6b233c5b690fd0e21c98beb7571825c67b519952dcc3` |
| `out/inputs/k273/root/root.img` (`make-root-image.sh`) | 20,936,916,992 | `e682bd1f9b9c24744b535743cb7f1278be41c310640e744288057826f5e2165b` |
| `out/inputs/k273/root/manifest/manifest.bin` | 4096 | `590173211ddba136804c53c7de7b5cfc00bf907e2c1afa59bd8a79011edc9e7f` |
| `out/inputs/k273/root/kernel.macho.trustcache` | 451,264 | `002db2e6ee9000c891bad43247bfa9c55ad6d3b2ee62e6c0b9e1965e78926da6` (3 modules) |

Two host details worth keeping: `make-systemos.sh` runs under sudo and leaves `systemos.img` as root mode `0600`, which the
non-root `make-root-image.sh` cannot read, so it needs a `chown`; and gap **G2** (the 27.0.1 base trust bundle) closed without
the owner's file, by the second option the skill names - this host's own `kernel.macho.trustcache.3mod`
(`9d225ea325b3b10eaed407afea100428b92fd8a790b9b8071110bba0f9accbb4`, 3 modules = the 27.0.1 static/System/App caches, zero own
modules) was placed at `out/inputs/kernel.macho.trustcache`, and `make-trust.py` then produces 27.3's three caches with the
staging tools adding their own modules in front. **G3** used our own seed (`48dbc08d5955a20255268aadc3f79660a62c395a68b358e349bbc3ea8f8b25e0`),
which is not the reference unit's pinned `cb50ceac...`.

## The hazard: `root.json`'s write plan is the reference unit's geometry

`make-root-image.sh` records the plan as

```json
"write": {"partition": "linux_data", "lun": 0, "lba_bytes": 4096,
          "partition_first_lba": 71789568, "partition_blocks": 52762619,
          "first_lba": 71789568, "last_lba": 76901119, "blocks": 5111552,
          "untouched": "linux_data blocks 5111552..52762618 (the rest of the partition, the later Data store window at its end)"}
```

Resolved against this unit's own LUN 0 GPT (`out/rollback-3.0.309-gpt/candidate/candidate-gpt-head.bin`, 4 KiB blocks, 37 entries):

| Partition | Our first LBA | Our last LBA | Size |
|---|---|---|---|
| `userdata` (34) | 3,632,128 | 102,269,695 | 376.3 GiB |
| `linux_rootfs` (35) | 102,269,696 | 107,512,575 | 20.0 GiB |
| `linux_data` (36) | 107,512,576 | 124,289,791 | 64.0 GiB |
| `esp` (37) | 124,289,792 | 124,551,935 | 1.0 GiB |

`71789568`, `76901119` and `5111552` all fall inside **`userdata`** on this unit. Following that plan here would write a
19.5 GiB macOS root image across the middle of Android's `userdata` and never touch `linux_data` at all. The content of
`root.img` is unit-independent (it is the manifest plus the two windows, and the OS window's hash equals the profile pin);
only the recorded plan is another machine's. The correct plan for this unit is `linux_data` first LBA `107512576`,
`5111552` blocks, so `107512576..112624127`, with `16777216` blocks in the partition - which also means the "Data store window at the
end of the partition" that `root.json` describes sits elsewhere than the tooling assumes, and
`tooling/persist/common.py` guards exactly that with `INSTALLED_STORE_PAGES` (it refuses a mismatched seed unless
`PIANO_DATA_STORE_RELAYOUT=1`).

This is gap **G6** in concrete form, and it is why upstream's rule "no write on a second unit until the unit has its own device
profile" exists. `tooling/repro/unit-profile.py` says `DIFFERENT` for our LUN 0 with matching partition numbers and differing
positions and sizes, and LUN 4's GPT head is still uncollected (`boot_a`, `vbmeta_a`, `dtbo_a`, `vendor_boot_a`, `init_boot_a`
are all on LUN 4), which is read-only work in the RAM maintenance image and a precondition for any persistent write.

Nothing may be written to the tablet from a plan this file records as wrong for this unit.
