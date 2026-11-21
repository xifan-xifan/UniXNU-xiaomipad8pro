# Retained witness replay identity (2026-11-22)

The archived `witness.fd` is in UbuntuPiano at `/tmp/darwinboot-post-ebs.tzsdzOfC`. The current output is `/tmp/piano-post-ebs.mIk1NZN0/uefi/Build/pianoPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`.

| Artifact | FD SHA-256 | Decoded DarwinBoot FFS SHA-256 |
| --- | --- | --- |
| Archived witness | `bf9ff0d490c8aaeecba0f09c4bb359688db4ea08003b7722bf51d08bf419f27f` | `2eea7ddd9b08b1119eef05410cfc9e0cc1d73471d16ee285074f2886d59ba9a0` |
| Current output | `777b2cedd454fc7f65a1ad77c11d7dc473044d18f5ec4cc0405b16958307d102` | `3dccf3ecb2c6ba053548fae9c0d311366dd5eecc9627e4a48a8e805065b892dd` |

Both decoded APRIORI files are 1260 bytes with SHA-256 `5badddcfa77bff6ed430af7df0b6cd3271e6c2bc0fcbbe5cebcddf05ac60e7d0`. Preserve the archived witness as the reference. A dispatch-only comparison requires identical DarwinBoot FFS, remaining driver inventory, payload and DTB. This check ran on the host only.
