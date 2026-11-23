# ufs184 KC rebuild audit (2026-11-24)

The ufs184 image (`4bef9114...`) is the last build whose native UFS console
reached `NativeUfs: OS media match entering`. Its kernelcache is
`/private/tmp/piano-ufs184.kc`, SHA-256
`84382771ff3639bf9f870352939e78dbf2a6e47f3e892bb9df81e1a9796925a6`, and the
KC extracted from the boot image matches it byte for byte.

## Reproduction attempts

Rebuilding with `kc-build273-rebase.py`, `kc-base.kc`,
`piano-paclr150/kernel.macho` and `desc-ufs184.json` alone produces
`836971b8...`, the hash recorded for ufs189. With the paclr53 kernel it
produces `84b43b73...`, the hash recorded for ufs187. The embedded kernel is
paclr150: its `LC_UUID` `3abfef9db5743a8abaf41cfa52dc03fe` appears in the
ufs184 KC.

The ufs184 KC carries 26 native kexts: ARMPlatformStandIn, NativeGopDisplay,
PianoA830, PianoBacklight, PianoDpu, PianoDwc3, PianoInput, PianoKeyboard,
PianoNullEthernet, PianoPower, PianoTorch, PianoCamera, PianoThermal,
PianoTouch, PianoWifi, PianoWifiEthernet, PianoAudio, PianoUsbHost,
PianoVideo, PianoSensors, PianoDp, PianoBluetooth, PianoPolicy, SoftwareAES,
NativeUfs and NativeRamBlock. The `desc-ufs184.json` builds contain only one.

Replaying the canonical `configs/k27-native-kexts.txt` order against the
`desc6` and `desc8` object sets fails in the linker:
`PianoWifi: no room for its 0x430d0-byte native __DATA (largest gap
0x29f9c, 0xc7cfc free in 48 gaps)`, with or without the repository's
best-fit placement patch (`patches/k27-kc/0001`, applied to
`/private/tmp/kc-preflight/scripts/kc-build.py`). ufs183 and ufs184 differ by
11290 bytes over an identical kext layout, so the 18x series did link this
set; the object files and the base KC that produced it are no longer the ones
on the host. `desc8` points at `/private/tmp/piano-rebuild/out/native-kexts`;
the checkpoint repo's own `out/native-kexts` directories contain no
`objects.json`.

## Consequence

ufs185, ufs187 and ufs189 early-stopped before any `NativeUfs` console because
their KC was rebuilt with a reduced native set, not because the synchronous
`registerService` change is wrong. The ufs185 source change is retained in the
worktree.

## Next step

Rebuild the native set through `tooling/k27/build-native-image.sh`, which
compiles every kext with its own script and expands `@OUT@` descriptors, then
check whether the resulting KC fits and boots. Only after that can the ufs185
source change be attributed to a device result.
