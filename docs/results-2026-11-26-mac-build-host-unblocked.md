# 2026-11-26 - the Mac build host is unblocked: ld-classic, fakeroot, first kernel link

Evidence from this host (MacBookPro, `MacBookPro17,1` M1, macOS 27.2.1 `26B131`, Xcode 27.2 `27B52`),
the combination `skills/install-piano-macos` names as the only tested one. Nothing was written to the tablet;
it stayed in HyperOS (`7ccdd855 device`, `OS3.0.309.0.WPYCNXM`).

## Upstream moved 134 commits: `3365412e` -> `d70c16ce`

`upstream/main` was fetched and the Mac's snapshot `~/Codex/upstream-tip` updated to `d70c16ce`. The Mac has no
GitHub credentials (`git+ssh` is refused and `https` resets at will), so the update travelled as an incremental
`git bundle` from this host instead of a network fetch on the Mac. `docs/STATUS.md` at `d70c16ce` says the owner's
daily build is **v13.1y12m** (installed 2026-11-26): display, touch, keyboard cover, speakers, microphones, cameras,
Metal on the Adreno 830, paged GPU memory, Safari/Chrome GPU and hardware H.264 decode, all **booted from disk**. The
OS switcher row is still "RAM only, read-only: a full switch has not been run", so the Android/macOS switch this
goal has to qualify is not something the upstream setup has proven either.

## The blocker: `ld-classic` (upstream `docs/ci.md` open item 2, gap G1) is a public download

The kernel links with `-threaded_starts_section`, which only ld64 understands. Every `ld` that Xcode 27 `27B52` and
Command Line Tools 27.2 ship is `PROJECT:ld-27106.1` (ld-prime) and refuses the flag; `xcrun -f ld-classic` fails and
no `ld-classic` exists anywhere on this Mac. Upstream records this as local-only input: "the local build uses Command
Line Tools' ld64-957", listed inside gap G1.

The owner downloaded Apple's public `Command_Line_Tools_26.6_Apple_silicon.dmg` (526,845,958 bytes) and extracted
`~/bin/ld-classic`:

```
$ ~/bin/ld-classic -v
@(#)PROGRAM:ld-classic  PROJECT:ld64-957.1
BUILD 16:36:49 Jun  8 2026
```

Verified to be the linker the kernel needs, invoked directly (`-Wl,...` through `clang` will not reach it):

```
$ ~/bin/ld-classic -arch arm64e -static -e _start -threaded_starts_section -o tss.out tss2.o
$ otool -l tss.out | grep -A3 thread_starts
  sectname __thread_starts
   segname __TEXT
      addr 0x0000000100003ffc
      size 0x0000000000000004
```

One correction recorded here so it is not repeated: an earlier note on this host claimed CLT 26.6's `ld-classic`
rejected `-threaded_starts_section`. That run passed the linker as a clang *input file*, so the system ld-prime did
the link and produced the message; the flag itself is accepted (ld64 `Options.cpp:4210`, with warnings only for
non-arm64 or non-`-static`/`-preload` inputs).

## `tooling/ci/k27-kernel.sh tree xnu ld fakeroot` passes on this host (`STAGE-RC=0`)

| Stage | Result |
| --- | --- |
| tree | public `Droid-VM/macOS-DroidVM @ abcbc18f` + `patches/k27-tree`, tree `544f9468b660aabbc73ee24638adabd14c274671` as `unixnu.lock` expects |
| xnu | `xnu-12377.121.6` (`ac9718fb`) + `patches/xnu-12377` 0001-0054 + `patches/k27` 0055-0066; `.piano-series = 0066` |
| ld | `LD_CLASSIC=~/bin/ld-classic` (ld64-957.1) |
| fakeroot | AvailabilityVersions 157.2, libplatform 375.120.2, libdispatch 1542.100.32 headers, the tree's `installhdrs` with `KDKROOT`, and `libfirehose_kernel.a` |

`libfirehose_kernel.a`: 1 object, arm64e, sha256
`a6ad6d76591a20730cb8d748b362a08a8d6d8228076b648594deba3c0169042f` (`-L$FR/usr/local/lib/kernel` is what the kernel
links). `docs/ci.md` says the desk Mac's archive matches the local one object for object; the hash above is this
host's own record of that.

