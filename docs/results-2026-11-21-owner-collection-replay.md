# Owner collection replay (2026-11-21)

In the `UbuntuPiano` WSL distribution, run the saved `/tmp/piano-verified-collection.68bZb6/owner-cryptex-bindfs-argv.json` from `/tmp/piano-linkinfo-relink.yab5k88c`, changing only the output path to a fresh scratch file. The basename in the retained directory and the replayed `/tmp/piano-kc-replay-8noxrd6u/layout.kc` compare byte for byte with `cmp`. Both contain 135,806,976 bytes and 26 linked native drivers.

| Input or output | SHA-256 |
| --- | --- |
| `k27/kc-build273-piano.py` | `75f02a999d63adb3b1316085c8a2860633cf75eda2e33a496cac5376e5835317` |
| `scripts/kc-build.py` | `e1094afb9e243be0065f537a70cda9860faf0b4600443e27319e8a9745a25ed0` |
| `apple-normalized.bin` | `17414460767739dd8e61437b649e71e17bcbf15fbdeecf58c9bd17b9d081b09d` |
| `D:/Temp/piano-bindfs-owner.macho` | `a30e23d626084533428cfd0a3afbea39f8de2d58374bfa33fd06c280210ab817` |
| `owner-cryptex-bindfs.kc` | `e5c72beb67784d985736b0c2719968727878288eab38e8d0c41a33de1eb92462` |

The Windows kernel matches the Mac copy at `/private/tmp/piano-bindfs-owner-pl2b42oh/kernel.macho`. The older `kc-preflight` working directory yielded the same alternate hash on two runs and emitted CPU subtype `0x00000002` instead of `0xc0000002`. The saved arguments use a relative builder path, so the working directory is part of the recipe.

The matching collection still carries fixture signing inputs and targets unshimmed SystemOS. The embedded launchd filesystem jobs request `-fy` and `-qL -R 1`. Their device selection and exit behavior on the composed root remain to be qualified. The replay ran on the host and performed no tablet operations.

The saved replay arguments select `RegisteredNativeUfs.json`. Its object list includes `ufs-verified.o` (SHA-256 `df7d1cf7da92b2983ac275ebbdbc9edc61d2ba951023ae29f00e9dd1dfd41f43`), the registered startup object, and the fixture `host-poll.o`. The generated `owner-cryptex-bindfs.natives.json` contains `_vr_open`, `_vr_verify_block`, `_vr_xnu_crypto`, and `_boot_progress_poll`. The VerifiedRoot implementation is therefore present in the retained collection. Deployment still requires policies bound to the final root windows, an owner anchor, and a decision on the boot progress configuration. These are host link results.
