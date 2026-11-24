# boot_a v4 rebuilt from an FD that already ran on the tablet (2026-11-25)

The persistent `boot_a` no longer depends on the missing signing host or the v5 image: the container was rebuilt offline
from local, hash-checked pieces and is staged for the first slot A write.

## Inputs (all verified on this host)

| Piece | Value |
| --- | --- |
| FD | `out/fd/piano-uefi-A-bundled.fd`, SHA-256 `44531624219cf0a2c9b114ca624c36452c5c5fb9841420f9d0ab90181e4f0afb`, 3 MiB |
| FD source | the kernel field of `out/no-demura-control/boot-piano-blendfix.img` (the same FD is inside 353 images of that series) |
| FD stamps | `config: volume %s` (ASCII, `unixnu/0001`) and `DARWINBOOT: window 0x%lx requested, 0x%lx taken (largest run)` (UTF-16, `darwinboot/0001`) both present after LZMA-decoding the guided section |
| BootShim | 192 bytes, first 192 of the same kernel field: Linux Image header `text_offset 0`, `image_size 0x301000`, `flags 0xa` |
| boot DTB | `out/inputs/v110/piano-boot.dtb`, 147,436 B, SHA-256 `50636effb5f54306ec0ab9a3061cd163d7f003d8797e04585a33205918856840` (recovered from `E:/codex/2026-11-01/ni/outputs/piano-macos-273-kernel-20261114-fixed-arm64-header/inputs/piano-boot.dtb`, no download needed) |
| mkbootimg | `out/inputs/v110/aosp-mkbootimg/mkbootimg.py`, SHA-256 `37d84b3d162e0bc62e36c1f4e1c63c85ea0caa9f29be023eb2f8efe006ad948c` (the pinned Ubuntu RAM-set tool) |

FD `44531624…` carries both `build-fd.sh` stamps, so its DarwinBoot window bound is the 13 GiB one (0x340000000). The
documented pair table (`docs/persistent-boot.md` section 12) says a 13 GiB FD boots with an 8 GiB ADT; the installed ESP's
`ram_window` is 8,589,934,592, so the pair is allowed.

## Output

`out/persistent/boot-piano-macos-persistent-v4.img`, 3,301,376 bytes, SHA-256
`b267c758d0659c9975d71bd06549d4800ee98ad2390c8b8092c4648b075077bd`.

- Android boot v4 header, `header_size` 1584, page 4096, `os_version` 0x1e0001a2 (15.0.0, patch 2026-02), empty command
  line, no ramdisk, no boot signature.
- kernel field 3,293,356 bytes, SHA-256 `d921655f247c67a4bf3542d705e07c5c00c51443f3029c02877b0daa092e67da`
  (shim 192 + FD 3,145,728 + DTB 147,436).
- Same total size as the historic v4 image, which is the container form ABL already accepted on this tablet.

No signature was added. The 2026-11-13 evidence is that a v4 image signed with a key other than the boot chain's
(`6b38a688…` against the vbmeta chain's `6be268f3…`) booted from a power-off on this unlocked tablet, so the AVB footer is
not treated as the gate; the first trial decides it.

## Not yet done

Nothing here touched the tablet. The slot A write still waits for the Ubuntu `vendor_boot.img` (the last entry of the
v1.1.0 boot archive). `init_boot.img` (`96057eb263fbe09284ba8bbe888128dbc0b36525dec4b01da918dcb7c6fe10ef`), `boot.img`
(`d8d812798b1ec042eff860f122013617b41f194323aa9607ae0f2e7735b12b3a`), `piano-boot.dtb` and `boot-ram-maintenance.img`
(`54001a50a13fe80c…`, 49,180,672 B) were already extracted and hash-checked from the partial archive against its own
`boot/SHA256SUMS`.
