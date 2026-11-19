# Native macOS 27.3 Reproducibility Audit

Audit date: 2026-11-19. Execution baseline: `lenzhang/UniXNU-xiaomipad8pro` commit `2438556c77c90c22c7b76e57eb8b40c44b35f1af`, targeting macOS 27.3 build `26C5035r`.

## Upstream comparison

GitHub REST API recheck on 2026-11-19 returned `2438556c77c90c22c7b76e57eb8b40c44b35f1af` as the latest upstream `main` commit; it is identical to the requested source baseline. The execution fork `xifan-xifan/UniXNU-xiaomipad8pro` is now at `9a0604d8638a1400c6cbbf2facbe23281512a86c`, a documentation-only commit on top of that source baseline; the other public fork is older. The upstream releases API returned no releases. Issue #1 remains open requesting second-device RAM validation, and PR #2 remains open as a staged macOS packaging proposal. No independent second-device success report was found in those repository records.

The baseline already contains commits for the single-frame video output teardown, passive video timeout diagnostics, Metal frontend staging, and reversible UID 501 `passd` policy. These do not establish continuous hardware decode, Safari qualification, or a successful clean restart; maintainer reports remain separate from this device's results.

## Locked source set

The source commit and all dependency pins remain unchanged. The Windows working-tree SHA-256 values are `unixnu.lock=ced429fc4a1e44be80076cc56f2c740e37265f6cfee3bcd268ccbacfc1d55a7` and `feeds.toml=4cd179d4bc2bb1810201ebec50653f41b4831dc511e823703fe0f122c8bb0385`. Canonical Git/LF byte streams hash to `unixnu.lock=7df01ca8fd9cbdbaa8864f0eb906ddbd6d107f628a26fa95540a2090164542c8` and `feeds.toml=fbc246637fe07cf61a620c1339a1f7859535c6f81841d07c5ed4046bf0633a83`; the apparent historical difference is line-ending normalization, not a dependency-pin change. The complete set is the contents of those two files at the execution baseline. Key pins include:

- UniXNU core: `d830d23b0d19c9322d906c21afeca1d6c54aeffe`.
- SM8750 source: `feaa135df4763e3c19a155402e6b17b124a12e13`.
- piano UEFI: `31545f9392d5f5c2acae14cec2fa3a0aa1de51d4`.
- DarwinBoot: `8544957e04bdc43e2aac9243764ac16979f15c42`.
- Mu-Silicium: `e51cd877e31c8fbb322032dc4cb6715e83d25ad3`.
- Local k27 tree pin: `11ac316b789d81fc9f734e15645456f536c05141`; its recorded XNU base is `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`, with a locally prepared tree at `de16623d957cccff6dfb78d8eca4a79842af9702`.

No dependency pin was changed. `tooling/k273/prepare-k27-tree.sh` consumes an existing `PIANO_K27_TREE`, applies the repository patches, and relinks it; it does not provide a zero-input public reconstruction of the local k27 tree.

The WSL host-side persistence test was rerun with GNU mtools 4.0.50, downloaded from ftp.gnu.org, verified with the signed release archive, and built locally. Repeated creation of the same 1 GiB FAT32 image is byte-identical with 4.0.50; the installed 4.0.43 produced differing hashes. Remaining test failures are host-fixture issues: a fake sysfs partition-size mismatch, Linux lacking macOS `fcntl.F_NOCACHE`, and a test continuation after an expected backup stop. No tablet I/O occurred.

## Input status

