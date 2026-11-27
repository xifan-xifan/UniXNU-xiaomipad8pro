# Boot argument length on piano

On 2026-11-26 the image recipe reported a 1013-byte command line and refused it. The builder applies a **1000-byte exclusive** budget, including the selected CPU suffix and any `piano-boot-args` selector. Its error mentions DarwinBoot's separate **1023-byte inclusive** limit. A 1013-byte ASCII input fits in DarwinBoot's field. A line longer than that field can already travel as a short loader line plus an ADT property on k27 0066. Patch 0076 exposes the same merged line to userland.

## Ubuntu mainline comparison

The reference sources are `xiaomipad-8pro-mainline` at `b43fc2b` and `linux-sm8750-piano` at `b5100d5f2e0b`. DarwinBoot references below use the `unixnu.lock` pin `8544957e`. XNU source paths refer to the prepared k27 source tree, with 0066 and 0076 recorded in this repository's patch series.

| Stage | Ubuntu mainline reference | UniXNU-piano |
|---|---|---|
| Build budget and boot transport | `xiaomipad-8pro-mainline/tools/build-persistent-boot.py:41–44,279–307,311–344,398–417` counts UTF-8 bytes and caps the shared RAM-check command line at 511. The v2 `extra_cmdline` field was dropped by piano's ABL. The persistent Linux set uses the v4 transport. | The RAM recipe puts the XNU line in `EFI/UniXNU/unixnu.toml` inside the payload. The Android wrapper hands off to UEFI. `tooling/bootimg/build-ram-images.py:256–266,418–469` composes and checks the entries. In `tooling/k273/build-k273-esp.py`, `split_line`, `diag_budget` and `main` assemble the installed 27.3 entries. |
| Device tree and copy | `linux-sm8750-piano/arch/arm64/kernel/setup.c:170–200,281–299` maps and scans the FDT before early parsing. `drivers/of/fdt.c:1091–1144,1228–1235` reads `/chosen/bootargs` into `boot_command_line` with `min(property length, COMMAND_LINE_SIZE)`. `arch/arm64/include/uapi/asm/setup.h:25` sets the capacity at 2048 including NUL. | `DarwinBootPkg/Driver/DarwinBoot.c:603,617–626,746–757` validates and copies the menu line into the 1024-byte ABI field. Its ADT walker checks each property's length and 4-byte alignment (`:273–369`). The builder writes `/chosen/piano-boot-args[-SET]` as a separate property. DarwinBoot carries the ADT through to XNU. |
| Parsing | `linux-sm8750-piano/init/main.c:808–826,1066–1093` parses early options from the bounded `boot_command_line`, prepares the saved/static command lines and parses general parameters. | `osfmk/arm/arm_init.c:312–316,368–386` passes the raw field to `static_if_init`, copies `*args` to `const_boot_args`, then calls `PE_init_platform(FALSE, args)`. k27 0066 extends the line after `SecureDTInit` in `pexpert/arm/pe_init.c:449–485`. |
| Userland view | `linux-sm8750-piano/init/main.c:671–713` saves the line with bootconfig additions when present. `fs/proc/cmdline.c:7–24` exports the saved line. `xiaomipad-8pro-mainline/device/boot/piano_bootslot.py:145–146,155–182` reads `/proc/cmdline` and `/proc/bootconfig` for boot identity. | k27 0066 keeps `kern.bootargs` and stackshots within the existing ABI. 0076 adds `kern.piano_bootargs` for the full merged text. See [0066](../patches/k27/0066-k27-piano-boot-args-the-ADT-s-tail-appended-to-the-boot-args.patch) and [0076](../patches/k27/0076-k27-export-merged-piano-boot-args.patch). |

## XNU path and byte budgets

