# 2026-11-25: UFS-capable FD found; one RAM trial stops without USB

## Finding

The FD used for the signed `boot_a` candidate (`44531624…`, built from the tablet's own OS3.0.303 binaries by
`tooling/firmware/make-own-binaries.sh`) has no UFS driver. Its UEFI log loads `Fat.efi` only, logs
`config: none found, using built-in configuration`, shows four built-in entries (`exit` first) and resets.

A local inventory of every `*.fd` under `E:\`, with the LZMA-compressed FVMAIN unpacked and searched, found one 3 MiB FD that
has both the Qualcomm UFS stack and the UniXNU patches:

| FD | SHA-256 | UFS stack | `config: volume` | DarwinBoot 13 GiB |
| --- | --- | --- | --- | --- |
| `E:\codex\2026-11-01\ni\work\piano-macos-latest\out\fd\piano-uefi-A-bundled.fd` | `394c5c786f4d114ccb0e709083ce574e4c5964593bd656bdfe8dd0f2e5bd5d2` | yes (`UfsBlockIO2`, `UfsCoreBspLib`) | yes | yes |
| `…\outputs\piano-upstream-r9-el1-diag-20261113\SILICIUM_UEFI.fd` | `ed159825a7b6194b0f00226dc3601c30b496e511f7ccf61d0bfef6e28277bb11` | yes | no | no |
| current `out/fd/piano-uefi-A-bundled.fd` | `44531624219cf0a2c9b114ca624c36452c5c5fb9841420f9d0ab90181e4f0afb` | no | unknown | no |

`394c5c78…` is the persistent-boot FD shape: FAT on UFS, the RAM-payload-first patch and the 13 GiB DarwinBoot window.

## Trial

Both FDs are 3,145,728 bytes and sit at byte 4288 of the unsigned boot image (BootShim + FD + DTB), so a candidate can be
made by replacing that block in `out/persistent/boot-piano-macos-persistent-v4.img`, zero-padding to 100,593,664 bytes and
adding the AVB hash footer with the local key. Trial 1 used the r9 FD:

- `out/persistent-r9test/boot.img`, 100,663,296 bytes, SHA-256 `5de858b8157e9572f9f44ad9648c87d669d6a53e5aecb50630df34d0b080f19c`
- `fastboot boot` was accepted; `piano-uefi` enumerated over USB for about 30 s, then disappeared.
- No `piano-uefi`, fastboot or ADB device returned in the next six minutes, so the UEFI log could not be captured. The r9
  build is an EL1 diagnostic with no `config: volume` patch; its historical status also records a non-enumerating stop.
- No partition was written by this trial; `boot_a` still holds the signed v4 image from the previous session.

Trial 2 is prepared with `394c5c78…` (`out/persistent-r9test/boot.img`, rebuilt) and will run once the tablet is back.

## Next

1. Power-cycle the tablet once (the only physical step) and return it to ABL fastboot.
2. Boot the `394c5c78…` candidate and capture `oem log` within the first seconds, before any XNU handoff.
3. If the ESP is mounted and `unixnu.toml` is read, write this FD into the persistent boot image and continue with the menu,
   desktop and acceptance matrix.
4. A fresh FD build is being prepared on the Mac from the pinned UniXNU, uefi-sheng, DarwinBoot, Mu-Silicium and piano-uefi
   checkouts for reproducibility.

## Content check of the candidate FD (2026-11-25, offline)

The 3 MiB block at byte 4288 of `out/persistent/boot-piano-macos-persistent-v5-ufs.img` decompresses to 8,097,800 bytes
across the FV's compressed sections (LZMA, GUID `98584eee-…`). Searching the decompressed image proves the three properties
that matter, from the artifact itself rather than from the build log:

- `UFSDxe` is present, with the Qualcomm package path `QcomPkg/Drivers/UFSDxe/…/UFSDxe.dll` and the DT bindings
  `/soc/ufs%d`, `/soc/ufs%d/ufs_clk%d`, `/soc/ufs%d/volt_regs/qref_on`.
- `config: volume %s` is present, so the embedded `UniXNU.efi` carries `firmware/patches/unixnu/0001`.
- `taken (largest run)` is present in UTF-16, the `DarwinBoot.efi` marker for the 13 GiB RAM window.

The v4 image the earlier `fastboot boot` logs came from has FD `44531624…`; the log stored under
`out/uefilog-20261125-025522/` is therefore the no-UFS case: `UniXNU: config: RAM Simple File System Unsupported`,
`config: none found, using built-in configuration`, `menu: shown 4 entries, default 0 'exit'`. That matches the diagnosis and
is not evidence about the new FD. The later 30 s enumeration trial left no log (its directory is empty), so the new FD has
never produced a captured log yet.

Artifact: `out/persistent/boot-piano-macos-persistent-v5-ufs.img`, 100,663,296 bytes, SHA-256
`5de858b8157e9572f9f44ad9648c87d669d6a53e5aecb50630df34d0b080f19c`; `avbtool verify_image` passes with
`out/keys/piano-boot-v4-local.pem` (footer plus 100,593,664-byte payload).