| Input | Status on this workstation / configured Mac |
|---|---|
| Device source at the requested commit | Present in this workspace; clean tracked tree at the baseline. `AGENTS.md` is an existing untracked instruction file and is not part of this change. |
| Pinned source checkouts / `unixnu-research` | Not present in the current workspace or in the configured Mac's documented project location. |
| Exact local k27 tree, generated objects/headers, KC build inputs | Missing. The old research workspace contains unrelated kernel/ramdisk outputs only; excluded. |
| macOS 27.3 / `26C5035r` Apple IPSW-derived inputs | Not present under this workspace's `out/inputs`; no compatible cached payload was identified. |
| Device DTB/firmware and per-unit calibration | Repository has the source-side configuration; no complete per-unit, provenance-checked build input set has been assembled. |
| Native kext and Metal/userland build outputs | No current 27.3 candidate outputs or matching manifest found. |
| Data seed, trust cache and root image | No matched 27.3 set in the current `out/` tree; `out/` is absent. |
| Signing chain / boot profile | Not verified for this device/build. The upstream persistent flow references an owner-specific signing setup; its private key is not an input we will request or copy. |
| Current 3.0.309 Android boot triplet and verified rollback package | No complete recovery package is available. A later interrupted `/data` collection left two identical 4,202,692,608-byte partial tar files on E: and F:; they are not backups and must not be resumed or used for recovery. The user has directed that no tablet backup be made. |

The configured Mac is Apple silicon running macOS 15.3 with about 198 GiB free. It has no project checkout and no globally selected developer directory. The Command Line Tools package is installed; `DEVELOPER_DIR=/Library/Developer/CommandLineTools` resolves Apple clang 17, `ld`, and `codesign`. The host has Python 3.9.6, while `docs/host-checklist.md` requires Python 3.11 or newer; `nasm`, `iasl`, and Homebrew are absent, and `xcodebuild` is unavailable without full Xcode. The host toolchain is therefore incomplete. No build was started.

The repository's committed artifact catalog describes older 2026-11-10 slice-1 outputs and inputs. Those files are not a matched 27.3 device candidate and are not reused. No build, image, or device test is claimed by this audit.

## Historical local evidence

The preserved `E:\codex\2026-11-01\ni\work\piano-macos-c146` checkout is commit `c146dfc6ce1a24c9b85df9b70003d6ecbe9fba63` (2026-11-14 17:19 UTC) with tracked edits and no `out/` tree. Its README and dated results report a native macOS 27.3 (`26C5035r`) desktop and earlier tests on the tablet, including GPU, input, Wi-Fi, and persistent Data. This is material historical evidence, but it is not a current-device result; the 2026-11-19 live GPT audit finds no native partitions. The intervening transition is not established. Its Git object database does not contain the requested baseline commit `2438556c77c90c22c7b76e57eb8b40c44b35f1af`, and its `unixnu.lock` / `feeds.toml` hashes (`7df01ca8…` / `fbc24663…`) differ from the current lock (`ced429fc…` / `4cd179d4…`). Treat the old deployment as evidence from a distinct, unproven source set; do not reuse its build outputs or dependency state.

A separate preserved sibling checkout, `E:\codex\2026-11-01\ni\work\piano-macos-latest` at `9097efb936692ef743c3c947e3682adade97d87d` (2026-11-14 14:18 +0800), contains 6,249,169,588 bytes across 114 files under `out/k27` and four small generated files under `out/inputs`. Its `unixnu.lock` / `feeds.toml` hashes match the c146 historical set, not the execution baseline. It has no `out/k27/xnu` source tree, persistent root image, or ESP image; the Mac also has no `PIANO_K27_TREE` configuration. The 151,158,784-byte `kc-k273-native.bin` matches that old build tree's hash, but is not a source-reproducible artifact for the current baseline.

The checked `ram-native-fd3d40-v3` image is 177,512,448 bytes and matches its adjacent JSON SHA-256 (`1040b921…`). That JSON marks it `OFFLINE_CANDIDATE_NOT_DEVICE_TESTED`, `data-persist=0`, `same_seed=false`, and its installed seed source as unknown. The old artifact catalog also says its signing profile uses the tablet's OS3.0.303 fingerprint. These artifacts are valuable historical diagnostics, not a matched 3.0.309 deployment or rollback set; none were booted or modified in this task.

