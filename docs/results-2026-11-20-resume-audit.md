# Resumption audit (2026-11-20)

This was a host-side, read-only continuation audit. No tablet command, backup, reboot, Fastboot operation, or write was performed.

## Source and host state

- Direct Git remote checks still report the requested device repository `main` at `2438556c77c90c22c7b76e57eb8b40c44b35f1af`. The Ubuntu switch-tool repository reports `main` at `aa4095a02c8455dfabc3dc42a8bacdb11bacab6b` and tag `v1.6.1` at `6ef7516b940e33b3384a7469783c2d0ed24ba3c8`; this matches the source and release already recorded in the manifest.
- `ssh mac` works for read-only inspection. The `mac-lan` alias currently fails name resolution. The existing Mac root build directory remains at `/tmp/piano-root-26C5035r.593r2m`; `root.unshimmed.img` still hashes to `e543c4ed25f77dec07c39b3ba840a0a6887a8a499a1d515eecfb9a0bc6099f2b`, and `systemos.unshimmed.img` to `c970de1b385486b2893c7a03a236ad612fb2f89fc45e48e9a7a80374d247487d`. No matching build or VM process was found.
- The root recipe requires `overlay/systemos-shims/sbin/mount` and `fsck`. A bounded read-only search of the retained WSL native directories, historical Windows workspaces, and Mac Codex checkout found no such files. The available DroidVM tree documents that its mount shim was removed and it keeps Apple's original `fsck`; it is a different VM layout and does not supply the required native shims.

## Historical deployment evidence

- The preserved `piano-macos-c146` results report a native macOS 27.3 desktop and persistent Data on this tablet. Its source lock differs from the current plan, and its documented successful desktop is not evidence for the current 3.0.309 storage layout.
- Its `persistent-boot.md` documents a computer-side boot-image exchange to Android and back. The available dated result records through 2026-11-14 do not record that exchange being performed or an Android return. The path from that historical installed macOS layout to today's stock Android layout remains unknown.
- `ARTIFACTS.md` records a kernel SHA-256 for the exact historical k27 pin, but the kernel binary is absent from the preserved output tree. The `piano-macos-latest` sibling has RAM images and generated inputs, but no persistent root or ESP; those assets are not a matched deployment set.

## Historical KC provenance check

- Read-only SHA-256 checks in `E:\codex\2026-11-01\ni\work\piano-macos-latest\out\k27` confirmed eight retained KC files. `kc-k273-native.bin` and `kc-k273-repro-all.bin` are byte-identical by hash (`663e6ef7f06666829b5d8a0770ea77c18923e1e534ad2b2e5ef2f18d1508459e`). The compact/repro variants have separate hashes and are not substitutes for the locked kernel.
- Six historical RAM build manifests (`ram-native`, `ram-native-diagnostic`, `ram-native-diagnostic2`, and `ram-native-fd3d40` through `v3`) record that exact native-labelled KC hash in their `kc_sha256` field. Each is explicitly `OFFLINE_CANDIDATE_NOT_DEVICE_TESTED`; their records say `data_persist=0`, `same_seed=false`, `installed_seed=null` / unknown, and zero firmware entries. The candidate image is therefore not a matched persistent Data or firmware set, and the manifest hashes do not establish the KC's source or signing provenance; they provide no device-boot evidence.
- The sibling checkout is at `9097efb936692ef743c3c947e3682adade97d87d`, has historical dependency lock hashes different from the current baseline, and is dirty with unrelated local changes. It contains no k27 source tree. The KC binary itself has no verified provenance tying it to current pin `11ac316b789d81fc9f734e15645456f536c05141` or prepared XNU tree `de16623d957cccff6dfb78d8eca4a79842af9702`. Do not reuse it for the current build or boot.
- Compact KC experiments are retained as historical research artifacts only. Their differing hashes, absence of a matching source/build record, and lack of device qualification leave their compatibility unknown. No historical binary was copied or modified during this audit.

## Related iPadOS evidence and transfer limits

