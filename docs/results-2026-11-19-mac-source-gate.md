# macOS Host k27 Source Gate

Audit date: 2026-11-19. Checked through `ssh mac-lan`; no tablet access or device write was performed.

The Mac host is reachable as `zhangqideMBP.lan` and has the current repository checkout. Its `unixnu.lock` records the same k27 pin: commit `11ac316b789d81fc9f734e15645456f536c05141`, branch `el1/native-variant`, XNU `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`, and prepared tree `de16623d957cccff6dfb78d8eca4a79842af9702`.

The Mac host has no configured `PIANO_K27_TREE` in its environment file. A read-only search of `/Users/zhangqi`, `/tmp`, and `/Volumes` found no `xnu-ac9718.tar.gz`, `macOS-DroidVM-xnu12377-patches.tar`, XNU checkout, or k27 source directory beyond this repository's `tooling/k27`, `patches/k27`, and empty `patches/xnu` series marker.

The host compiler and support tools remain usable for later builds, but the exact native source/prepared tree must be supplied or reconstructed before a native build can be claimed reproducible.