The wider historical workspace also contains `E:\codex\2026-11-01\ni\work\xnu-ac9718-full`, a source snapshot outside a Git checkout. GitHub identifies `ac9718fb1af618d5ce8678d0dc6e8a58f252216f` as Apple XNU `xnu-12377.121.6`; the local Git blob IDs for `bsd/kern/kern_exec.c` and `osfmk/kern/startup.c` match that commit. This establishes a verified base-source candidate, not the locked k27 tree: the snapshot has no Git metadata or prepared build state, and the separate `xnu-ac9718-src` directory is incomplete. No local checkout contains k27 pin `11ac316b789d81fc9f734e15645456f536c05141`.

The preserved `piano-macos-latest` checkout has an untracked helper that applies an ordered `Droid-VM/macOS-DroidVM` XNU patch tree before its local piano series. The public repository's `main` was `524d27bf57ae92375aa39e3a81e11727c08973ff` on 2026-11-16 and contains 48 numbered XNU patches plus four additional patch files under `patches/xnu-12377`. All 48 numbered patches from that exact public snapshot now replay in order against the verified Apple `xnu-12377.121.6` (`ac9718f`) source archive in an isolated temporary checkout; the result is clean. The old fixed `9b31c23` archive remains incompatible at patch 0008, whose `kern_exec.c` preimage differs from the current public series. The successful public replay is a VM-source reproducibility result only: it does not recreate the device's private k27 pin `11ac316b789d81fc9f734e15645456f536c05141`, and none of this series has been merged into the device source set. See the manifest for archive hashes and replay fingerprints.

The local `E:\\codex\\2026-11-01\\ni\\work\\external\\macOS-DroidVM` notes contain concrete successful DroidVM runtime reports that can guide feature-level diagnosis: macOS 27.0.1 (`26A434`) reaches automatic login, Dock and home; GPU/Metal desktop rendering and interaction were exercised; on 2026-11-04 Safari/Bilibili playback ran 23 video sessions with counters `in=2131`, `out=2111`, `dropped=20`; and on 2026-11-05 the user confirmed audible playback without stutter and with audio/video synchronization. The notes also preserve failures and remaining work, including audio helper scheduling, microphone access, and GPU/resource limits. These are reported results from Android/Gunyah/crosvm DroidVM, not native Xiaomi Pad UFS boots, not macOS 27.3 (`26C5035r`), and not direct re-verification in this task. Reuse the implementation and diagnostic leads only after comparing their pinned inputs and execution environment with the native target.

The current clone of that checkout has no `out/` directory. Its artifact catalog names historical generated images, but the binaries and the exact local k27 tree are not present there. The valid `F:\piano-backup\data-local-tmp\tmp.tar` listing contains 13 `macos27` entries; its archived `run.sh` launches `crosvm run --hypervisor gunyah` with `os.img` and a Data image. These files are DroidVM/Gunyah guest evidence, not native UFS install inputs. The archive's member list was read without extraction; do not reuse those VM images as native boot artifacts.

The separate 2026-11-16 Android wipe proposal targets HyperOS 3.0.308 and recommends `fastboot -w`; its archived verification log marks the large app/data/media tar files as unrecognized archive formats. It is not authorization or a recovery plan for the current 3.0.309 device. No wipe has been performed in this task, and the paused erase must remain paused.

Read-only follow-up on the existing 52,839,244,759-byte `F:\piano-backup\data-data\data-data.tar` found a 39-byte GNU tar warning before the first header. Skipping only that prefix let Python's streaming tar parser read 4,719 entries (169,597,666 declared payload bytes) to an end marker at about 177 MB; the file continues for about 52.6 GB, with tar diagnostic text after the marker. A scan through the unexplained tail was stopped before traversing tens of gigabytes. No files were extracted or changed. This may be a partial salvage lead, but it does not validate the aggregate archive or provide a complete recovery set.

## Reproduction gate

**Not reproducible yet.** The blocking inputs are the exact local k27 tree and its generated build state, matched Apple `26C5035r` payloads, complete firmware/calibration and userland inputs, a verified signing/boot profile, and the missing Mac host tools. First resolve these by inventorying existing user assets and checking public preparation steps/branches/forks; do not download duplicates or mix newer pins into this locked set.