Two host gaps had to be closed first, both plain environment:

- `AvailabilityVersions` installs `availability.pl` through a cmake/Ninja build found with `xcrun --find cmake`.
  `xcrun` falls back to `$PATH`, so `brew install cmake ninja` plus `/opt/homebrew/bin` on `PATH` is enough. Without
  them `stage_fakeroot` dies later with `Unable to locate .../availability.pl`, which looks like a header failure.
- The KDK: `stage_kdk` looks for `*Payload/Library/Developer/KDKs/*.kdk` inside `$KDK_DMG`. The KdkSupportPkg release
  `Kernel_Debug_Kit_27.2_build_26C5035r.dmg` (1,258,400,759 bytes, sha256
  `529ba51a476e1f22c34bda3b3eff357b70bf19a6869d4542c66201cbf2eaa8ff`, equal to the `SHA256SUM` of the same tag) instead
  carries `KernelDebugKit.pkg` whose components expand to `System/Library/{Kernels,KernelCollections,KernelSupport,Extensions}`
  and `System/Library/Frameworks/Kernel.framework/Versions/A/{Headers,PrivateHeaders}`. `kdk-notb` was filled by hand with
  the same rsync include set and the same Tightbeam prune, and `installhdrs` accepted it with `KDKROOT` pointed at it.
  Upstream open item 1 (where a hosted runner gets KDK headers) is a runner problem, not a local-host problem.

## The first kernel link, and the provenance question we have not closed yet

```
kernel-k27-native.macho        27,557,496 bytes, Mach-O 64-bit executable arm64e
sha256  c896e2032bd35218fc57dba0081cd2db575244c64d35978d656864768ae7552f
copied to out/k27/kernel-k27-0066-native.macho (the name tooling/k273/prepare-k27-tree.sh measures)
```

`K27_TREE_TOP=0066` is correct for the configured tree: `patches/k27` now runs to 0075 and `tooling/k273/prepare-k27-tree.sh`
applies the patches past the tree's series (0067-0075) to the 27.3 clone, so 0066 is the baseline, not a stale pick.

What is not yet proven is that the objects are ours. `kernel.log` for that stage is 74 lines of `libtool` warnings, so the
stage was a relink, and `build-k27-piano.sh` documents that a native build starts as an APFS clone (`cp -c`) of `k27-obj`.
The tree used here came from a copy of an earlier WSL checkout, so its `out/` may carry objects built elsewhere. To settle
it, `out/x12377/build/xnu/k27-native-obj` was moved aside and the kernel stage re-run from scratch; the clean build's
sha256 is compared with `c896e203...`. Equal means this host builds the kernel from the pinned sources; different means the
first artifact reused another machine's objects and must not be quoted as a reproducible build of ours. Until that check
returns, this document makes no claim that the kernel was compiled here.

## Recorded as a fallback: building ld64 from source, and exactly where it stops

`ld64-957.1` source (apple-oss-distributions/ld64) compiles with Xcode 27 once its non-public headers are supplied:

| Need | Resolution |
| --- | --- |
| `<tapi/tapi.h>` | `tapi-1600.0.11.8` `include/`, plus `tapi/Version.inc` written by hand from `Version.inc.in` (4 macros; CMake normally generates it). Link with the toolchain's own `libtapi.dylib` (TAPI 21.0.0, 35 `LinkerInterfaceFile` symbols) |
| `corecrypto/ccdigest.h` | the k27 tree's `xnu/EXTERNAL_HEADERS/corecrypto` (symlinked alone: adding the whole `EXTERNAL_HEADERS` shadows SDK headers and breaks `sys/unistd.h` and `dlfcn.h`) |
| `mach-o/dyld_priv.h` | not in the SDK; from apple-oss-distributions/dyld. `dyld-1378` does not parse against macOS 27's availability macros (`bridgeos(...)` in `__API_AVAILABLE`); an older tag is needed |
| `os/lock_private.h` | `libplatform-375.120.2 private/os/`, used only for `os_lock_unfair_s` and `os_lock_lock`/`unlock`, and `libSystem.tbd` does export `os_lock_lock` |
| `os/base_private.h` | not in that libplatform release at all (only `os/base.h` in the SDK); a shim is enough for those three uses |
| `-lxar` | neither Xcode nor CLT ships `libxar`; only `src/ld/passes/bitcode_bundle.cpp` uses it |

