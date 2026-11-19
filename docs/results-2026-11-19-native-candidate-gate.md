# Native k27 Candidate Gate

Audit date: 2026-11-19. This check used a disposable host directory only. It did not access, back up, reboot, or modify the tablet.

The retained XNU archive was verified before use:

- source archive: `E:\codex\2026-11-01\ni\work\UniXNU-upstream-git\out\feeds\xnu-ac9718.tar.gz`
- source commit named by the archive: `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`
- SHA-256: `577f4a220304296332e28c802779e7b5e6a27a46812315aacf61c0269a03e12a`

The 47-patch public DroidVM archive was then replayed in filename order with `git am --3way` onto that base. The disposable checkout at `E:\Temp\k27-native-candidate-20261119-a` reached 48 commits including its synthetic archive baseline and had a clean work tree. This reproduces the public VM series only.

The repository's native series `patches/k27/0053-0070` was checked against that result with `git apply --check --3way`. The first patch, `0053`, failed before any native patch was applied:

```
error: patch failed: bsd/vfs/vfs_subr.c:2661
error: patch failed: osfmk/arm64/machine_routines.c:923
```

Therefore the public VM replay is not the locked native source and must not be used for the device build. This check does not recreate native pin `11ac316b789d81fc9f734e15645456f536c05141` or prepared tree `de16623d957cccff6dfb78d8eca4a79842af9702`.

