# CI and releases

GitHub Actions on GitHub-hosted runners only (no self-hosted runner). Four workflows:

| Workflow | When | Runner | What |
| --- | --- | --- | --- |
| `checks.yml` | every pull request, every push to `main`, by hand | ubuntu-24.04, macos-26 | privacy scan, pinned sources, self-tests |
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

- **privacy**: `scripts/ci/privacy-scan.py` on the lines a change adds and on its commit messages (`--diff BASE`), then on the whole
  tree. Pattern classes only, no private literals: a user's home path, the maintainer's private notes folder (added lines only),
  RFC 1918 addresses, local host names, MAC addresses, e-mail addresses other than no-reply ones (author headers such as a patch's
  `From:` and `Co-authored-by:` are fine), private keys, access tokens, inline secrets, a password piped to `sudo -S`, and file types
  that would be Apple payloads, encrypted blobs or build output. A meant hit carries `privacy-scan: ok` on its line or an entry in
  `scripts/ci/privacy-scan.allow`. `scripts/ci/test-privacy-scan.py` holds a known positive and negative per class. The
  maintainer's own pre-publish audit (with its literal list) is separate and stays private.
- **pins**: `scripts/fetch-pins.py --verify` fetches every `unixnu.lock` pin and submodule pin by its commit (depth 1, no blobs), and
  `tooling/k27/fetch-k27-tree.sh` rebuilds the k27 tree from upstream plus `patches/k27-tree` and checks its git tree id.
- **self-tests** (macos-26: the C/C++ host tests use Apple clang): `scripts/ci/self-tests.py` runs every Python, shell and C/C++ host
  test of `package/`, `tooling/`, `firmware/` and `scripts/` that needs no Apple content, no `out/` and no tablet, with an empty
  `HOME` and no `PIANO_*` variables. `scripts/ci/self-tests.skip` names each one it does not run and why (a checker that takes a
  built file, a test that needs build output or a local backup, a header build line that names generated inputs, or `KNOWN-FAIL`:
  failing on `main` today, an open item). A JUnit report is the only upload.

Locally: `python3 scripts/ci/privacy-scan.py [--diff BASE]`, `python3 scripts/ci/self-tests.py [--list] [--only SUBSTR]`,
`python3 scripts/fetch-pins.py --verify`.

## The k27 tree

`unixnu.lock` `[pin."k27"]` pins upstream [Droid-VM/macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) `abcbc18`; our delta
is `patches/k27-tree` (two patches, provenance and what was left out: [patches/k27-tree/README.md](../patches/k27-tree/README.md)).
`tooling/k27/fetch-k27-tree.sh DEST` makes the tree; `scripts/fetch-pins.py` does it for `k27` (as `k27-tree`). Locally nothing
changes: `PIANO_K27_TREE` still names the tree the build reads.

## build.yml

**fd**: `scripts/fetch-pins.py --only` (the UEFI pins) into `$RUNNER_TEMP/unixnu-research`, then `tooling/firmware/build-fd.sh`;
uploads `fd.sha256` and the build log.

**kernel-kexts** (`tooling/ci/k27-kernel.sh`, `tooling/ci/build-kexts.sh`):

| Stage | From |
| --- | --- |
| tree | upstream + `patches/k27-tree` |
| xnu | apple-oss-distributions/xnu `xnu-12377.121.6` + the tree's `patches/xnu-12377` 0001-0054 + `patches/k27` 0055-0066 (`git am`) |
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
6. **KNOWN-FAIL self-tests** (`scripts/ci/self-tests.skip`): four tests fail on `main` and are skipped until fixed.