Keep this as the documented fallback for a host without the CLT 26.6 dmg. It is not what this host builds with now.

## The tablet, and the gaps that still decide the route

Read-only re-check: `ro.product.model` `25091RP04C`, `ro.product.device` `piano`, 512 GB (`/data` 376 GiB), which is the
model and the G10 layout the skill expects. Its stock build is `OS3.0.309.0.WPYCNXM` where the skill asks for
`OS3.0.303.0.WPYCNXM`; the goal keeps 3.0.309, so that difference has to be understood before any write, not papered over.

Against `skills/install-piano-macos`, with G1's `ld-classic` closed and the k27 tree public, the route to a second unit now
turns on: **G4** the Turnip/Mesa delta is not public and there is no A830 desktop without it (last checked from this host:
`https://github.com/lenzhang/UniXNU-turnip` and `https://github.com/Droid-VM/UniXNU-turnip` could not be resolved, the
network reset on every attempt, so "not public" is still unproven either way); **G6** the write tooling is still pinned to
the reference unit - `tooling/persist/common.py` has no device-profile hook and `tooling/repro/unit-profile.py` is only the
read-only start; **G1b** the SystemOS boot shims; **G2** the base trust bundle; **G3** a Data seed from the IPSW template.
G7 (boot_a signing) is already solved on this side: this host signs with its own AVB key and knows that a host-signed image
can only be `fastboot boot`ed, since the device's own key differs.

## Resolved the same day: the kernel is built here, not relinked from another machine's objects

The clean rebuild finished in 63 s and produced 27,557,496 bytes with sha256
`12d3088acce0b2c375066d94dd60264766c9d6cb978f5cd03e062a1c81dcc3be`, which differs from the first link's
`c896e203...`. Comparing hashes was the wrong test and is recorded as such: a linked Mach-O carries a freshly
generated `LC_UUID` (`2B8C8311-3728-3ED7-938F-D75B0EFEEBC1` here), so two correct builds of identical sources never
share a sha256. What settles provenance instead:

```
$ python3 -c "...stat every .o/.cpo under k27-native-obj/DEVELOPMENT_ARM64_BCM2837..."
objects 1093   min 05:51:37  max 05:52:29  span_s 52.3   distinct_seconds 53
$ strings -a kernel-k27-native.macho | grep -m1 'Darwin Kernel Version'
@(#)VERSION: Darwin Kernel Version 27.0.0: Thu Nov 26 05:52:29 CST 2026; zhangqi:k27-native-obj/DEVELOPMENT_ARM64_BCM2837
```

All 1093 objects were written inside the build's own window, spread over 53 distinct seconds (~20/s at `JOBS=6`:
the BCM2837 `DEVELOPMENT` configuration is a small subset of xnu, most of it tiny generated or stub files), and the
`XNU_BUILD_STRING` names this user and this object tree. `kernel.log` for a clean stage is 7686 lines starting at
`CC externs.o`, against 74 lines for the relink that first produced an image; that relink was the *second* pass of
`build-k27-piano.sh` (the first pass exists to produce the platform recipes), which is why it looked impossibly fast.

The image is what the downstream tools expect: `__thread_starts` present (the k27 tree's `scripts/kc-build.py` reads
that section out of the base kernel), zero `LC_LOAD_DYLIB` and zero `LC_FILESET_ENTRY`, so it is a plain kernel Mach-O
rather than a fileset collection, exactly what `tooling/ci/release-assets.sh` checks before publishing.

Two notes for the next reader of this file: the Mac's clock runs 8 hours behind the tablet's and the operator's
timezone while still printing `CST`, so timestamps in these logs are not wall-clock comparable with the tablet's
logs; and reproducibility here means "this host, from the pinned sources, twice", not "byte-identical to the owner's
kernel" - the owner's own hashes are not published, and `LC_UUID` alone makes byte equality impossible.
