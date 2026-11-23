# ufs184 KC rebuild audit (2026-11-24)

The ufs184 image (`4bef9114...`) is the last build whose native UFS console
reached `NativeUfs: OS media match entering`. Its kernelcache is
`/private/tmp/piano-ufs184.kc`, SHA-256
`84382771ff3639bf9f870352939e78dbf2a6e47f3e892bb9df81e1a9796925a6`, and the
KC extracted from the boot image matches it byte for byte.

## Reproduction attempt

Rebuilding with `/private/tmp/kc-preflight/k27/kc-build273-rebase.py`,
`kc-base.kc`, `/private/tmp/piano-paclr150/kernel.macho` and
`/private/tmp/desc-ufs184.json` produces
`836971b84c26590b640ac32801a4290181cc50e4118527e20639bf107e8f8993` — the same
hash already recorded for ufs189. The paclr53 kernel and `desc-ufs187.json`
produce `84b43b73...`, the hash recorded for the ufs187 FAT.

The embedded kernel is confirmed to be paclr150: its `LC_UUID`
`3abfef9db5743a8abaf41cfa52dc03fe` appears in the ufs184 KC.

The difference is the native kext set. The fileset header of the ufs184 KC
lists `com.unixnu.driver.NativeGopDisplay`, `com.unixnu.driver.PianoA830`,
`PianoBacklight` and the rest of the full native set; a `--native
desc-ufs184.json` build contains only `NativeUfs`, `NativeRamBlock` and
`NativeGopDisplay`. ufs183 and ufs184 differ by 11290 bytes over the identical
kext layout, so every 18x build used one multi-kext descriptor list that was
not preserved on the Mac host.

Replaying the old `rebuild-ufs7.sh` ORDER list against `desc6` and `desc8`
fails in the linker with `no room for a 0x430d0-byte native kext __DATA`;
placing `NativeUfs` last fails earlier with an unresolved
`_native_ufs_store_sync` from `NativeRamBlock`.

## Consequence

ufs185, ufs187 and ufs189 early-stopped before any `NativeUfs` console
because their KC was rebuilt with a reduced native set, not because the
synchronous `registerService` change is wrong. The ufs185 source change is
retained in the worktree.

## Next step

Recover the exact multi-kext descriptor list (24 kexts including NativeUfs,
with `NativeUfs` before `NativeRamBlock` and a layout that fits) so a rebuild
reproduces `84382771...`; then rebuild the ufs185 source change on that base.
Until then no new image can be attributed to the ufs185 patch.