| Stage | Limit and effect |
|---|---|
| RAM and 27.3 ESP image builders | The RAM recipe uses `fit()` and checks both the 1-CPU and 8-CPU lines. The daily install recipe calls `build-k273-esp.py --split` and the ESP builder checks the macOS, diagnostic and fastboot entries. The default split moves most `piano-*` words into the ADT, then applies the under-1000-byte check to the remaining line. The retired daily `tooling/persist/build-esp.py:398–403` has its own under-1000-byte check. |
| Menu and DarwinBoot | UniXNU `package/UniXNUPkg/Library/UniXNULib/Config.c:1165–1167` reads `entry.args` as Unicode and `Boot.c:82–93` forwards it as `Request.BootArgs`. DarwinBoot zero-fills `CHAR8 CommandLine[1024]` and rejects an index of 1023 when there is still a character, or a character above `0x7f`. `BootArgs.h:21,27–31` fixes the field and following offsets. A larger local array alone would make the existing fixed-size ABI copy unsafe. |
| Loader ADT and kernel copy | The builder stores a printable ASCII tail with a NUL in `/chosen/piano-boot-args-ram` or `/chosen/piano-boot-args-macos`. The firmware copies the ADT and its size alongside the fixed boot structure. The arm64 XNU field remains 1024 bytes (`pexpert/pexpert/arm64/boot.h:18,61`) and `arm_init` takes a structure copy before platform init. |
| Platform expert and kexts | 0066 resolves `piano-boot-args=SET` from the raw line (`=0` disables, no selector looks for `/chosen/piano-boot-args`). The merge buffer is 4096 bytes including NUL, so prefix + one space + tail can total 4095 text bytes. `PE_parse_boot_argn()` and `PE_parse_boot_arg_str()` use `PE_boot_args()` (`pexpert/gen/bootargs.c:191–217`), which returns the merged buffer after a successful append. The caller's `max_len` is the output value size. The parser scans the merged line through its NUL. |
| Fixed views | 0066's `kern.bootargs` uses a 1024-byte buffer and drops any partial final word. A few plug-ins provide a 1023-byte output buffer to the sysctl, which can fail on a full 1023-byte text answer plus NUL. Stackshots keep the DarwinBoot field. |
| Full userland view | 0076's read-only `kern.piano_bootargs` uses `sysctl_io_string` with no fixed truncation size. `PianoBootArgs.h` provides 4096 bytes and falls back to `kern.bootargs` on `ENOENT`. The USB network shell helper follows the same missing-OID convention. |

## Argument placement

If any active reader needs an argument in an earlier view, keep its complete token in that view. The 1008-byte layout budget includes the selector and any per-entry suffix. That puts every required early or legacy token within the legacy clip. The userland visibility check also tests the actual clip length for the plug-ins that pass a 1023-byte buffer to `sysctl`.

| Required placement | Arguments | Reader and evidence |
|---|---|---|
| DarwinBoot field | `piano-boot-args=SET`, `piano-extra-ram-stage` | k27 reads the selector before the merge (0066, `pe_bootargs.c`). The optional extra-RAM firmware patch reads its stage from `CommandLine` before handoff (`firmware/patches/darwinboot/0002-*.patch:71–88,156`). The opt-in layout also keeps `piano-long-boot-args=1` here as an image marker. |
| DarwinBoot field | `validation_disables`, `serverperfmode`, `lcks`, `wdt` | arm64 `static_if_init` runs before ADT setup and takes `args->CommandLine` (`osfmk/arm/arm_init.c:314–316,368–386`, `osfmk/kern/debug.c:2265–2298`). |
| DarwinBoot field by image policy | `piano-reset-after`, `piano-reset-to`, `piano-boot-guard` and `piano-boot-guard-*`, CPU selector, root/store binding and other unclassified platform or boot arguments | Preserve reset and guard behavior on a kernel without the tail. Keep the entry identity and compatibility keys visible to existing menu, host and OS readers. `tooling/adt/boot_args.py`, `tooling/k273/build-k273-esp.py:236–271`, and `docs/k27-userland-args.md` describe these consumers. |
| Legacy `kern.bootargs` prefix | `droidvm-utc`, `droidvm-tz`, `piano-camera-grab`, `piano-lw-quiet` | Seed `settime.sh` reads the clock and time zone. `tooling/native/piano-camera-grab/piano-camera-grab.c:29–39` reads the legacy sysctl. `piano-lw-quiet.sh:10` can read the full sysctl in this checkout. Treat the installed variant as legacy, as the image guard do. |
| Legacy `kern.bootargs` prefix | `soft-metal`, `soft-metal-{report,fb8,stubs,all,family}`, `piano-metal-{report,barriers}`, `piano-tu-debug`, `piano-tu-bo-clear`, `piano-surface-residency` | `package/NativeSoftMetal/NativeSoftMetalPlugin.mm:83–94`, `tooling/metal/turnip-a830/abort_reporter.mm:39–46`, frontend patches `0002`/`0007` and Mesa patch `0005` read the legacy view. Mesa `0013` reads the full sysctl in source and falls back to the legacy one. The opt-in layout keeps the plugin keys and any other `piano-metal-*` or `piano-tu-*` in the loader line for compatibility with staged bundles. |
| ADT tail, including past byte 1023 | Kext `piano-*` knobs such as `piano-gpu`, `piano-dpu`, `piano-wifi`, `piano-audio`, `piano-touch`, `piano-camera` and `piano-video`; audited display keys `native-gop-display-{builtin,flip-lossless,shot,night-shift}` | Their argument wrappers call the post-merge PE parser. Examples are `PianoDwc3.cpp:150–175`, `PianoA830/board/PianoA830.cpp:226–232`, `PianoBacklight.cpp:115–116`, `NativeGopDisplay.cpp:842,949,999,1090`, and `PianoDpu/DpuSlices.cpp:125` under `package/`. The four display keys have no early or fixed-width reader in this checkout. |
| ADT tail, including past byte 1023, with 0076 and matching helpers | `piano-usb-shell`, `piano-usb-net`, `piano-rotate` and the rotation helper's variants, `piano-backlight-{native,auto}` | The kernel consumers use the merged PE parser. `pianousb-shell.c:35–39`, `piano-bootargs.sh:3–18`, `piano-rotated.m:84–87` and `piano-brightness-sync.m:75–81` read the full view. The builder checks the staged reader ABI against the KC when a helper's argument is beyond the clip. |