- The preserved local status record `E:\codex\2026-11-01\ni\PIANO-IPADOS-CURRENT-STATUS-20261114.md` identifies the public same-device iPadOS home-screen report and screenshot `E:\codex\2026-11-01\ni\work\upstream-research\piano-home-screen.jpg`. The public README change is commit `2f4bfa676274d8556d8de5ee8df6408cda081f69` in the same repository, preceding its current `main` `d4f160c8ee60c11b293b5425af375846fcd438ff`. It reports iPadOS 17.7.11 reaching launchd/home screen, NT36532 touch, and Adreno 830 Metal drawing app icons at about 25 fps. This supports prior display/input/GPU hardware progress on this model; it is not macOS or current-build evidence.
- The referenced device repository is `xiaomi-pad-8-pro-development/android_device_xiaomi_piano-unixnu` at `d4f160c8ee60c11b293b5425af375846fcd438ff`. Its macOS 27.3 profile uses a separate UniXNU core checkout and a separate `macos/unixnu.lock`; that lock starts from XNU `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`, not the current local native k27 pin. The repository's macOS README says it is not booting and its kernel does not link, listing missing SM8750 UFS storage, mac17j collection bindings, display and board services.
- The iPadOS desktop report is therefore useful for validating individual hardware bring-up assumptions and for comparison of payload transport. It does not recover local native k27 pin `11ac316b789d81fc9f734e15645456f536c05141`, validate the 26C5035r macOS KC, or establish a macOS boot path. Do not merge its iPadOS source, kernel artifacts, seed, or pins into this deployment set.

## WSL XNU tree review

- Read-only checks of `/root/piano-native/xnu-k27-final` and `/root/piano-native/xnu-k27-patched` found detached HEAD `c2936efdf3724e3268ee812e6712d9d3b8d5a403` (2026-11-15), not prepared tree `de16623d957cccff6dfb78d8eca4a79842af9702`. The prepared-tree object is absent from both Git object databases. In `xnu-k27-final`, base commit `ac9718fb1af618d5ce8678d0dc6e8a58f252216f` is also absent, the working diff changes 60 files (+3898/-129), and untracked files include probe backups and Piano-specific headers/sources. It is not a clean, source-traceable checkout.
- `/root/piano-native/xnu-k273-work` is a separate tree at `d18680c1784616b7bc0b2ca5db70886eee3e3c83`; it also lacks the prepared-tree object and has 166 modified tracked files. None of these working trees was changed or built during this review. Keep them as historical forensic material; do not use them as the locked native source or cherry-pick their uncommitted state.

## Mac host checkpoint re-read

- The preserved independent driver checkpoint contains 183 verified arm64e objects across 25 of 26 fully built drivers. The display driver has a separate two-object ABI probe whose declarations/vtables were checked against the retained 26A434 reference. A subsequent host-only collection probe linked all 26 configured descriptors (including those provisional display objects), serialized 364,799 chained pointers, and passed the pinned DarwinBoot parser after a corrected relink preserved the source `__LINKINFO` bytes. Its artifact hash is `db13a30673c6431f730a10f1a98118f1b74ccff31d4d1fca508f743678d05c2f`.
- This is host-side packaging/parser evidence from the independent frozen build, not proof of equivalence to XNU pin `11ac316b`, device loading, display operation, or startup. The root/SystemOS images on the Mac remain unshimmed; no matching Data image has been produced. No build, VM, Fastboot process, tablet command, or backup ran in this re-read.

## Result and next gate

Historical native macOS 27.3 success confirms that macOS once reached a desktop on this hardware, but it does not recover the locked `11ac316b789d81fc9f734e15645456f536c05141` source tree or a reproducible image set. The newly cross-checked KC is attached to offline-only RAM candidates with mismatched/unavailable seed state and no firmware entries; it does not close that source gate. The separate iPadOS desktop result helps bound hardware feasibility but does not qualify macOS. Do not boot the current unshimmed root or historical candidates. Continue host-side work from the frozen 26C5035r inputs; the next image gate is to obtain or recover the native shim source and the matching Data seed/trust inputs. Storage migration and paired Android/macOS recovery remain separate device-write gates.
