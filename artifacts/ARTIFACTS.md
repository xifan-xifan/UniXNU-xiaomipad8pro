# Artifacts of slice 1 (2026-11-10)

Built on the Mac by `tooling/build-slice1.sh` (SIGN=1) at ~19:56 SGT from the pins of `unixnu.lock`; files under `out/`
(ignored by git). Status of every image: **OFFLINE_CANDIDATE_NOT_DEVICE_TESTED**, `fastboot boot` only, never flash.
Verification logs: `logs/bootimg/verify-*.log` (all PASS, 0 failures).

Reproducibility: the RAM images are byte-identical when rebuilt from the same FD and ADT (checked 61 s apart). Two things
change per build by design or by tool: the ADT's `/chosen/random-seed` (256 fresh bytes, UniXNU's generator, kernel boot
entropy) and PcdDxe's CodeView GUID inside the FD (`logs/fd/reproducibility.txt`). So these hashes identify this build.

## Outputs

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `fd/piano-uefi-A-bundled.fd` | 3145728 | f74807d387e3b570039f4a5b4f5a8e5d22dda32fa89c7978e209b7d98b922f11 | pianoPkg FD, piano-uefi's bundled DXEs (other firmware build) |
| `fd/piano-uefi-B-own303.fd` | 3145728 | f88f5fcdbb99a96afad7ba43696e77b33463ab732737da5106f1ac239e190ead | pianoPkg FD, this tablet's own OS3.0.303 DXEs (UFSDxe unpatched) |
| `k27/kc-k27-native.bin` | 92782592 | e12a2ad3bc50027e0e6fb91de380e1b1d637a02a93d16260cdafaaf62cbc0af0 | k27 native-variant kernel collection (kernel 0054 native + the VM's kext set) |
| `adt/piano-k27.adt` | 14896 | f9b45c6560e05fac3c315f3cf6f219b39cad1a729cbd67c0b6d639657c18c695 | piano ADT (UniXNU generate, dts/piano.dts + configs/k27-adt.toml) |
| `ram/boot-piano-uefi-ram-A-bundled.img` | 11837440 | d326172ae388c191e40714c7e6ce3a4caade5544ce46fc132bb8d67b5fd60121 | M1 image, unsigned (as the Ubuntu RAM set) |
| `ram/boot-piano-uefi-ram-B-own303.img` | 11837440 | b8e01cfa00c525462a5dbf39baafe02793a345bacbf8ffb8ae32cba6e240989b | M1 image, unsigned |
| `ram/boot-piano-macos-ram-A-bundled.img` | 203726848 | 77a79f9a57e23ed715b3ea82d50f9485356008f953dba05b21281b6791a33656 | M2 image, unsigned |
| `ram/boot-piano-macos-ram-B-own303.img` | 203726848 | b7d36b2b2224d62a252e2e7c0c8c02c17de1ed7b41b7f66f7be6fc90be479ec3 | M2 image, unsigned |
| `ram/boot-piano-uefi-ram-A-bundled-signed.img` | 100663296 | 7754b8cbfa593aafb6068c8c97827770e10d6fbf45072e6ce7913b6b06e728e6 | AVB footer, partition_size = boot (96 MiB) |
| `ram/boot-piano-uefi-ram-B-own303-signed.img` | 100663296 | 4d03f3d46d112b38a3e1159d26a93b1195063d0358421163875254714cea0c08 | AVB footer, partition_size = boot |
| `ram/boot-piano-macos-ram-A-bundled-signed.img` | 205824000 | f3efb4c6d93763f9fe29890514f640976ac68df1ebef42a50697847fab8763d2 | AVB footer, partition_size 205824000 (> boot: RAM only) |
| `ram/boot-piano-macos-ram-B-own303-signed.img` | 205824000 | 977e55ee62513ae23959af6f654d60167c0300902be10ef139cb6b86e6a360b7 | AVB footer, partition_size 205824000 |
| `keys/piano-boot-signing.avbpubkey` | | 6b38a68872db145dbee63091b487a669f913574c38a8be4a8883a0019aef78b5 | public key; AVB sha1 f5151930dc69791637b2a48505cb175f5f78a522 = the key of this tablet's boot_a |
| `keys/piano-boot-signing.pub.pem` | | 12b38cc79502bff4b2436cb13541f52b37184b2ec131aec666692dc50aebad3b | same, PEM (verify_image) |

Signing: SHA256_RSA4096, partition `boot`, rollback index 1769904000, the three `com.android.build.boot.*` properties of
`boot-profile.json` (OS3.0.303 fingerprint), exactly as `build-persistent-boot.py` signs the installed set; done on
the signing host, where the private key stays.

## Inputs

| File | SHA-256 | From |
|---|---|---|
| `inputs/piano-boot.dtb` (147436 B) | 50636effb5f54306ec0ab9a3061cd163d7f003d8797e04585a33205918856840 | v1.1.0 release `piano-boot.dtb` = the DTB of the Ubuntu RAM images (maintenance-manifest.json) |
| `inputs/ubuntu-boot-ram-maintenance.img` | 54001a50a13fe80c3bd8e926161608b71d91e57e2d6be6b83cdc2bc62b5f6fbc | v1.1.0 release, the reference v2 container |
| `inputs/kernel.macho.trustcache` (446804 B) | 2b87b2241090c90508f26c8ca33129f7540922481a102359e99cf0adf00d10d6 | the VM accept boot disk |
| `inputs/scratch-data.img` (83967024 B) | cb50ceac16394c9f1da7f141ce0bb0b831b5ffac6bc9eeb56a9af99b1c147c11 | the VM accept boot disk (Data seed; = tablet data-store.seed) |
| AOSP `mkbootimg.py` | 37d84b3d162e0bc62e36c1f4e1c63c85ea0caa9f29be023eb2f8efe006ad948c | v1.1.0 host-tools (the Ubuntu RAM set's) |
| `avbtool.py` 1.3.0 | e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b | xiaomipad-8pro-mainline `tools/aosp-avb.json` pin |
| k27 kernel `kernel-k27-0054-native.macho` | 20a5b4fdcf54b6ac6e8f1878856b6049fc800591d6936c204545e15c53ebbfae | the k27 tree at 11ac316 (pin `k27` of unixnu.lock), `K27_VARIANT=native` |
| k27 kernel `kernel-k27-0054-vm.macho` | a8de3ece88b0477aa01e4bdfbdd8132b3ab04a5e4754ee212a63b3fbb6b74cf6 | same tree, `K27_VARIANT=vm` (not deployed) |
| tablet `uefi_a` (OS3.0.303, BOOT.MXF.2.5.1-00265) | cee5757c034be1d5d656ace60d5f43fd242c19c511bbf52907655d2edf333a00 | 2026-11-10 backup `sde/1-uefi_a.img.zst`; source of FD B's DXEs (kept outside every repo) |
| piano hardware DTB `adt/piano-k27.hw.dtb` | 3e8ec34e612242bb93182713c77b8209890407b6497fc71042fac16e5ad5760d | dtc of dts/piano.dts + sm8750.dtsi |

## Persistent boot (2026-11-11, docs/persistent-boot.md)

Built offline by `tooling/persist/build-all.sh` (`ESP_PROFILE=base`, `ESP_UTC=1794350126`) in an agent worktree; files under `out/`
(ignored by git). Status: **OFFLINE_BUILD_NOT_DEVICE_TESTED**; the `-signed` images do not exist yet (`tooling/persist/sign.sh`
makes them on the signing host). The FD build is not bit-reproducible: everything below is tied to this FD.

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `fd/piano-uefi-A-bundled.fd` | 3145728 | 6d15f0f830e5ea4d7d456d1ffd239a62c996c51c5e91649bedd29df334f26802 | FD A + `firmware/patches/unixnu/0001` (RAM payload first; patch sha256 1be90f66…) |
| `persistent/kernel-field.bin` | 3293356 | a1df7f145cf2d851fa2cdb3a8f8d3986bc9b4cf67d1660492e0f673c4da485ff | BootShim + that FD + boot DTB (the same field as the RAM images below) |
| `persistent/boot-piano-macos-persistent-v4.img` | 3301376 | 605c3a60254efb3620a1a4e8843d2effb4ffadcff831efd67631bec81201f6c5 | boot_a image, v4, unsigned |
| `persistent/boot-piano-esp-test-v2.img` | 3452928 | 04ed0d00483978e22bc67aed48558fc758148bc85c07a6d386d03c33fd5d11a6 | RAM test of the flashed path (v2, 4096-byte zero ramdisk); signed = the v2 fallback for boot_a |
| `persistent/esp-base-diag.img` | 1073741824 | a7a2afefc6af46feb519861a8e2a8fd5c726517dfc28cdf7937cd39d9121f760 | ESP, FAT32 4 KiB sectors, default `macos-diag` (reset to fastboot after 240 s) |
| `persistent/esp-base-macos.img` | 1073741824 | f5f4c6de3d61bed5b8337c2c964199c76e87445370c271a9a53ce99a9f301dd7 | ESP, default `macos` (daily use) |
| `persistent/gpt-head.bin` | 24576 | 8c3ab20f60226441096d4c61ea2671db605b4347163cd556e4080135eaab450e | LUN 0 LBA 0-5, new primary GPT (100 bytes differ from new-primary.bin: gpt-diff.txt) |
| `persistent/gpt-tail.bin` | 20480 | 39da467b3c6f99100583c5a0f42350a1d3c8e014be508f1fc53941414e7aa543 | LUN 0 LBA 124552187-124552191, new backup GPT |
| `ram/boot-piano-macos-ram-A-bundled.img` | 203726848 | c7f40f62500e36180a43883a79554f0a8b58037d18f91f1254e36e4d70b5a11a | dev RAM image with that FD (K27_ARGS, reset 45 s): `ram-precedence` |
| `ram-dump/boot-piano-macos-ram-A-bundled.img` | 203726848 | 0b129dfa8eda049b5210a572ad09eeea119f47979852d7988c01f5c1d2474b33 | the loop's dump image with that FD (reset 1 s, data-persist=0) |

ESP inputs: KC 415bbea57f0f6699… (8 native kexts), ADT bafbdaac91f0118f… (vmm-present 1), trust cache 2b87b2241090c905…, seed
cb50ceac16394c9f… (seed identity 469ed4401f5b5241…, 3145728 pages, store window 3145825 blocks). ESP partition GUID
3F8B0988-D851-5539-AE47-6E53C621D288.

ESP profile `gpu` (built 07:56 from a copy of the main checkout's 07:44 build, the one that showed the login screen on the A830 at
07:45: KC 68fd4487e36bff02… with PianoA830, ADT f441d8b108478d24… vmm-present 0, Turnip-staged trust cache 97fb617d9c274035… and
seed df5579ddb4a1d9ec… (seed identity aced5bf9794b42f3…), `ESP_UTC=1794354270`):

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `persistent/esp-gpu-diag.img` | 1073741824 | 97fecadfb84cc91ff5d64c6c7d99e9906de907e3c593d03c7cb80734716a5b18 | default `macos-diag` (session 1, RAM tests) |
| `persistent/esp-gpu-macos.img` | 1073741824 | 1120f7d86ce688f9e122f863f59f71e50a5865e17227fa999824e2d331970615 | default `macos` (session 3, `esp-final`) |
| `ram-precedence/boot-piano-macos-ram-A-bundled.img` | 203726848 | 7f8c2d6194d8c8c11f005572d107de6dbb495122ff14638c4e8557458faa4846 | `ram-precedence`'s dev image: patched FD, base inputs, reset 45 s, data-persist=0 (cannot reset the installed store) |

ESP v3 (`docs/persistent-boot.md` §11; built 2026-11-12 in an agent worktree, `--utc 1794427214`; **OFFLINE_BUILD_NOT_DEVICE_TESTED**,
not installed). The seed is the store's base, pinned from the installed `esp-daily2-macos` (`e0acb375…`) with
`tooling/persist/base-seed.py` (`docs/data-store.md` §9):

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `persistent/esp-daily3-macos.img` | 1073741824 | f6f7bb53658b3fd621d6dc5563bae6b1da2884613eb1b99a5767cbb484105309 | daily profile, default `macos`, `piano-boot-guard=180`, `--layer out/layer-v3` |
| `k27/kc-k27-native.bin` | 92782592 | 557420c421c42dce0bf3dc3beed655453a867995eb454a5152365293f86f39e2 | k27 0064, the 23 kexts of `configs/k27-native-kexts.txt` |
| `adt/piano-k27.adt` | 15432 | 1532b37e251640e5eb24e66505e6ccf7259d6a49df4f83bee35a3103876a0c6f | vmm-present 0 |
| `base/8f3331104e03c970/scratch-data.img` | 124665984 | 23a5ec59fc4c4c467e3dc6227cdf37ca291b5a48a270d08137ae4755535f6944 | the base: seed identity 8f3331104e03c970… (the installed store's) |
| `base/8f3331104e03c970/kernel.macho.trustcache` | 446880 | cd4aa536a546437007c708c8408f4ae0dd1734fb3dffec06b1d52d9a4c743e4c | the base's trust cache, 18611 CDHashes in 5 modules |
| `layer-v3/kernel.macho.trustcache` | 447004 | 607c60efd5565a90d4357b05949af8528547ccd97a5a84ac3230db13a322d213 | the base's + 1 module: Turnip 0006 `5efbe639…`, pianousb-shell `83876bc9…`, piano-rotated `c401d51b…`, piano-torch `f386f217…` |
| `layer-v3/userland-update.tar` | 41082880 | 5f48c12572e390d18cec5d6b9bf173b8765c56e5fdfae08a854c914ac4d5133d | the system layer (12 files, 6 units); `apply-update.sh` 240cd52b… |

The Turnip bundle built from a fresh worktree is byte for byte main's: `NativeAdrenoMetal` d4ffaf27…, `libvulkan_freedreno.dylib`
8c6958ba… (patch 0006), `Info.plist` 9595a4d2… (`docs/data-store.md` §9.4).


---

## Records of this worktree's line, 2026-11-19 to 11-25 (kept across the merge of upstream 480c661a)


Built on the Mac by `tooling/build-slice1.sh` (SIGN=1) at ~19:56 SGT from the pins of `unixnu.lock`; files under `out/`
(ignored by git). Status of every image: **OFFLINE_CANDIDATE_NOT_DEVICE_TESTED**, `fastboot boot` only, never flash.
Verification logs: `logs/bootimg/verify-*.log` (all PASS, 0 failures).

Reproducibility: the RAM images are byte-identical when rebuilt from the same FD and ADT (checked 61 s apart). Two things
change per build by design or by tool: the ADT's `/chosen/random-seed` (256 fresh bytes, UniXNU's generator, kernel boot
entropy) and PcdDxe's CodeView GUID inside the FD (`logs/fd/reproducibility.txt`). So these hashes identify this build.

## Outputs

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `fd/piano-uefi-A-bundled.fd` | 3145728 | f74807d387e3b570039f4a5b4f5a8e5d22dda32fa89c7978e209b7d98b922f11 | pianoPkg FD, piano-uefi's bundled DXEs (other firmware build) |
| `fd/piano-uefi-B-own303.fd` | 3145728 | f88f5fcdbb99a96afad7ba43696e77b33463ab732737da5106f1ac239e190ead | pianoPkg FD, this tablet's own OS3.0.303 DXEs (UFSDxe unpatched) |
| `k27/kc-k27-native.bin` | 92782592 | e12a2ad3bc50027e0e6fb91de380e1b1d637a02a93d16260cdafaaf62cbc0af0 | k27 native-variant kernel collection (kernel 0054 native + the VM's kext set) |
| `adt/piano-k27.adt` | 14896 | f9b45c6560e05fac3c315f3cf6f219b39cad1a729cbd67c0b6d639657c18c695 | piano ADT (UniXNU generate, dts/piano.dts + configs/k27-adt.toml) |
| `ram/boot-piano-uefi-ram-A-bundled.img` | 11837440 | d326172ae388c191e40714c7e6ce3a4caade5544ce46fc132bb8d67b5fd60121 | M1 image, unsigned (as the Ubuntu RAM set) |
| `ram/boot-piano-uefi-ram-B-own303.img` | 11837440 | b8e01cfa00c525462a5dbf39baafe02793a345bacbf8ffb8ae32cba6e240989b | M1 image, unsigned |
| `ram/boot-piano-macos-ram-A-bundled.img` | 203726848 | 77a79f9a57e23ed715b3ea82d50f9485356008f953dba05b21281b6791a33656 | M2 image, unsigned |
| `ram/boot-piano-macos-ram-B-own303.img` | 203726848 | b7d36b2b2224d62a252e2e7c0c8c02c17de1ed7b41b7f66f7be6fc90be479ec3 | M2 image, unsigned |
| `ram/boot-piano-uefi-ram-A-bundled-signed.img` | 100663296 | 7754b8cbfa593aafb6068c8c97827770e10d6fbf45072e6ce7913b6b06e728e6 | AVB footer, partition_size = boot (96 MiB) |
| `ram/boot-piano-uefi-ram-B-own303-signed.img` | 100663296 | 4d03f3d46d112b38a3e1159d26a93b1195063d0358421163875254714cea0c08 | AVB footer, partition_size = boot |
| `ram/boot-piano-macos-ram-A-bundled-signed.img` | 205824000 | f3efb4c6d93763f9fe29890514f640976ac68df1ebef42a50697847fab8763d2 | AVB footer, partition_size 205824000 (> boot: RAM only) |
| `ram/boot-piano-macos-ram-B-own303-signed.img` | 205824000 | 977e55ee62513ae23959af6f654d60167c0300902be10ef139cb6b86e6a360b7 | AVB footer, partition_size 205824000 |
| `keys/piano-boot-signing.avbpubkey` | | 6b38a68872db145dbee63091b487a669f913574c38a8be4a8883a0019aef78b5 | public key; AVB sha1 f5151930dc69791637b2a48505cb175f5f78a522 = the key of this tablet's boot_a |
| `keys/piano-boot-signing.pub.pem` | | 12b38cc79502bff4b2436cb13541f52b37184b2ec131aec666692dc50aebad3b | same, PEM (verify_image) |

Signing: SHA256_RSA4096, partition `boot`, rollback index 1769904000, the three `com.android.build.boot.*` properties of
`boot-profile.json` (OS3.0.303 fingerprint), exactly as `build-persistent-boot.py` signs the installed set; done on
the signing host, where the private key stays.

## Inputs

| File | SHA-256 | From |
|---|---|---|
| `inputs/piano-boot.dtb` (147436 B) | 50636effb5f54306ec0ab9a3061cd163d7f003d8797e04585a33205918856840 | v1.1.0 release `piano-boot.dtb` = the DTB of the Ubuntu RAM images (maintenance-manifest.json) |
| `inputs/ubuntu-boot-ram-maintenance.img` | 54001a50a13fe80c3bd8e926161608b71d91e57e2d6be6b83cdc2bc62b5f6fbc | v1.1.0 release, the reference v2 container |
| `inputs/kernel.macho.trustcache` (446804 B) | 2b87b2241090c90508f26c8ca33129f7540922481a102359e99cf0adf00d10d6 | the VM accept boot disk |
| `inputs/scratch-data.img` (83967024 B) | cb50ceac16394c9f1da7f141ce0bb0b831b5ffac6bc9eeb56a9af99b1c147c11 | the VM accept boot disk (Data seed; = tablet data-store.seed) |
| AOSP `mkbootimg.py` | 37d84b3d162e0bc62e36c1f4e1c63c85ea0caa9f29be023eb2f8efe006ad948c | v1.1.0 host-tools (the Ubuntu RAM set's) |
| `avbtool.py` 1.3.0 | e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b | xiaomipad-8pro-mainline `tools/aosp-avb.json` pin |
| k27 kernel `kernel-k27-0054-native.macho` | 20a5b4fdcf54b6ac6e8f1878856b6049fc800591d6936c204545e15c53ebbfae | the k27 tree at 11ac316 (pin `k27` of unixnu.lock), `K27_VARIANT=native` |
| k27 kernel `kernel-k27-0054-vm.macho` | a8de3ece88b0477aa01e4bdfbdd8132b3ab04a5e4754ee212a63b3fbb6b74cf6 | same tree, `K27_VARIANT=vm` (not deployed) |
| tablet `uefi_a` (OS3.0.303, BOOT.MXF.2.5.1-00265) | cee5757c034be1d5d656ace60d5f43fd242c19c511bbf52907655d2edf333a00 | 2026-11-10 backup `sde/1-uefi_a.img.zst`; source of FD B's DXEs (kept outside every repo) |
| `inputs/v110/release/boot/boot.img` (fetched 2026-11-25) | d8d812798b1ec042eff860f122013617b41f194323aa9607ae0f2e7735b12b3a | Ubuntu v1.1.0 release `SHA256SUMS`, slot A candidate |
| `inputs/v110/release/boot/init_boot.img` | 96057eb263fbe09284ba8bbe888128dbc0b36525dec4b01da918dcb7c6fe10ef | same |
| `inputs/v110/release/boot/vendor_boot.img` (100663296 B) | 7cbb06b9eddb8051c201d7a1f672620a5440283f0d5edbf0ace9270c05a60174 | same; the partial download of 2026-11-25 02:33 was completed and verified |
| `inputs/v110/release/boot/boot-ram-check.img` | 9071c5afd4be8637254795fea92df8f3a5ab67541a438d5d0b329dd246f230df | same |
| piano hardware DTB `adt/piano-k27.hw.dtb` | 3e8ec34e612242bb93182713c77b8209890407b6497fc71042fac16e5ad5760d | dtc of dts/piano.dts + sm8750.dtsi |

The whole `piano-ubuntu-v1.1.0-boot.zip` (153,123,957 B, 16 entries) passes `zipfile.testzip()` and every file under
`release/boot/` matches the release `SHA256SUMS`, so the slot A write set (`boot.img`, `init_boot.img`, `vendor_boot.img`,
`piano-boot.dtb`) is fully materialized offline. Built and signed on the v1.1.0 build host; not booted from this worktree.

## Persistent boot (2026-11-11, docs/persistent-boot.md)

Built offline by `tooling/persist/build-all.sh` (`ESP_PROFILE=base`, `ESP_UTC=1794350126`) in an agent worktree; files under `out/`
(ignored by git). Status: **OFFLINE_BUILD_NOT_DEVICE_TESTED**; the `-signed` images do not exist yet (`tooling/persist/sign.sh`
makes them on the signing host). The FD build is not bit-reproducible: everything below is tied to this FD.

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `fd/piano-uefi-A-bundled.fd` | 3145728 | 6d15f0f830e5ea4d7d456d1ffd239a62c996c51c5e91649bedd29df334f26802 | FD A + `firmware/patches/unixnu/0001` (RAM payload first; patch sha256 1be90f66…) |
| `persistent/kernel-field.bin` | 3293356 | a1df7f145cf2d851fa2cdb3a8f8d3986bc9b4cf67d1660492e0f673c4da485ff | BootShim + that FD + boot DTB (the same field as the RAM images below) |
| `persistent/boot-piano-macos-persistent-v4.img` | 3301376 | 605c3a60254efb3620a1a4e8843d2effb4ffadcff831efd67631bec81201f6c5 | boot_a image, v4, unsigned |
| `persistent/boot-piano-esp-test-v2.img` | 3452928 | 04ed0d00483978e22bc67aed48558fc758148bc85c07a6d386d03c33fd5d11a6 | RAM test of the flashed path (v2, 4096-byte zero ramdisk); signed = the v2 fallback for boot_a |
| `persistent/esp-base-diag.img` | 1073741824 | a7a2afefc6af46feb519861a8e2a8fd5c726517dfc28cdf7937cd39d9121f760 | ESP, FAT32 4 KiB sectors, default `macos-diag` (reset to fastboot after 240 s) |
| `persistent/esp-base-macos.img` | 1073741824 | f5f4c6de3d61bed5b8337c2c964199c76e87445370c271a9a53ce99a9f301dd7 | ESP, default `macos` (daily use) |
| `persistent/gpt-head.bin` | 24576 | 8c3ab20f60226441096d4c61ea2671db605b4347163cd556e4080135eaab450e | LUN 0 LBA 0-5, new primary GPT (100 bytes differ from new-primary.bin: gpt-diff.txt) |
| `persistent/gpt-tail.bin` | 20480 | 39da467b3c6f99100583c5a0f42350a1d3c8e014be508f1fc53941414e7aa543 | LUN 0 LBA 124552187-124552191, new backup GPT |
| `ram/boot-piano-macos-ram-A-bundled.img` | 203726848 | c7f40f62500e36180a43883a79554f0a8b58037d18f91f1254e36e4d70b5a11a | dev RAM image with that FD (K27_ARGS, reset 45 s): `ram-precedence` |
| `ram-dump/boot-piano-macos-ram-A-bundled.img` | 203726848 | 0b129dfa8eda049b5210a572ad09eeea119f47979852d7988c01f5c1d2474b33 | the loop's dump image with that FD (reset 1 s, data-persist=0) |

ESP inputs: KC 415bbea57f0f6699… (8 native kexts), ADT bafbdaac91f0118f… (vmm-present 1), trust cache 2b87b2241090c905…, seed
cb50ceac16394c9f… (seed identity 469ed4401f5b5241…, 3145728 pages, store window 3145825 blocks). ESP partition GUID
3F8B0988-D851-5539-AE47-6E53C621D288.

ESP profile `gpu` (built 07:56 from a copy of the main checkout's 07:44 build, the one that showed the login screen on the A830 at
07:45: KC 68fd4487e36bff02… with PianoA830, ADT f441d8b108478d24… vmm-present 0, Turnip-staged trust cache 97fb617d9c274035… and
seed df5579ddb4a1d9ec… (seed identity aced5bf9794b42f3…), `ESP_UTC=1794354270`):

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `persistent/esp-gpu-diag.img` | 1073741824 | 97fecadfb84cc91ff5d64c6c7d99e9906de907e3c593d03c7cb80734716a5b18 | default `macos-diag` (session 1, RAM tests) |
| `persistent/esp-gpu-macos.img` | 1073741824 | 1120f7d86ce688f9e122f863f59f71e50a5865e17227fa999824e2d331970615 | default `macos` (session 3, `esp-final`) |
| `ram-precedence/boot-piano-macos-ram-A-bundled.img` | 203726848 | 7f8c2d6194d8c8c11f005572d107de6dbb495122ff14638c4e8557458faa4846 | `ram-precedence`'s dev image: patched FD, base inputs, reset 45 s, data-persist=0 (cannot reset the installed store) |

ESP v3 (`docs/persistent-boot.md` §11; built 2026-11-12 in an agent worktree, `--utc 1794427214`; **OFFLINE_BUILD_NOT_DEVICE_TESTED**,
not installed). The seed is the store's base, pinned from the installed `esp-daily2-macos` (`e0acb375…`) with
`tooling/persist/base-seed.py` (`docs/data-store.md` §9):

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `persistent/esp-daily3-macos.img` | 1073741824 | f6f7bb53658b3fd621d6dc5563bae6b1da2884613eb1b99a5767cbb484105309 | daily profile, default `macos`, `piano-boot-guard=180`, `--layer out/layer-v3` |
| `k27/kc-k27-native.bin` | 92782592 | 557420c421c42dce0bf3dc3beed655453a867995eb454a5152365293f86f39e2 | k27 0064, the 23 kexts of `configs/k27-native-kexts.txt` |
| `adt/piano-k27.adt` | 15432 | 1532b37e251640e5eb24e66505e6ccf7259d6a49df4f83bee35a3103876a0c6f | vmm-present 0 |
| `base/8f3331104e03c970/scratch-data.img` | 124665984 | 23a5ec59fc4c4c467e3dc6227cdf37ca291b5a48a270d08137ae4755535f6944 | the base: seed identity 8f3331104e03c970… (the installed store's) |
| `base/8f3331104e03c970/kernel.macho.trustcache` | 446880 | cd4aa536a546437007c708c8408f4ae0dd1734fb3dffec06b1d52d9a4c743e4c | the base's trust cache, 18611 CDHashes in 5 modules |
| `layer-v3/kernel.macho.trustcache` | 447004 | 607c60efd5565a90d4357b05949af8528547ccd97a5a84ac3230db13a322d213 | the base's + 1 module: Turnip 0006 `5efbe639…`, pianousb-shell `83876bc9…`, piano-rotated `c401d51b…`, piano-torch `f386f217…` |
| `layer-v3/userland-update.tar` | 41082880 | 5f48c12572e390d18cec5d6b9bf173b8765c56e5fdfae08a854c914ac4d5133d | the system layer (12 files, 6 units); `apply-update.sh` 240cd52b… |

The Turnip bundle built from a fresh worktree is byte for byte main's: `NativeAdrenoMetal` d4ffaf27…, `libvulkan_freedreno.dylib`
8c6958ba… (patch 0006), `Info.plist` 9595a4d2… (`docs/data-store.md` §9.4).


## Independent host build, 2026-11-20

| Local artifact | SHA-256 |
| --- | --- |
| XNU source snapshot | `119dc3f35273084c1629d2fc7744442fb11b0c47638d340c17e7e38d2ee41576` |
| Build inputs and recipes archive | `f687af31008c76d57978a0712e7c6ec8f3c08dd60acbda5deef2ffbf02437c1d` |
| First linked measurement kernel with binary symbol sets | `9471d79293ff8fb9e66c8fb79bc852f42719747ac7addee1f67749193bfda0c0` |
| Measurement kernel reproduced twice with fixed object mtimes | `d827848fe3a905d068b521e298dcd70a3bcc03988c753bdbf0a74001b76fd2ba` |

The kernel is a host sizing artifact. The mac17j layout check failed. [Measurements and scope](../docs/results-2026-11-20-independent-k27.md).

The Apple clang object checkpoint archive SHA-256 is `3c04ed4434e30a079fd604872334c63d4d4d846229b7078f296e7b679c42e09b`. [Mac object verification](../docs/results-2026-11-20-mac-k27-objects.md).

## Mac native kext objects, 2026-11-20

| Local artifact | SHA-256 |
| --- | --- |
| PianoBacklight.o | `b38bcf5384aaa595cad11ab90fcf594c9844569c2aa2b69f976c1267b73ec634` |
| NativeKextGlue.o (shared by the three kexts) | `0c8a17d0da036f256a89657e9e41841d2f86014ad57427431c27408329b2e68a` |
| DpuFlip.o | `49c6c1ff1382245fa3690ce7a8220652cff109b58bd3aaec29d4e87b1986f9e0` |
| DpuSlices.o | `da2aaf95fe53c73c30c886885e15d46da520a4e6ba78950a21a692fd64060fb6` |
| PianoDpu.o | `d3495c3ae5ef61e6ae424ceecf9c8f0bb9abc1475a625027b4c27bc45f1a0576` |
| NativeAES.o | `04df3b94746f41bfe08607833bf4721d53073018d1bc8ade40a2f5a19d64bccf` |

All objects have subtype `0xc0000002`. [Build evidence](../docs/results-2026-11-20-mac-native-drivers.md).

| Local artifact | SHA-256 |
| --- | --- |
| Mac external archive build inputs | `7488a65a0fc7637bf5905011bfb8e5bdac6fb56e8e8596ac5a0aea6faaf6fb12` |
| Mac positioned kernel (28,026,624 B) | `25a417668cbba250ee0d055935ef0b2da1381f7224237fe1aa089cff607e6d31` |
| 26-descriptor KC preflight log | `3c31d03862c731fe42e74532f5fbb08dc64cbc9c5362132c7e58873af6bee84c` |

The Mac kernel and KC preflight have host evidence in the [dated result](../docs/results-2026-11-20-mac-k27-kc-preflight.md).

The 2026-11-21 BDS console diagnostic FD SHA-256 is `daf8a0d5bd2ee8ecf274fa426f787a51450f2403433c9759180d8bc5ceb139bd`. The temporary RAM boot container SHA-256 is `5640d89d7bfe7b53cb5f0011c299e56455910878876856dd37ce2e16b3489724`. Host verification passed. Tablet screen output is pending.

The follow-up USB diagnostic FD SHA-256 is `970c7ae888c319323ec7d2d66f01f5d82e618f8bcc8d8344167dac71cec387f2`; its RAM boot image SHA-256 is `12802a1c97e75aa473090e3012b8c79592b880454da649c5e990d0a0e598135f`. [Tablet result](../docs/results-2026-11-21-bds-usb.md).

The generated FAT16 diagnostic container SHA-256 is `708d0e79ffa8adae5227eae5dbdff27121da4fa7e9530db31f71dd5153570468`. [Packaging and tablet result](../docs/results-2026-11-21-fat16-packaging-replay.md).

## Historical fips4b inventory (2026-11-21)

See [the image inventory](../docs/results-2026-11-21-fips4b-inventory.md) for the macOS RAM image and its provenance limits. The paired UEFI image SHA-256 is c5b513b5ed5b3f154110b330f932991310fcaefcc8989853d0a1d22d65053c6e. The manifest SHA-256 is 92e1459df7560e55c3b292b20098f913d6c5f181b354f0d0cef0c38719bdb363.

- 2026-11-21 NativeUfs probe 2 host KC, `readonly-inspect.kc` (135806976 bytes), SHA-256 `80da9440d3ec0f610dc3721d10f4fc36cad72c88e14a00f09f79cdc73165d18d`.

- 2026-11-21 probe 2 mailbox RAM candidate, boot-piano-macos-ram-inspect-mailbox.img (185733120 bytes), SHA-256 56c84e6547c1b21a18854a8acc754f050c1a1cbb6595977899b3d6c94e61a536.

- Probe 2 mailbox verifier log: `/tmp/piano-inspect-relink.BlbwS0/verify.log`; SHA-256 `649d7e15522fdae4bfadb8ec3b39a5d0e95d0a8d820b21f00578e18ebf21187f`.

- 2026-11-21 4 KiB F2FS mkfs log, `/tmp/piano-f2fs-loop.c99i5s/mkfs.log` SHA-256 `2ff21545b367c68cfe4ee2453031aaa5f22abe19f25493295889b6273ff1ed83`; dry-run fsck log `/tmp/piano-f2fs-loop.c99i5s/fsck.log` SHA-256 `8128dbcc3085fa8289f8dd95a27112f9afbcf991ff696ea24fb52d8275e9aed3`.

- 2026-11-21 firmware dispatch timing candidate, `out/boot-piano-darwin-lookup-auto2.img`, SHA-256 `36d8406c0afc5c7b7d6bcf0872a2dec559d3d7df9962953eb13802c8a936d11b`. Host verified that the kernel and DTB sections match lookup-auto8, the boot header ID was recomputed, and the only live FAT file difference is the menu timeout (8 to 2 seconds). [Hardware observation](../docs/results-2026-11-21-darwin-lookup-auto2.md).

- Gunyah watchdog status diagnostic, `out/boot-piano-watchdog-status.img` (13,766,656 bytes), SHA-256 `0843d0bb05caceb9c2e2a534eb43631a145688489bdd336ee13148135fb04adb`. Linked FD SHA-256 `e603a803e6951e59c559d82d0fa8db6e8de393c2f92943bf4bbc49367cd7a9ae`. Host verified the Android v2 header ID and byte equality outside the FD and ID fields against the action-only lookup image. Tablet run pending.

- Gunyah watchdog expiry diagnostic, `out/boot-piano-watchdog-expiry.img` (13,766,656 bytes), SHA-256 `09a30114856cb3712158577292bfc3056e294c8194818ccd33f7adc7c4542e46`. Linked FD SHA-256 `83a7b411dfc553afc2964fd9eeee12246796b382177954797cde84d10072576d`. Android v2 ID and FD-only payload difference verified. See [tablet result](../docs/results-2026-11-21-uefi-watchdog-expiry.md).

- Post-ExitBootServices watchdog RAM diagnostic, `out/boot-piano-watchdog-post-ebs.img` (13,766,656 bytes), SHA-256 `6709d0fd6b4569f03e62146cd6d21ebae2b94645a15ee932e8ec0c92bf1a2183`. Linked FD SHA-256 `2ed78a1f52b147ac32e5259aa6e8bf992031a320fc5fa0040bb41d24949ccc82`. Build log `UbuntuPiano:/tmp/piano-post-ebs.mIk1NZN0/build-retry.log`. See [tablet result](../docs/results-2026-11-21-watchdog-post-ebs.md).

- DarwinBoot watchdog stop FFS (host build only), `UbuntuPiano:/tmp/darwinboot-post-ebs.ffs`, SHA-256 `6563739a80454379e850bc9fa5d08bc01fff23c408902e75e338cb9badfbf034`.

- Combined DarwinBoot watchdog RAM image, out/boot-piano-darwinboot-watchdog-stop.img, SHA-256 1adc49c0a613beb7775f4276978cfa55528e911cff02211d72e5b085e36a9e4c. FD SHA-256 31f490b6e1f4589f5d6657f18128822ab1ea01340b0b370ec37a14577cdd0ce3.

- DarwinBoot witness RAM image, out/boot-piano-darwinboot-witness.img, SHA-256 a74a9e605283aa68c6f5cecf6e1e1fd624e0a1c767774ee302fde868a3f945aa. See [tablet result](../docs/results-2026-11-21-darwinboot-witness.md).
- DarwinBoot witness FD replay, UbuntuPiano build cache `Build/pianoPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`, SHA-256 `5506f1aa4035013108ddbebf0ae620e06a1118864ee5fcca0744f8677ce8f304`. [Host comparison](../docs/results-2026-11-21-darwinboot-build-replay.md).

- DarwinBoot EBS mailbox diagnostic FD SHA-256 `777b2cedd454fc7f65a1ad77c11d7dc473044d18f5ec4cc0405b16958307d102`. `out/boot-piano-darwinboot-mailbox.img` SHA-256 `978d5a787b1bbcc44ff33d0471fef54947a8f96b8c0a20c8f8df3cba895db42f`. [Host result](../docs/results-2026-11-21-darwinboot-mailbox.md).

- No-poll watchdog control, `out/boot-piano-witness-watchdog-gate-no-poll.img` (185,733,120 bytes), SHA-256 `4a03fb5d42d97340272b2653cb5fece3d8b9c65742db30b02b478b6efc2d8a74`. [Host result](../docs/results-2026-11-22-watchdog-no-poll-control.md).

- Watchdog-enter no-poll image, `out/boot-piano-watchdog-enter-no-poll.img`, SHA-256 `1c4feecd143e8c990e42dc022a32100094917a05570efd825429faddba33d90a`. The DEBUG CLANGPDB build log is `UbuntuPiano:/tmp/piano-watchdog-enter.hf3u5UqC/build.log`.

- SHA-256 HMAC-final RAM candidate, `out/no-demura-control/boot-piano-sha2final.img` (185,733,120 bytes), SHA-256 `F77C8F83538A4FEEF6D1F8893B9274415F6855BF4C1783A11F4A3A07F7D504F4`; packed KC SHA-256 `0ABF77FBFA77B493EFA26DDF75509F32DD391418F572C94262D20E7E5C6E9489`. Tablet run reached phase 62 after `bootseed_init()` and returned unattended to HyperOS A. [Tablet result](../docs/results-2026-11-22-sha2final-live.md).

- DRBG direct-call boundary image, `out/no-demura-control/boot-piano-drbgdirect.img`, SHA-256 `4370DAE75112483447491DBFB1BF808287078792B89C6FCA1D505FC531B7549B`. Tablet run stopped at phase 232, before the first DRBG `update()` call, and returned unattended to HyperOS A. See [tablet result](../docs/results-2026-11-22-drbg-sha256-live.md).

- DRBG update boundary image, `out/no-demura-control/boot-piano-drbgupdate.img`, SHA-256 `0471C9E3203CAF47CCEDDCEB7ED47E87358ED89D99DE2A2408169B6987A02C72`; Android boot v2 ID `79969940eeda7b7180524743c66031cb3851ae90`. Tablet run stopped at phase 236, at the first loop entry inside `update()`, and returned unattended to HyperOS A.

- SHA-256 HMAC DRBG RAM candidate, `out/no-demura-control/boot-piano-drbgsha256.img` (185,733,120 bytes), SHA-256 `19C094B7179F0C493E887DA33679E2D50960ACC46E8E378E0AEF2D9D8ABEEA51`; packed KC SHA-256 `e6ed0bd7f8554f0f55583a651ebd0c72a890d45d7fdae505b5461fb675a3525a`; kernel SHA-256 `85299f0310e4b8ca093e5f2db3e2e4a4d018780a0d77509ac536d0efd0484371`; `cchmac_init.o` SHA-256 `ba36b8bc1d458f488053de672deeeda433bd4f08b934eb4aa5b3c91d4415b6c1`. Tablet run reached phase 13 before `machine_startup()` and returned unattended to completed HyperOS A. [Tablet result](../docs/results-2026-11-22-drbg-sha256-live.md).

- paclr53 UFS root backstop image, `out/no-demura-control/boot-piano-paclr53-ufsroot3.img` (185,733,120 bytes), SHA-256 `e8d21614f587eba4c488453467d31f35ff9b74027943be63104c426dab0b93e7`. Same payload as `fc9cfa24…` (kernel `4efc1dea…`, trustcache `002db2e6…`, ADT `4e904d90…`) with `piano-reset-after=180 piano-reset-to=bootloader piano-boot-guard=150` added. Not yet run. See [host note](../docs/results-2026-11-23-paclr53-ufsroot-backstop.md).

- reset-timer control image, `out/no-demura-control/boot-piano-paclr53-timercheck.img` (185,733,120 bytes), SHA-256 `678f6729f9d2e6c8a9c59d58158cd99aa0ab7624b19de74da2553487fdca26f4`. Same paclr53 payload as the rooted image with the short fips4 argument line, `native-ufs-probe=2` and `piano-reset-after=20 piano-reset-to=bootloader`. Built, to be run: the tablet was off USB. See [probe result](../docs/results-2026-11-23-paclr53-probe11-probe2-noreturn.md).

- short-argument root image, `out/no-demura-control/boot-piano-paclr53-rootshort.img` (185,733,120 bytes), SHA-256 `a324ebe1e17b8aa7b219757f844934bd229b10890aa7cc96eaf9abb12089c46f`. The timercheck line with `native-ufs-root=1 native-ufs-root-part=linux_rootfs native-ufs-diagnostics=1` and `piano-reset-after=20`. Built, to be run. See [probe result](../docs/results-2026-11-23-paclr53-probe11-probe2-noreturn.md).
- Both short-argument images above were run on device `7ccdd855` on 2026-11-23. Each returned the fastboot serial at 80.1 s with no manual power cycle and HyperOS reached `boot_completed=1`; the console shows `piano-reset-after: reset at 20 s, Gunyah watchdog armed (32 s bite)` and no `20 s reached` line, so the 32 s Gunyah bite returned them. The root image stopped after `NativeUfs: DMA attempt 1` without any `DMA virt=` line. See [watchdog return](../docs/results-2026-11-23-paclr53-watchdog-return.md).

## Native UFS RAM-boot probes (2026-11-23)

Built on the Mac from the pinned k27 tree (/private/tmp/piano-k27-independent.zbovHflG) with the NativeUfs object
rebuilt from package/NativeUfs; each was astboot boot only and returned to HyperOS unattended. See
`docs/results-2026-11-23-native-ufs-root-window.md`.

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `out/no-demura-control/boot-piano-ufs17mark.img` | 185733120 | 0c197b68af2a0d22f84a4c537ab268c797cfbdd726d7d23498a555b2b5beadf2 | ring instrumentation; NOP did not report |
| `out/no-demura-control/boot-piano-ufs18mark.img` | 185733120 | fcc19758c3802986dcf3af1938f5ee9adcde2ebddce20cee97f41f88d26ba78c | NOP and query complete |
| `out/no-demura-control/boot-piano-ufs19mark.img` | 185733120 | e0aa46c73cdefe1ea6adff28566999e98c111da1eb0c09d9f5d06b5af740f1fd | SCSI wake completes, IOSleep stops |
| `out/no-demura-control/boot-piano-ufs20mark.img` | 185733120 | c9615fe1b13a2c6c392670b73e5c25ca484ba71802d221f3d4b4fe288594ad2f | IODelay spin still stops |
| `out/no-demura-control/boot-piano-ufs21mark.img` | 185733120 | 503c3151616c0bf58f4684d4886c5060c0019c08a50dcb306777e6ec7d830611 | cntvct spin; root windows published |

## Device-layout ESP (2026-11-24, offline)

- `esp-device.img` on the Mac at `~/Codex/k273-esp/out/esp-device.img`, 1,073,741,824 bytes, SHA-256 `68c28df15bcf55a589441df37e9c5fd94614e2771c437e955f190e3eb0aab898`, built twice byte-identically with a fixed `--utc`. FAT hidden sectors 124,289,792; record `~/Codex/k273-esp/out/esp-device.json` says `OFFLINE_BUILD_NOT_DEVICE_TESTED`. Inputs: `piano-ufs264.kc` (`529265c7…`), `paclr53.adt` (`4e904d90…`), `kernel.macho.trustcache.3mod` (`9d225ea3…`), the retained 24 GiB first-install seed (file `48dbc08d…`, seed identity `7e9bd39b…`; see [record audit](../docs/results-2026-11-25-esp-record-audit.md)), and the two pinned firmware files. Host geometry test: `tooling/persist/test-esp-geometry.py`.
| `out/persistent/boot-piano-macos-persistent-v5-ufs.img` | 100663296 | 5de858b8157e9572f9f44ad9648c87d669d6a53e5aecb50630df34d0b080f19c | slot A boot: v4 BootShim+DTB with FD `394c5c78` (UFSDxe, `config: volume`, 13 GiB window), AVB signed; RAM trial pending |

## Mac-built FD and v6 boot image (2026-11-25)

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `out/persistent/piano-uefi-mac-full.fd` | 3145728 | 3fcc84291a42275cd8feffc5c00192af1e2717660c1385e6d6964e1122db7037 | full `build-fd.sh` output on the Mac: UFSDxe plus patched UniXNU and DarwinBoot |
| `out/persistent/piano-uefi-mac-built.fd` | 3145728 | b835ccd19a2476c64d4bbec5eba7165e2e0c08ca3e07e11bfcc1d0c803deed5a | superseded inner-only build, no patched UniXNU or DarwinBoot |
| `out/persistent-v6-macfd/boot.img` | 100663296 | 946adfe02aa166e8c76e59bbf60a76c8e41396c980bb04a5aeb7b5c8d9c06ac3 | v5 container with the Mac FD, AVB signed; not yet run |

See [Mac FD build](../docs/results-2026-11-25-mac-fd-build.md).

## acceptNewMedia probe RAM image (2026-11-25)

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `out/k27-acceptprobe/piano-ufs265.kc` | 147095552 | 05d8ccb53c61c7185f11f025fb8f698eccc342a50a455ccd1d40644077e1c89d | ufs264 KC with 0xA0-0xA6 probes in `acceptNewMedia`; 0xA4 replays through a trampoline; 0xA7/0xA8 hold the instantiate and attach return values |
| `out/k27-acceptprobe/boot-piano-ufs265-probe.img` | 185733120 | 86b534f29c528058a85e4ae8660be07f860cfe1ce12d9ab62efac6f683cea5f7 | v2 RAM image carrying that KC, boot id `6d766e67`; `fastboot boot` only, not run yet |

See [the probe result](../docs/results-2026-11-25-acceptnewmedia-probe.md).
## startMatching probe RAM image (2026-11-25)

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `out/k27-smprobe/piano-ufs266.kc` | 147095552 | 9f276dff4a35ef6ed89ffd2c8a535d3742ed4f843623b744f310c8fa68aca64b | ufs264 KC with 0xB0-0xB7 probes across `IOService::startMatching`: entry, before/after `doServiceMatch`, loop condition, before/after `thread_block`, loop exit and final return |
| `out/k27-smprobe/boot-piano-ufs266-smprobe.img` | 185733120 | a56f132dc2f3f5e89bbd09f90252c6e630d6f977427a2f9e16267830119e43fe | v2 RAM image carrying that KC, boot id `a79d3a17`; `fastboot boot` only, not run yet |

See [the probe result](../docs/results-2026-11-25-startmatching-probe.md). The
runner is `tooling/native/run-ufs266-smprobe.ps1`.

## Upstream root-publication driver built from source (2026-11-25)

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `out/native-kexts/NativeUfs-upstream/NativeUfs.o` | 190208 | e595a1433cea088265be16cd2b309fe7833f82f4971ca5ecad153acc11ee839c | `package/NativeUfs/NativeUfs.cpp` (the upstream publication sequence) compiled by the retained k27 recipe in WSL UbuntuPiano |
| `out/upstream-pub-20261125/piano-ufs269.kc` | 135806976 | dafd48f127783c3348439457a76f7bb5a2daa1c3d298a49a398391ebf9426ab1 | macOS 27.3 kernel collection: the Apple base and the 25 other native drivers of the last booted source-built collection, plus that driver |
| `out/upstream-pub-20261125/boot-piano-ufs269.img` | 185733120 | c6fc5b7aafff87b06341a06f065aa013c9cee441bd905175ed6d04a2e8f32ec5 | v2 RAM image carrying that collection (boot id `a870ecc5…`, ramdisk round-trip hash equals the collection); `fastboot boot` only, **built, to be run**: the tablet's ABL download path still refuses every RAM image |

Built by `tooling/native/build-ufs269-upstream-pub.sh`; run it with `tooling/native/run-ufs269pub.ps1`.
See [the build result](../docs/results-2026-11-25-upstream-publication-build.md).

## Combined boot_a with the pre-UEFI selector (2026-11-26)

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `out/selector/selector.bin` | 10912 | b4130d111f0b001167fa3a084795fcdc5f54b20e2853d5fd5b2c14e082f99ecd | pre-UEFI boot target selector (firmware/selector), the Android-boot + UEFI chain's first stage |
| `out/switch/boot-a-combined-20261126.img` | 40853504 | ad24e08d025982a64e4e5a73cd1d8b158ce8cc7eaaceeb0129cc691aa24f46e8 | unsigned combined boot_a: stock 3.0.309 Android boot + selector at 0x23c0000 + macOS UEFI payload at 0x23d0000 (pack-selector.py) |
| `out/switch/boot-a-combined-20261126-signed.img` | 100663296 | 375478f6c64110a952d44ca57b7c166f3c391729aafef351351a148390d8bed5 | the combined boot_a signed with this host's AVB key (piano-boot-v4-local.pem), stock 3.0.309 props and rollback 1788220800 from the .avb.json beside it |
| `out/switch/boot-a-probe-android-20261126.img` | 40853504 | 84cc4af63743aa00d2b3dc590cd81353be0018f45089f73babe5968f26a9d56a | unsigned probe variant: the selector forced to hand off to Android, to prove the selector runs before either payload |
| `out/switch/boot-a-probe-android-20261126-signed.img` | 100663296 | 8e8669ba44df25460d5f9222ddc758bc2aa3abd72792a7afe7934a7d95fbf859 | the probe variant signed the same way |

`tooling/switch/pack-selector.py` packs them, `tooling/bootimg/sign-on-remote.sh` with `SIGN_HOST=local` signs them, and the host
tests in `tooling/switch/run-tests.sh` pass on this host (19,705 boot-flag codec checks, the selector binary checks, the packer
against the unit's own stock boot image, 14 `bootflag.sh` checks, the FD patch check).

Two cautions carried with these files. The host key's AVB SHA-1 digest is `aae7ce67...` while the device's stock boot_a
carries `f5151930...`, so these images verify under `--check-signed` but are not accepted by Android's dm-verity chain on a
persistent write: they are `fastboot boot` candidates only until the stock key is in reach. And the probe selector is a
thought-proof build only: with its forced Android handoff the compiler removes the PSCI and `dc civac` sequences, so two of
the selector's binary checks fail on purpose. It is never a candidate for a persistent write; the selector of record is the
`b4130d11...` binary above.

Both were run on the tablet as `fastboot boot`, and neither wrote to disk. `logs/selector-20261126-022452-…`: the probe image went
out in 3.3 s and a freshly booted HyperOS `OS3.0.309.0.WPYCNXM` reported `boot_completed=1` 11 s later, so the selector really does
run ahead of both payloads. `logs/selector-20261126-024000-…`: the combined image reached the macOS side and enumerated as the
`piano-uefi` fastboot device 6.1 s after the boot command, which is the fail-safe of a macOS boot set with no SDAM request (this
unit's flag is still the factory empty value). ABL's preflight read right in both runs (`product piano`, `current-slot a`,
`successful yes`, `unbootable no`, `retry-count 6`) and `tooling/native/ensure-hyperos.ps1` brought the tablet back to completed
HyperOS with the slot state untouched. One shape rule came out of the second run: the UEFI load of a combined image has to be the
persistent one (`out/persistent/boot-piano-macos-persistent-v4-avb.img`), because `pack-selector.py` hands the UEFI payload only the
FDT, so a RAM image that keeps its macOS payload inside its own Android ramdisk is invisible to the selector. See
[the run result](../docs/results-2026-11-26-selector-first-runs.md).

## 2026-11-26 - the k27 native kernel built on this host, and the inputs it needed

Built on the desk Mac (macOS 27.2.1 `26B131`, Xcode 27.2 `27B52`, Homebrew prefix `/opt/homebrew`) from
`tooling/ci/k27-kernel.sh` at upstream `d70c16ce`, with `K27_DIR=~/Codex/k27-tree` (public k27 tree, tree id
`544f9468b660...`), `LD_CLASSIC=~/bin/ld-classic` (ld64-957.1 from Apple's public Command Line Tools 26.6 dmg) and
`KDK_DMG` from dortania/KdkSupportPkg `26C5035r`. Evidence and the provenance check:
[results-2026-11-26-mac-build-host-unblocked.md](../docs/results-2026-11-26-mac-build-host-unblocked.md). Status of the
kernel: **BUILT_ON_SECOND_HOST, NOT YET BOOTED ON THE TABLET**. It is an input to the KC, not a boot image.

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `out/kernel-k27-native.macho` (clean build) | 27557496 | 12d3088acce0b2c375066d94dd60264766c9d6cb978f5cd03e062a1c81dcc3be | k27 native variant, xnu-12377.121.6 + series 0001-0066, plain kernel Mach-O, `__thread_starts` present |
| `out/kernel-k27-native.macho` (earlier second pass of the same day) | 27557496 | c896e2032bd35218fc57dba0081cd2db575244c64d35978d656864768ae7552f | same sources; differs only because the linker generates a fresh `LC_UUID` per link |
| `out/x12377/fakeroot/usr/local/lib/kernel/libfirehose_kernel.a` | - | a6ad6d76591a20730cb8d748b362a08a8d6d8228076b648594deba3c0169042f | built from libdispatch `701f4d1a` (`src/firehose/firehose_buffer.c`), arm64e, the archive the kernel links |
| `Kernel_Debug_Kit_27.2_build_26C5035r.dmg` (not stored; fetched from the public KdkSupportPkg release) | 1258400759 | 529ba51a476e1f22c34bda3b3eff357b70bf19a6869d4542c66201cbf2eaa8ff | the KDK whose headers `stage_kdk` stages; only headers are kept, nothing of it is linked |

`K27_TREE_TOP=0066` is the configured baseline series; 0067-0075 are applied to the 27.3 clone by
`tooling/k273/prepare-k27-tree.sh`, so `out/k27/kernel-k27-0066-native.macho` is the name that tool measures.


## 2026-11-26 - the root images this unit can be written with, and the ESP each one fits

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `out/inputs/k273/root/root.img` (the desk Mac) | 20936916992 | e682bd1f9b9c24744b535743cb7f1278be41c310640e744288057826f5e2165b | the 27.3 line's root image (SHNGROOT manifest + the unchanged 26C5035r OS container + our SystemOS window). Its recorded `root.json` write plan is the **reference unit's** (`linux_data` at 71789568) and must not be followed here |
| `out/piano-stock-root-26C5035r.img` (this machine, 2026-11-22) | 17547919360 | db3629e5da012ce76e64c0f42739ada9541ed8cca5adaab32256fc9b64b34ef7 | the stock-window SHNGROOT v2 root (4 windows, self-declared size equal to the file), verified block by block against the archived public anchors on 2026-11-21; the one that fits this unit's `linux_rootfs` without eating its tail reserve |
| `out/persistent/esp-device.img` (installed 2026-11-24 23:23) | 1073741824 | 68c28df15bcf55a589441df37e9c5fd94614e2771c437e955f190e3eb0aab898 | the installed ESP. Its menu is now the authority on the layout: default entry `macos`, `native-ufs-root-part=linux_rootfs`, `data-store-part=linux_data`, `data-store-lba=117735999`, `data-persist=2`. Its collection carries a retained kernel (`Darwin Kernel Version 25.5.0 ... root:ios-obj`), **not** one built here |

The write plan for this unit is generated, not copied: `tooling/persist/unit_profile.py --lun0 HEAD --lun4 HEAD --image-bytes
17547919360 --esp out/persistent/esp-device.img --json out/unit-profile/piano-7ccdd855-root-on-rootfs.json` -> root at
`linux_rootfs` first block 102269696, 4284160 blocks, last 106553855, 958720 blocks free, tail reserve untouched, Data store on
`linux_data` untouched. See [results-2026-11-26-root-image-and-esp-line.md](../docs/results-2026-11-26-root-image-and-esp-line.md).
## 2026-11-26 - the verified-root RAM images (27.3, this unit)

| File | Bytes | SHA-256 | What |
|---|---|---|---|
| `out/ram-verified/boot-piano-macos-ram-A-bundled.img` (Mac), `out/k273-ram-verified/boot-...-A-bundled.img` | 160735232 | 83109f8c07054611f43fc52250c719264fa09a860858b3af6a867e361 | first verified-root assembly: the policies linked as a pre-assembled object. Ran: dies `Kernel data abort` at `NativeUfs: stage policy selection` (the `.quad` pointers arrive as plain rebases). Do not reuse |
| `out/ram-verified/boot-piano-macos-ram-A-bundled.img` (Mac), `out/k273-ram-verified2/boot-piano-macos-ram-A-bundled.img` | 160735232 | 2ffa854c6a441e468b30642cc14347286b32c5ac327d6ea082b8b02ba973ef0c | the same image with the policies compiled from `out/root-policy-src/root-policies.cpp` (`make-root-policy-source.py`). Ran 2026-11-26 20:12: both windows published from signed policies, root mounted, `init (pid 1)` up, SystemOS cryptex mounted; panics `offline scratch Data mount failed: 2` (no Data store on this unit) |
| `out/ram-verified/boot-piano-uefi-ram-A-bundled.img` | 11837440 | eb570025859ee90f3b3fb96d3b41414ceda5bf34747a9e94a1ad6dca57e1d128 | the M1 half of both images above (unchanged between them) |

Evidence: `logs/native-runs/ram-k273-verified2-20261126-201206/` (run.log + the 7 MiB pstore dump), decoded with
`tooling/native/xnu-log.py`. Read [results-2026-11-26-k273-verified-root-owner-cache.md](../docs/results-2026-11-26-k273-verified-root-owner-cache.md).
