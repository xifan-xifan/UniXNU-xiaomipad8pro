# CI and releases

GitHub Actions on GitHub-hosted runners only (no self-hosted runner). The draft release uses the built-in job token; the only optional
secret is `PIANO_KDK_AUTH` for the KDK (open item 1 below).
The official actions run on Node.js 24 (`checkout@v7`, `upload-artifact@v7`, `download-artifact@v8`).

| Workflow | When | Runner | What |
| --- | --- | --- | --- |
| [`checks.yml`](../.github/workflows/checks.yml) | every pull request, every push to `main`, by hand | ubuntu-24.04, macos-26 | privacy scan, pinned sources, self-tests |
| `dependencies.yml` | monthly (1st, 05:23 UTC), by hand | ubuntu-24.04, macos-26 | pins still fetch, pins behind upstream (report), Apple OSS pins, the IPSW index, the runner's toolchain |
| `build.yml` | by hand, or called by `release.yml` | macos-26 | the FD (hash only), the k27 native kernel, our kexts |
| `release.yml` | a pushed `v*` tag, or by hand with a tag | ubuntu-24.04 + `build.yml` | a **draft** release of our binaries, recipes, SHA-256 manifests and the install skill |

`.github/dependabot.yml` adds monthly update pull requests for the actions themselves.

## What never leaves a runner

