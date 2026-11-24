# 2026-11-25: the full FD pipeline builds on the Mac

`tooling/firmware/build-fd.sh` now produces the persistent-boot FD end to end on the Mac build host, with the same
properties the tablet needs: the Qualcomm UFS DXEs, the UniXNU `config: volume` patch, and the 13 GiB DarwinBoot window.

## What was wrong

The earlier Mac build ran only the inner `tooling/unixnu.sh build firmware`. That builds the Mu-Silicium DXE volume but
skips `build-fd.sh`'s two stamped checks, so it silently produced an FD without a patched `UniXNU.efi` or `DarwinBoot.efi`.
The report that the volume had no `UFSDxe` was a misread of `AARCH64/*.efi`: prebuilt binary INFs never appear there.
`FV/Ffs` carries `0D35CD8E…UFSDxe` and the built `UFSDxe.efi` contains `UfsSnapshotStart` and `[ufssnapshot]` messages.

The second failure was patch state. The research trees were copied from Windows and the patched files carried CRLF, so
`git apply --reverse --check` and `git apply` both failed on `Silicon/Silicium/SiliciumPkg/Sec/*`. Converting those four
files to LF (the patches themselves are LF) let the build accept the already-applied deltas.

`setup-mu.sh` also listed `Mu_Basecore` in `submodule update`; on this host that submodule is a plain populated directory
and the update tried to reach GitHub and timed out. The Mac-local copy drops that name from the list.

## Result

| Item | Value |
| --- | --- |
| Build command | `PATH=$HOME/llvm-20.1.8/bin:$HOME/shim:$HOME/py311/bin:$PATH JOBS=4 bash tooling/firmware/build-fd.sh` |
| Log | `out/fd/build-A-bundled.log` |
| FD | `out/fd/piano-uefi-A-bundled.fd`, 3,145,728 bytes, SHA-256 `3fcc84291a42275cd8feffc5c00192af1e2717660c1385e6d6964e1122db7037` |
| Local copy | `out/persistent/piano-uefi-mac-full.fd`, same bytes |
| Patched modules | `AARCH64/UniXNU.efi` (90,112 B) has `config: volume %s`; `AARCH64/DarwinBoot.efi` (24,576 B) has `taken (largest run)` in UTF-16 |
| Packed into the FV | `FV/Ffs` holds 196 entries, including `0D35CD8E…UFSDxe`, `1B64AF87…DarwinBoot`, `29B950CF…UniXNUDxe`, `75CA63AD…UniXNURamDiskDxe`, `9B665204…UniXNU` |

`build-fd.sh` refuses to finish unless both stamps are present, so the FD above carries the RAM-payload-first patch and the
13 GiB window by construction. It is not byte-identical to `394c5c78…` (different build host and toolchain), and it has not
run on the tablet yet.

## Boot image from the new FD

`out/persistent-v6-macfd/boot.img`, 100,663,296 bytes, SHA-256
`946adfe02aa166e8c76e59bbf60a76c8e41396c980bb04a5aeb7b5c8d9c06ac3`. It is the v5 container with bytes 4288-3149007
replaced by the new FD and re-signed with `out/keys/piano-boot-v4-local.pem` (SHA256_RSA4096, the v5 salt and rollback
index, the original three boot props). `avbtool verify_image` passes against `piano-boot-v4-local.pub.pem`, and the FD
block reads back as `3fcc8429…`.

The armed watcher still boots `out/persistent-r9test/boot.img` (`5de858b8…`, FD `394c5c78…`); v6 is the reproducibility
artifact for the round after that trial.

## Not done

Nothing here touched the tablet. The tablet has been off USB since 03:18, and the next step is still one power press so the
watcher can RAM-boot the `394c5c78…` image and capture the first `config: volume` log.
