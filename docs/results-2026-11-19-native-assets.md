# Native k27 Asset Audit

Audit date: 2026-11-19. This is a read-only audit of retained host assets. No tablet access, backup, userdata operation, GPT change, or persistent boot write was performed.

## Locked input

The historical `unixnu.lock` records the native k27 input as commit `11ac316b789d81fc9f734e15645456f536c05141`, branch `el1/native-variant`, on XNU `ac9718fb1af618d5ce8678d0dc6e8a58f252216f` with `patches/xnu-12377` `0001-0054`. The recorded prepared-tree hash is `de16623d957cccff6dfb78d8eca4a79842af9702`.

The same lock entry is present in the retained `piano-macos-latest` and `piano-macos-upstream-main-49c589` checkouts. These checkouts contain the lock metadata and downstream build scripts, but neither contains the native source checkout or the prepared tree object.

## Reusable source assets

The retained archive `E:\codex\2026-11-01\ni\work\UniXNU-upstream-git\out\feeds\xnu-ac9718.tar.gz` has SHA-256:

`577f4a220304296332e28c802779e7b5e6a27a46812315aacf61c0269a03e12a`

It contains 6,511 archive entries rooted at `xnu-ac9718fb1af618d5ce8678d0dc6e8a58f252216f/`. The local retained tree `E:\codex\2026-11-01\ni\work\xnu-ac9718-full` matches this archive for `bsd/kern/kern_exec.c`, `Makefile`, `osfmk/arm64/arm_vm_init.c`, and `.upstream_base_commits` byte-for-byte. This is a verified XNU base, not the native k27 result.

The retained archive `E:\codex\2026-11-01\ni\work\external\macOS-DroidVM-xnu12377-patches.tar` has SHA-256:

`91eeedeeb3e879f05f53939183c8e0e96f87dca41a043e3bb8c4ac325e1a3247`

It contains 47 ordered VM k27 patches plus README. Its README states that replaying them onto `ac9718fb...` yields VM tree `c97cc109b2b49890091f3fb035777204c2fd1e4b`; this is provenance for the successful DroidVM line and cannot establish native commit `11ac316b...` or prepared tree `de16623d...`.

The two retained semantic patch directories under `piano-workspace` contain the same patch bytes except that the older set additionally has `native-union-statfs-fsid.patch`. They are not a complete, independently authenticated export of the native commit.

## Historical build outputs

`E:\codex\2026-11-01\ni\work\piano-macos-latest\out\k27` retains native-labelled RAM images and kernel collections, including `kc-k273-native.bin` and `ram-native\boot-piano-macos-ram-A-bundled.img`. Their presence is useful for later artifact/provenance comparison, but without the exact source tree, build manifest, and device-side run log they are not accepted as a reproducible native 27.3 build or boot result. They must not be copied into the current dependency pins.

## Reproducibility boundary

The verified state is therefore: exact XNU base available; historical VM patch archive available; native lock metadata and downstream patches available; native source/prepared tree object and a matching source-to-binary manifest unavailable. The next safe step is an offline replay comparison in a disposable directory. A match to `de16623d...` is required before treating the result as the locked native tree.
