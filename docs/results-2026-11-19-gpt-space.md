# GPT Free-Space Audit (2026-11-19)

This is an offline audit of the existing GPT boundary captures. It does not access the tablet, read userdata contents, create a backup, or write to any disk.

Command, run once against each independently stored capture set:

```sh
python tooling/persist/audit-gpt-space.py --manifest outputs/manifests/native-macos-27.3.json --captures E:\piano-gpt-boundaries-20261119-075429
python tooling/persist/audit-gpt-space.py --manifest outputs/manifests/native-macos-27.3.json --captures F:\piano-gpt-boundaries-20261119-075429
```

Both runs passed. For each of the six LUNs, the tool checked the head/tail capture lengths and SHA-256 values from the manifest, primary and backup GPT header CRCs, primary and backup entry-array CRCs, matching entry arrays, and partition ranges before calculating unallocated usable GPT extents.

| LUN | GPT partitions | Unallocated usable extents | Largest free extent |
|---|---:|---|---:|
| `sda` | 34 | none | 0 bytes |
| `sdb` | 5 | LBA 2262-3066 | 3,297,280 bytes |
| `sdc` | 5 | LBA 2262-3066 | 3,297,280 bytes |
| `sdd` | 3 | LBA 576-2042 | 6,008,832 bytes |
| `sde` | 79 | LBA 374924-378872 | 16,175,104 bytes |
| `sdf` | 7 | LBA 14848-16378 | 6,270,976 bytes |

No secondary UFS LUN has a GPT-free range large enough for the native root, Data and ESP layout. LUN 0 has no unallocated usable blocks because `userdata` reaches the last usable LBA. The largest secondary-LUN extent is 16,175,104 bytes on `sde`; it cannot avoid resolving the `userdata` capacity issue. Free filesystem space inside `userdata` is not a GPT-free range.

The first run caught a transposition in the `sdd` tail SHA-256 recorded in the JSON manifest. The E: and F: binary captures and both `MANIFEST.csv` files agreed on `632f8714835baf5a779bd5f42e6a1394e2a5612da37b003658736310bee67316`; only the JSON field was corrected. All 12 E:/F: capture files and both CSV manifests were then rechecked, and both full audits passed.

This audit rules out allocating native partitions from another existing UFS LUN. It does not qualify any userdata shrink method, choose final partition sizes, provide a WRITE-PLAN, or authorize a partition change.