The 27.3 ESP builder includes `native-gop-display-night-shift` and
`data-store-write-back` in the ADT whenever it splits a line. Both are
post-merge kext arguments. This placement keeps the installed USB helper
arguments in the legacy view for the v13.1y12u daily and diagnostic entries.
[Actual ESP replay](y12u-daily-combo-audit.md#exact-daily-profile-delta)

## Opt in and verify

Add `piano-long-boot-args=1` to `PIANO_EXTRA_ARGS` for RAM or pass `--arg piano-long-boot-args=1` to `build-k273-esp.py` for the current daily ESP. The default layout and its 999-byte budget still apply when the option is absent or `=0`. The opt-in fits the required loader words, selector and per-entry suffix in 1008 bytes and uses the 0066 tail for the rest, up to 4095 merged bytes. The builders preserve the relative order of repeated arguments. DarwinBoot, the kernel and the kexts use the existing ABI. The optional ESP diagnostic entry is included when its reset arguments fit both budgets.

Host-only tests use the existing RAM and ESP JSON records. They reconstruct the input arguments, replay the original layout byte for byte, exercise the opt-in and check the ADT merge and legacy clip. The 1013-byte replay adds a synthetic filler word to the recorded y12s qual line to produce the reported size after the default split.

```sh
bash tooling/k27/test-piano-bootargs.sh
python3 -B tooling/adt/test-boot-args-length.py \
  --qual-record "$QUAL_OUT/build-A-bundled.json" \
  --daily-record "$PIANO_MAIN/out/persistent/esp-k273-v131y12q.json"
```

| Recorded input | Opt-in loader bytes | Merged bytes | Legacy text bytes |
|---|---:|---:|---:|
| `v131y12s-qual1` | 863 / 868 (1 / 8 CPUs) | 3891 (8 CPUs) | 1019 (8 CPUs) |
| Qual replay with 1013-byte default prefix | 908 / 913 (1 / 8 CPUs) | 3936 (8 CPUs) | 1003 (8 CPUs) |
| Installed `v131y12q` daily | 749 / 797 (main / diag) | 3503 (main) | 1014 (main) |

BA3 passed on the `v131y12t-qual1` RAM qualification run on 2026-11-27. The
qualification compared `kern.piano_bootargs` with the build record and checked
the whole-word legacy clip in `kern.bootargs`. The opt-in can keep an argument
beyond the legacy clip visible to both a kext using the PE parser and a
userland helper using the merged sysctl.
