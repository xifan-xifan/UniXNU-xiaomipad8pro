# ESP record audit — 2026-11-25

Read-only audit of the device-layout ESP: `out/persistent/esp-device.json` plus the
local `out/esp-device.img`, cross-checked against the Mac copy at
`~/Codex/k273-esp/out/esp-device.img` and the two device readbacks recorded on
2026-11-24. No tablet write, no boot, no change to the image.

## Identity

| Where | Bytes | SHA-256 |
| --- | --- | --- |
| local `out/esp-device.img` (re-hashed today) | 1,073,741,824 | `68c28df15bcf55a589441df37e9c5fd94614e2771c437e955f190e3eb0aab898` |
| Mac `~/Codex/k273-esp/out/esp-device.img` | 1,073,741,824 | `68c28df1…` (same) |
| device `sda37` readback (2026-11-24) | — | `68c28df1…` |
| device LUN 0 offset 485,507 blocks readback | — | `68c28df1…` |

All four agree. The record's own `status` is `OFFLINE_BUILD_NOT_DEVICE_TESTED`:
the image was built and installed to the ESP partition, but no boot has started
from it yet. That status is accurate and stays.

Geometry from the record matches the FAT header read directly: 4096 B sectors,
1 sector per cluster, 32 reserved, 2x256 FAT sectors, 262,144 total sectors,
hidden sectors 124,289,792, root cluster 2, label `PIANOESP`.

## Inputs match the pins

| Input | Recorded SHA-256 | Manifest pin |
| --- | --- | --- |
| `piano-ufs264.kc` | `529265c7839c51fe…` | same |
| `paclr53.adt` | `4e904d90e92e880c…` | same |
| `kernel.macho.trustcache.3mod` | `9d225ea325b3b10e…` | same |
| `scratch-data.img` | `48dbc08d5955a202…` | same |

The two firmware files (`PianoAudio/fs19xx.fsm`, `PianoVideo/vpu35_4v.mbn`)
and `EFI/UniXNU/unixnu.toml` carry recorded hashes with no manifest pin to
compare against.

## The seed hashes are two different hashes of one seed

`artifacts/ARTIFACTS.md` calls this "the 24 GiB empty seed (`7e9bd39b…`)". That
mixes up two values. The record separates them:

- file: 22,200,480 B, SHA-256 `48dbc08d5955a20255268aadc3f79660a62c395a68b358e349bbc3ea8f8b25e0`
- logical size: 25,769,803,776 B (24 GiB), 8 extents, 6,291,456 pages
- store: 6,291,649 blocks
- seed identity: `7e9bd39b72da851d9e4174258b3b2eb2277ef74e75fcb42f0bc07d662448e93b`

`7e9bd39b…` is the identity hash the kernel computes over the seed's page map,
not the file hash. The same pairing appears in
`outputs/manifests/native-macos-27.3.json` under
`first_install_data_candidate` (`sha256` 48dbc08d, `identity_sha256` 7e9bd39b),
and the local `out/inputs/k273/first-install/scratch-data.img` re-hashes to
`48dbc08d…` at 22,200,480 B today. So the ESP carries the preserved first-install
seed, and both recorded hashes are right; the ARTIFACTS line only needs to say
which is which.

## Store binding is consistent with WRITE-PLAN

Record: `store.partition=linux_data`, `extent=[107512576,124289791]`,
`first_lba=117735999`, `window_blocks=6291649`, `reserve_blocks=262144`.

- extent length: 124,289,791 - 107,512,576 + 1 = 16,777,216 blocks = 64 GiB
- store occupies 117,735,999 .. 124,027,647 (6,291,649 blocks)
- tail reserve 124,027,648 .. 124,289,791 = 262,144 blocks = 1 GiB

That is exactly the derivation in `WRITE-PLAN.md` line 77, and the ESP records
the extent as the binding source rather than the stock builder's partition end
(LBA 117,998,394), which NativeUfs would reject for this layout.
`tooling/persist/test-esp-geometry.py` passes all 15 checks, including the
binding above and the refusal of a one-block-short extent.

## Open risk to carry into the flash step

`data_store` in the record has `installed_seed=null`, `same_seed=false`,
`kc_has_store_guard=true`. The host test covers the intended first case — a
blank pre-install store window is accepted for its first `data-persist=2` boot —
and refuses a blank store once installation is recorded. The practical
consequence: the seed identity must be recorded as installed at the same time
the Data window is written, or the guard will treat the store as missing on a
later boot. This is a deployment-order constraint, not an image defect.

## Menu entries

Six entries, `default=macos`, `timeout=3`, `piano-boot-guard=180` on both macOS
entries. `macos` and `macos-diag` bind `native-ufs-root=1`,
`native-ufs-root-part=linux_rootfs`, `data-store-part=linux_data`,
`data-store-lba=117735999`, `data-persist=2`; `macos-diag` adds
`piano-reset-after=240 piano-reset-to=bootloader`; `fastboot` uses
`data-persist=0 piano-reset-after=1 piano-reset-to=bootloader`.

## Verdict

The ESP image, its inputs, its geometry and its store binding are internally
consistent and match the pinned plan. Two record fixes are warranted: label the
two seed hashes by role in ARTIFACTS, and carry the installed-seed ordering
constraint into the write plan. No rebuild is needed on this evidence.