- **Apple content.** The only IPSW member CI fetches is the kernelcache (`kernelcache.release.mac17j`, 31 MiB, by HTTP range
  requests from Apple's CDN, `tooling/ci/with-kernelcache.sh`). It lives under `$RUNNER_TEMP`, is used to relink the kernel and
  build the kexts, and is deleted when that command ends. The Kernel Debug Kit's headers (see the open items) are deleted at the end
  of the job. Neither is in an `actions/cache` or an `actions/upload-artifact` path, and nothing is ever uploaded encrypted.
  `IOGPU.tbd` and the other text stubs stay in the repository.
- **Build output of the k27 tree.** Nothing under its `out/` (the KC base, the KC-derived headers, object trees) is uploaded; the
  artifacts hold our kernels, our kext objects and SHA-256 manifests (`build.yml` checks the file list before the upload).
- **The FD.** It bundles third-party DXEs from piano-uefi's `Binaries/piano`, so only its SHA-256 is uploaded and released.

## checks.yml

The workflow uses GitHub-hosted runners with read-only contents permission and cancels superseded runs on the same ref.
[Checks workflow](../.github/workflows/checks.yml)

- **Privacy, `ubuntu-24.04`, 10 min.** The job checks out full history and runs the scanner's self-test. With a valid PR base or
  push-before commit, `--diff BASE` scans added lines, commit messages and added or modified file types. The whole-tree scan runs
  regardless of base availability on a successful job. The scanner checks for private paths, network identifiers, personal e-mail,
  credentials and disallowed payload or build-output file types. Its exceptions are in the allowlist or marked `privacy-scan: ok`.
  [Scanner](../scripts/ci/privacy-scan.py) · [Scanner tests](../scripts/ci/test-privacy-scan.py) · [Allowlist](../scripts/ci/privacy-scan.allow)
- **Pinned sources, `ubuntu-24.04`, 20 min.** `scripts/fetch-pins.py --verify` fetches each `unixnu.lock` pin and submodule pin
  by commit. `tooling/k27/fetch-k27-tree.sh` then reconstructs the k27 source tree from upstream and `patches/k27-tree` and checks
  its Git tree ID. [Source verifier](../scripts/fetch-pins.py) · [Lockfile](../unixnu.lock) · [k27 tree reconstruction](../tooling/k27/fetch-k27-tree.sh)
- **Host self-tests, `macos-26`, 60 min.** Homebrew installs `mtools` for FAT images, `llvm` for the freestanding selector
  build, and `ffmpeg` for the video host tests. The runner discovers tracked Python, shell and C/C++ host tests under
  `package/`, `tooling/`, `firmware/` and `scripts/`. It gives each command up to 1,500 seconds, an empty `HOME`, and an
  environment filtered of `PIANO_*`, `TURNIP_*`, `K27_*` and `K273_*` variables. The log ends with up to ten slowest commands,
  including compilation time. The `self-tests-report` JUnit upload runs with `if: always()` and 14-day retention.
  [Self-test runner](../scripts/ci/self-tests.py)
- **Real APFS pool capture.** The same macOS job separately runs `bash tooling/pool/test-pool-capture-check.sh --real-apfs`
  after the host suite. It creates and attaches fresh APFS disk images using `hdiutil` and `diskutil`. Test discovery also
  runs the script in its default mode with simulated disk commands and fixture headers. [Pool capture test](../tooling/pool/test-pool-capture-check.sh)

### Host test inventory (2026-11-27)

`python3 scripts/ci/self-tests.py --list` reports **162 runnable entries** (68 Python, 34 shell, 60 C/C++), **49 explicit
skips**, and **97 C/C++ files without a standalone build line** (`none`). The count is of runner entries. Suites can run
multiple tests, and the real APFS step is outside this count. [Self-test runner](../scripts/ci/self-tests.py)

The skip list contains **39 `SUITE`** entries run by another test, **6 `ARGS`** entries that need arguments from a caller, and
**4 `INPUT`** entries that need local inputs. `KNOWN-FAIL`, `HEADER` and `TOOL` each have **0** entries.
[Skip list](../scripts/ci/self-tests.skip)

The self-test job fetches the DarwinBoot pin from `unixnu.lock`. The bootloader host tests apply the repository patches in a
temporary directory and check extra-RAM and parsing against synthetic fixtures. For a local run, set `DARWINBOOT` to a checkout
containing the pin. The entries below run through test discovery.

| Area | Discovered entry points |
| --- | --- |
| Repository integrity | [`test-repo-manifests.py`](../scripts/ci/test-repo-manifests.py) checks manifests, native descriptors and build wiring, patch hygiene, and shell syntax. [`test-doc-links.py`](../scripts/ci/test-doc-links.py) checks tracked Markdown paths and heading anchors, plus driver-index coverage. |
| CI tooling | [`test-privacy-scan.py`](../scripts/ci/test-privacy-scan.py) covers scanner rules and Git diffs. [`test-fetch-pins.py`](../scripts/ci/test-fetch-pins.py) uses local Git fixtures for pin validation, fetch and recovery. [`test-self-tests.py`](../scripts/ci/test-self-tests.py) covers discovery, timeouts, JUnit output and duration reporting. |
| Boot and release | [`test-home-ram-boot.py`](../scripts/ci/test-home-ram-boot.py) checks paths and checksums with simulated fastboot. [`test-verify-ram-images.py`](../tooling/bootimg/test-verify-ram-images.py) exercises the offline image verifier. [`test-partition-safety.py`](../tooling/persist/test-partition-safety.py) covers partition and installed-identity guards with fixtures. [`test-release-assets.py`](../tooling/ci/test-release-assets.py) checks release assembly and refusal cases. |
| Audio and power | [`PianoAudio/tests/run-audio-host.sh`](../package/PianoAudio/tests/run-audio-host.sh), [`PianoPower/tests/run-platform-host.sh`](../package/PianoPower/tests/run-platform-host.sh), and [`test-extractors.py`](../tooling/audio/test-extractors.py). |
| Camera | [`PianoCamera/tests/run-camera-host.sh`](../package/PianoCamera/tests/run-camera-host.sh), [`tooling/camera/extension/tests/run-camera-provider-host.sh`](../tooling/camera/extension/tests/run-camera-provider-host.sh), [`test-camera-parsers.py`](../tooling/camera/test-camera-parsers.py), and [`test-diagnostic-host.py`](../tooling/camera/diagnostic/test-diagnostic-host.py). |
| Graphics, video and display | [`test-metal-host.py`](../tooling/metal/test-metal-host.py), [`PianoVideo/tests/run-bringup-host.sh`](../package/PianoVideo/tests/run-bringup-host.sh), [`PianoDp/tests/run-dp-host.sh`](../package/PianoDp/tests/run-dp-host.sh), [`test-dp-phy-tables.py`](../tooling/dp/test-dp-phy-tables.py), and [`piano-frametime/run-tests.sh`](../tooling/native/piano-frametime/run-tests.sh). |
| IR, brightness and torch | [`PianoIR/tests/run-ir-host.sh`](../package/PianoIR/tests/run-ir-host.sh), [`piano-brightness-sync/tests/run-sync-host.sh`](../tooling/native/piano-brightness-sync/tests/run-sync-host.sh), and [`piano-torch/test-cli.sh`](../tooling/native/piano-torch/test-cli.sh). |

The inventory also covers UFS and durability, DPU readback and scanout, the GOP display and `gop-shot`, input and sensors,
USB, Wi-Fi and Bluetooth, long boot arguments and ADT merge, ESP tooling, and userland staging.

### Adreno830 host suite

The suite fetches the public `ArmSmmu4K`, `AdrenoPM4` and `AdrenoFirmware` kits with `scripts/fetch-pins.py --only`. It checks
each checkout's HEAD against `unixnu.lock` and adds PianoA830's `ArmSmmu4KPaged.c/.h` to the staged SMMU kit. The ArmSmmu4K pin
matches the driver's fixed-pool API. The kits and test binaries share a temporary directory, and the 1,500-second budget
covers the fetch, compile and run with ASan and UBSan.
[Adreno830 CI wrapper](../scripts/ci/adreno830-host.py) · [Lockfile](../unixnu.lock) · [Kit provenance](../package/PianoA830/UPSTREAM)

The suite covers the tier allocator, paged SMMU, PM4 submission, HFI votes and recovery, among other host checks.
It runs the SHA-256 test vectors and synthetic firmware tests. Validating stock gen80000 images locally requires
the stock images and the matching pin file, supplied to the runner with `--firmware` and `--pins`. The suite skips
`test_a830regs`, whose comparison needs the unpublished Mesa register export under `docs/inputs/regs`, and the
kernel-object check, which needs a built UniXNU tree. The standalone register-header checks still run. Two timing tests run
only locally: `test_a830fence` (a sliced wait of at least 40 ms) and `test_p1wire` (a parallel wiring speedup), which a loaded
3-core hosted runner cannot guarantee. [Adreno830 CI wrapper](../scripts/ci/adreno830-host.py)

Locally: `python3 scripts/ci/privacy-scan.py [--diff BASE]`, `python3 scripts/ci/self-tests.py [--list] [--only SUBSTR]`,
`python3 scripts/fetch-pins.py --verify`. To repeat the Adreno830 fetch and suite on a Mac with Xcode:

```sh
SELFTEST_TIMEOUT=1500 python3 scripts/ci/self-tests.py --only package/PianoA830/adreno830/tests/run-host-tests.py
```

## The k27 tree

`unixnu.lock` `[pin."k27"]` pins upstream [Droid-VM/macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) `abcbc18`; our delta
is `patches/k27-tree` (two patches, provenance and what was left out: [patches/k27-tree/README.md](../patches/k27-tree/README.md)).
`tooling/k27/fetch-k27-tree.sh DEST` makes the tree. `scripts/fetch-pins.py` does it for `k27` (as `k27-tree`). Use
`PIANO_K27_TREE` to point the local build at the tree. [Local builds](building.md)

## build.yml

**fd**: `scripts/fetch-pins.py --only` (the UEFI pins) into `$RUNNER_TEMP/unixnu-research`, then `tooling/firmware/build-fd.sh`;
uploads `fd.sha256` and the build log.

**kernel-kexts** (`tooling/ci/k27-kernel.sh`, `tooling/ci/build-kexts.sh`):

| Stage | From |
| --- | --- |
| tree | upstream + `patches/k27-tree` |
| xnu | apple-oss-distributions/xnu `xnu-12377.121.6` + the tree's `patches/xnu-12377` 0001–0054 + `patches/k27` 0055 through `K27_TREE_TOP` (default 0066 in the CI recipes, applied with `git am`) |
| ld | `ld-classic` (ld64; the kernel links with `-threaded_starts_section`) from the runner's toolchain or `LD_CLASSIC` |
| kdk | headers of a Kernel Debug Kit: `vars.PIANO_KDK_URL` (+ `secrets.PIANO_KDK_AUTH`, an HTTP header), open item |
| fakeroot | apple-oss-distributions AvailabilityVersions-157.2, libplatform-375.120.2, libdispatch-1542.100.32 headers; the tree's installhdrs; `libfirehose_kernel.a` built from libdispatch's source |
| platform | ArmDTPlatform and ArmCpu at their `unixnu.lock` pins, compiled with the kernel's own recipes |
| kernel | `build-k27-piano.sh` with `K27_VARIANT=native` |
| kexts | inside `with-kernelcache.sh`: `tooling/k273/prepare-k27-tree.sh` (the 27.3 clone, the kernel relinked at the collection's addresses), then `tooling/k27/build-native-image.sh` with `PIANO_STOP_AFTER=kexts` |

Artifact `kernel-kexts`: `kernel-k27-native.macho`, `kernel-k273-native.macho`, `piano-kexts.tar.gz` (the kexts' `.o` objects and
`tooling/k27/kext` descriptors only), `SHA256SUMS`, `BUILD-INFO.txt` (commit, k27 tree id, xnu source tree id, Xcode). The kernel
collection, the ADT and the RAM images are not built in CI.

### `libfirehose_kernel.a`: built from source

The kernel links `-lfirehose_kernel` from the fakeroot (`build-k27-piano.sh`: `-L$FR/usr/local/lib/kernel`). The local tree's copy
came from UniXNU's bootstrap of the 12377 environment (Droid-VM/UniXNU `wip/droidvm`, `scripts/build-firehose.sh`), which builds the
`libfirehose_kernel` target of Apple's open-source libdispatch (`src/firehose/firehose_buffer.c`, APSL) with `xcodebuild`; the
archive's debug strings name that source file in the bootstrap's `libdispatch-work` checkout (libdispatch-1542.100.32, `701f4d1a`).
It is not an Apple binary download. `k27-kernel.sh fakeroot` builds it the same way: on a desk Mac (Xcode 27.2) the result has the
same object, the same exported and undefined symbols and the same machine code (`otool -tv`) as the local archive.

## release.yml

A release is made only by hand, after the tablet qualification: push a tag `v*` that is on `main` (or run the workflow with the tag).
It never runs on a push to `main`. The `guard` job refuses a tag that is not `v*` or not on `main`; `build.yml` builds the tag;
`tooling/ci/release-assets.sh` adds the recipes (`piano-recipes.tar.gz`: pins, patches, build scripts), the install skill
(`skills/install-piano-macos`, refused when the tag lacks it unless `ALLOW_NO_SKILL=1`), `FD-SHA256.txt`, notes and one
`SHA256SUMS`, checks that the kernels are plain kernel Mach-Os (no fileset collection), and `gh release create --draft`. The
maintainer reviews the draft and publishes it. The root image and the kernel collection are built on the user's side by the install
skill from the user's own IPSW.

## Open items

1. **Kernel Debug Kit (owner decision).** The kernel build includes KDK headers (`KDKROOT`: Kernel.framework, KernelSupport; none of
   the KDK's libraries is linked: BCM2837 is not an SPTM SoC). Apple's KDK download needs an Apple Account, so a hosted runner cannot
   fetch it from Apple anonymously. Choices: an Apple Account session in a secret (`PIANO_KDK_URL` + `PIANO_KDK_AUTH`), a third-party
   mirror (upstream macOS-DroidVM uses dortania/KdkSupportPkg; that is Apple content from someone else, outside the rule "fetch it from
   Apple"), or reduce the build to the few headers it needs and generate them. Until then `build.yml`'s kernel job stops at "kdk".
2. **ld-classic.** Xcode 27 has no `ld-classic`; the local build uses Command Line Tools' ld64-957. Whether the macos-26 image's
   Xcode still ships it is checked monthly (`dependencies.yml`); otherwise `LD_CLASSIC` needs a source (ld64 is open source).
3. **ArmDTPlatform / ArmCpu recipe.** The local libraries were built by the upstream author's unpublished environment; the `platform`
   stage rebuilds them from the pins with the kernel's recipes, which has not been compared with the local archives yet.
4. **Not yet run on a hosted runner**: `build.yml` and `release.yml` as a whole (validated: YAML syntax, the tree, xnu, ld and
   fakeroot stages, the kernelcache fetch and deletion, the release assembly with stand-in binaries, on a desk Mac).
5. **The Metal plugin** (`tooling/metal/build-turnip-a830.sh`) needs the Mesa work tree and the metal2vulkan bridge, neither pinned in
   `unixnu.lock` yet; not in CI.
6. **Adreno830 local checks.** The local wrapper uses the pinned AdrenoFirmware vote reference and stays on the skip list as
   `SUITE` because CI runs the shared suite. The local wrapper also runs the macOS build mode, board checks, timing tests and
   optional stock firmware checks. [Local wrapper](../tooling/k27/run-a830-host-tests.py) · [Skip list](../scripts/ci/self-tests.skip)
