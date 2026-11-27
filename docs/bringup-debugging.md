# Native bring-up on piano: how to boot, debug and iterate without watching the screen

## Current baseline (2026-11-27)

The reference tablet booted **v13.1y12q** from disk on 2026-11-26. It runs native macOS 27.3 (26C5035r) as Mac17,11 at
EL1, with Gunyah at EL2. The installed `boot_a` image carries the v5 BootShim and UEFI firmware. The ESP's `macos-27.3` entry supplies the
kernel collection, ADT, trust cache, Data seed and boot arguments. The 13 GiB XNU window measured 13310 MiB on v13.1y12l.
[27.3 line](k273-line.md#current-line-2026-11-27) · [Installed profile](USING.md#2-the-installed-profile)

The macOS root uses the OS and SystemOS windows on `linux_rootfs`. The 200 GiB direct Data store is on `linux_data`
at LBA 71859642. Data mounts in serial mode and switches to concurrent mode after mount. Write-back and queue modes
stay off. USB debug network TX flow control is on by default and passed 66/66 exact transfers in qualification.
[November 26 results](results-2026-11-26.md) · [Data concurrency and durability](drivers/data-store-m4-m5.md)

Daily display stays at 60 Hz with Night Shift routing, native panel sleep and refresh switching off. Fixed 120 Hz and
144 Hz have RAM scanout results. [Support status](STATUS.md) · [Hardware backlog](BACKLOG-hardware.md)

Routine tests use RAM boots. Changes to the ESP, `boot_a`, GPT and Linux-era root and Data regions follow the approved
maintenance procedure with verified backups and read-back. Firmware and calibration partitions and all `_b` partitions
stay read-only. Send debug `oem` commands only to the in-RAM `piano-uefi` device. Never use `systemctl isolate` on the tablet.
[Project rules](PROJECT-MAP.md#6-hard-rules) · [Install design](persistent-boot.md) · [Operator's guide](USING.md)

## 1. The boot chain, and what each stage needs

```text
Disk: ABL → BootShim + v5 FD on boot_a → UniXNU.efi → macos-27.3 on the ESP
 RAM: ABL → BootShim + FD A in the RAM image → UniXNU.efi → RAM payload
Both: DarwinBoot.efi → native k27 XNU at EL1 (Gunyah at EL2)
```

| Stage | What to check | Failure signal |
|---|---|---|
| ABL → BootShim on a RAM boot | The ARM64 header has `text_offset 0`, `flags 0xa` and an `image_size` covering BootShim + FD. [RAM image builder](../tooling/bootimg/build-ram-images.py) | In the early bring-up, ABL accepted the download but returned to fastboot about 80 s later. Subsequent boots in that session reported `Load Error`. |
| UEFI drivers | Use FD A, built with the bundled piano-uefi DXEs. [FD builder](../tooling/firmware/build-fd.sh) | The early FD B built with the unit's OS3.0.303 DXEs reset before XNU. |
| UEFI → UniXNU RAM payload | SEC preserves the ABL device tree argument in the entry-argument HOB. The RAM payload takes precedence over the ESP. [SEC patch](../firmware/patches/mu-silicium/0001-sec-entry-arg-hob.patch) · [RAM precedence patch](../firmware/patches/unixnu/0001-UniXNULib-config-prefer-the-RAM-payload.patch) | `UNIXNURAMDISK: no entry argument HOB` if entry argument x0 was lost. An FD without the precedence patch can select the ESP configuration. |
| DarwinBoot → XNU | The linear BGRA GOP comes from SimpleFbDxe. [Linear GOP patch](../firmware/patches/0002-pianoPkg-linear-gop-for-darwinboot.patch) | With the old Blt-only GOP, the menu redraws after `DARWINBOOT: boot refused: Unsupported`. |
| DarwinBoot RAM window | The 27.3 ADT requests `0x33fe00000` bytes. The FD builder stamps the FD that supports it. Check the console's `NativeRamBlock: RAM window` line for 13310 MiB. [27.3 ADT](../configs/k273-adt.toml) · [DarwinBoot window patch](../firmware/patches/darwinboot/0001-DarwinBoot-window-up-to-13-GiB-shrink-to-the-largest-run.patch) | Older FD builds refuse requests above 8 GiB. A short allocation produces `taken (largest run)`, and a missing run produces `NO WINDOW`. |
| XNU CPUs | Boot with all eight ADT CPUs using `sheng-smp=1`. [RAM image builder](../tooling/bootimg/build-ram-images.py) | The early `cpus=1` test aborted in `recount_switch_thread+0x84`. |
| XNU root and Data | NativeUfs publishes both SHNGROOT windows. Check `native-ufs-root-part`, `data-store-part` and the store state against the intended profile. [UFS driver](drivers/ufs.md) · [Data store](data-store.md) | Repeated `Waiting on <dict> IOMedia` points to root matching. The store-state line distinguishes a resumed store from volatile Data. |
| UFS controller | The driver redirects SID 0x60 to the bypass bank and exits Hibern8 for UFS link bring-up. [UFS driver](drivers/ufs.md) | The early failures were `conflicting UFS stream mapping` and `FAILED at NOP OUT`. |

In the RAM image menu, volume keys move and power selects. The RAM builder defaults to `PIANO_MENU_TIMEOUT=0` and
`PIANO_MENU_DEFAULT=macos-8cpu`. A timeout of `-1` waits for a key. [RAM image builder](../tooling/bootimg/build-ram-images.py)

## 2. Seeing what happened without a camera

| Stage or symptom | Evidence and tool |
|---|---|
| ABL download or firmware handoff | During a RAM boot the USB sequence is ABL fastboot → disconnect → `piano-uefi` fastboot → disconnect as DarwinBoot hands off to XNU. Before handoff, the in-RAM UEFI exposes the console (`oem log`), panel (`oem screenshot`) and the `Piano_Log` DEBUG buffer (`oem dump`, address `D7F00000`, length `100000` in hex). The DEBUG buffer includes driver loads and the `UNIXNU:` and `UNIXNURAMDISK:` lines. |
| Live XNU console and monitor | PianoDwc3 device mode (`piano-usb=2`) enumerates as `1209:0001` with CDC-ACM console and shell ports. The first console open replays the retained log. The monitor's `args` command shows the merged boot arguments. [Host USB console](../tooling/native/usb-console.py) · [USB debug driver](drivers/usb-device-debug.md) |
| Console after a reset | Patches 0055 and 0058 mirror the console and replicate the ring and its header 11 times for majority decoding. With 0071 the last 64 KiB of the 7 MiB CVP region is reserved for the separate reset reason, and the console uses the first `0x6f0000` bytes. The decoder's `decode` mode reads an existing dump file on the host. [Mailbox decoder](../tooling/native/xnu-log.py) · [27.3 ADT](../configs/k273-adt.toml) |
| Reset intent or boot hang | Patch 0063 guards the installed boot, with the launchd exec check at 180 s and continued Gunyah watchdog and CPU liveness checks. Patch 0071 records reset intent independently of the console ring when the ADT reservation validates and recording is enabled. An absent record leaves the cause unknown. [Boot guard](boot-guard.md) · [Halt diagnosis](halt.md) · [Halt trace grader](../tooling/native/halt-trace-verdict.py) |
| Desktop is up but the panel is suspect | Use the native framebuffer capture for the pixels presented to scanout. [Framebuffer capture source](../tooling/native/gop-shot/gop-shot.c) · [Camera app checks](tools/cam-apps-check.md) |
| Panic addresses or a missing backtrace | The native kernel runs unslid. Resolve PC and LR against the matching unstripped kernel, and use the collection's `natives.json` for kext addresses. Patch 0073 adds opt-in panic text on the retained linear scanout during the panic hold. [Historical panic example](#6-historical-panic-investigations-2026-11-12-to-2026-11-13) · [Panic text diagnostics](../tooling/k27/piano-panic-text/README.md) |

For host controls and console capture, see the [operator's guide](USING.md#3-from-the-mac). Use the
[boot timeline tool](../tooling/native/boot-timeline.py) to derive timing from a recorded console and the
[boot-time guide](boot-time.md) for the historical timing probes.

### Historical console recovery (2026-11-10 to 2026-11-12)

The first native RAM boots recovered the log through Ubuntu's `/dev/mem` using aligned 8-byte accesses.
Ubuntu's `ramoops@a3500000` lost its log on reboot because the firmware uses that range for display demura.
The CVP mailbox retained enough data to decode after a warm reset, with about 4% bit loss measured in the first tests.
A hard power-off lost the mailbox. The first disk boot on November 11 reached macOS but had no USB console. The
November 12 cold-start changes brought up the USB console on the installed path.
[November 11 forensics](forensics-2026-11-11.md) · [November 12 results](results-2026-11-12.md)

## 3. The unattended loop

[The RAM test loop](../tooling/native/native-loop.sh) boots a test image and checks that ABL answers after it returns.
The RAM builder defaults to `PIANO_RESET_AFTER=45`. Patch 0056 schedules the PSCI reset with the Gunyah watchdog as
a backstop, and 0057 directs the reset to ABL fastboot. A test with an active `piano-reset-after` owns the watchdog
in place of the installed boot guard.

The loop normally leaves the tablet in ABL after the test. Use the USB console tool for a live capture. A second boot for
a mailbox dump requires a matching `DUMP_GUARD_RECORD` with a qualified independent reset path that covers the ABL
handoff before USB appears. The loop also checks battery voltage before permitting a dump boot. Consult the
[RAM test procedure](USING.md#4-test-a-change-ram-boot-nothing-written) for the reference layout and seed handling.

### Historical two-boot capture (2026-11-11)

The original unattended loop ran the test, rebooted to ABL, then booted a separate dump image to pull the console
mailbox from its in-RAM UEFI. The dump image reset after one second in XNU. The earlier Ubuntu-based loop retired
when the macOS root moved to `linux_rootfs`. The maintenance RAM image remains part of the approved install procedure.
[November 11 results](results-2026-11-11.md) · [Maintenance shell](../tooling/native/maint-shell.py) · [Install design](persistent-boot.md)

## 4. Rebuild after a change

Set `PIANO_K27_TREE` in the build environment to the prepared k27 source tree. The [lockfile](../unixnu.lock)
pins its source, and the [k27 tree delta](../patches/k27-tree/README.md) documents its reconstruction. The
[shell helper](../tooling/k27/k27-tree.sh) and [Python helper](../tooling/k27/k27_tree.py) resolve the setting.
The tree supplies the kernel sources, kext compile recipes and generated headers, and the KC tooling.

The [27.3 kernel preparer](../tooling/k273/prepare-k27-tree.sh) clones that tree, applies the 27.3 kernel and KC
deltas and the native patches above `K27_TREE_TOP`, and relinks at the mac17j collection's segment addresses.
Set `K27_TREE_TOP` to match the native patch level of the input tree (default `0066`). The mac17j base and other
27.3 build inputs are described in the [27.3 line guide](k273-line.md) and [second-unit install guide](../skills/install-piano-macos/SKILL.md).

| Build or check | Entry point |
|---|---|
| Rebuild FD A | [`build-fd.sh`](../tooling/firmware/build-fd.sh) checks and stamps the RAM-payload precedence and DarwinBoot RAM-window patches. |
| Prepare kernel, then build the KC, ADT and RAM images | [`prepare-k27-tree.sh`](../tooling/k273/prepare-k27-tree.sh), then [`build-k273-image.sh`](../tooling/k273/build-k273-image.sh) with `K273_ROOT_PART=linux_rootfs` on the reference layout and `K273_KERNEL` set to the path printed by the preparer. The script's default root partition is `linux_data`, and it defaults to volatile Data. |
| Check an image offline | [`verify-ram-images.py`](../tooling/bootimg/verify-ram-images.py) takes the build record, for example `out/ram/build-A-bundled.json`. [`check-piano-bootargs-kc.py`](../tooling/k27/check-piano-bootargs-kc.py) checks 0076 in the built kernel or with `--kc` in the collection. |
| Build an install candidate | [`build-k273-esp.py`](../tooling/k273/build-k273-esp.py) builds the 27.3-only ESP with the current inputs and `--root-part linux_rootfs`. [Reference layout and install procedure](persistent-boot.md#138-the-whole-disk-layout-the-root-on-linux_rootfs-the-owners-data-grown-over-linux_data-2026-11-14) |
| Grade qualification evidence | [GUI checks](tools/gui-checks.md) describe recorded desktop, networking and Energy Mode evidence. [Durability checks](tools/durability.md) cover RAM qualification against a bound scratch window. |

### Native kernel patches 0053–0076

The 27.3 build uses xnu-12377.121.6 with the k27 baseline series 0001–0052, the native patches below, and the
[27.3 kernel patches](../patches/k273/README.md). The [native patch index](../patches/k27/README.md) has implementation
details. The series at this revision has no 0072 patch file.

| Patch | Change |
|---|---|
| [0053](../patches/k27/0053-k27-one-kernel-for-the-VM-and-the-HLOS-the-PSCI-conduit-from.patch) | Selects the PSCI conduit from the ADT for the VM and native kernel. |
| [0054](../patches/k27/0054-k27-SM8750-MPIDRs-Affinity2-in-the-reset-handler-s-and-ml_ge.patch) | Includes MPIDR Affinity2 in SM8750 CPU lookup. |
| [0055](../patches/k27/0055-k27-pstore-console-phys-the-console-mirrored-into-a-RAM-mail.patch) | Mirrors the XNU console into the retained RAM mailbox. |
| [0056](../patches/k27/0056-k27-piano-reset-after-s-PSCI-reset-after-N-seconds-Gunyah-wa.patch) | Adds a timed PSCI reset with the Gunyah watchdog as a backstop. |
| [0057](../patches/k27/0057-k27-piano-reset-to-bootloader-the-PMIC-SDAM-restart-reason-s.patch) | Sets the PMIC restart reason for a return to ABL fastboot. |
| [0058](../patches/k27/0058-k27-pstore-console-header-stored-11-times-too-DBGR.patch) | Replicates the mailbox header for majority recovery. |
| [0059](../patches/k27/0059-k27-on-BSD-list-the-busy-IOServices-who-holds-the-IO.patch) | Dumps busy IOServices when IOBSD matching times out. |
| [0060](../patches/k27/0060-k27-piano-rtc-1-the-calendar-from-the-PMIC-RTC.patch) | Initializes the calendar from the PMIC RTC. |
| [0061](../patches/k27/0061-k27-piano-cpuidle-N-the-idle-loop-s-WFI-becomes-a-PS.patch) | Adds PSCI CPU idle states and counters. |
| [0062](../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch) | Adds suspend-to-idle and wake reporting. |
| [0063](../patches/k27/0063-k27-piano-boot-guard-SECS-the-installed-system-s-boo.patch) | Adds the installed boot guard and CPU liveness checks. |
| [0064](../patches/k27/0064-k27-piano-llcc-island-live-1-and-piano-sleep-cancel-1.patch) | Checks the live LLCC island reservation and lets drivers cancel a pending suspend. |
| [0065](../patches/k27/0065-k27-a-failed-contiguous-kmem-allocation-gives-its-pages-back-ungobbled.patch) | Rejects oversized contiguous allocations early and fixes page accounting on failure. |
| [0066](../patches/k27/0066-k27-piano-boot-args-the-ADT-s-tail-appended-to-the-boot-args.patch) | Appends the selected ADT boot-argument tail before tunable and kext reads. |
| [0067](../patches/k27/0067-k27-cs-native4k-threads-N-and-cs-native4k-skip-1-the-native-4K-code-signing-pre-hash.patch) | Adds parallel native 4 KiB code-signing pre-hash and its opt-in skip. |
| [0068](../patches/k27/0068-k27-piano-halt-trace-1-markers-on-the-halt-restart-path.patch) | Adds halt/restart tracing and shutdown timeout diagnostics. |
| [0069](../patches/k27/0069-k27-piano-psci-halt-restart-and-power-off-through-PSCI-the-guard-keeps-a-started-restart-s-reason.patch) | Routes native halt and restart through PSCI, preserves the reason after restart starts, and adds an opt-in panic hold. |
| [0070](../patches/k27/0070-k27-cs-native4k-skip-accepts-only-the-blobs-it-published-review-M4.patch) | Restricts the pre-hash skip to code-signing blobs published by that path. |
| [0071](../patches/k27/0071-k27-independent-retained-reset-reason.patch) | Adds a retained reset reason in a separate CVP reservation. |
| [0073](../patches/k27/0073-k27-panic-text-on-linear-scanout.patch) | Adds opt-in panic text on the retained linear scanout. |
| [0074](../patches/k27/0074-k27-gpu-addon-app-cryptex-view.patch) | Adds the read-only App cryptex library view used by the GPU plug-in. |
| [0075](../patches/k27/0075-k27-user-va-47-bit.patch) | Enables 47-bit user VA by default, used by Chrome WebGPU. |
| [0076](../patches/k27/0076-k27-export-merged-piano-boot-args.patch) | Exports the full merged boot arguments through read-only `kern.piano_bootargs`. |

## 5. The native kext rule (2026-11-11)

The native collection links the device kexts in [the native allowlist](../configs/k27-native-kexts.txt) alongside
the Apple components from the 27.3 base collection. The [KC builder](../tooling/kc/build-kc-k27.sh) runs the
[allowlist and denylist check](../tooling/k27/check-native-kexts.py) on the descriptors and the linked collection.
The 27.3 ESP builder also checks for VM-era bundles. Code reused from that era lives in the native packages with
its `UPSTREAM` attribution.

### 5.1 Historical VM-era kext tests (2026-11-11 to 2026-11-13)

Early native tests used kexts from the retired 27.0.1 VM line. These were the observed failures and the corresponding
native changes. [November 11 results](results-2026-11-11.md) · [November 13 results](results-2026-11-13.md)

| Early test | Observed behavior | Native implementation |
|---|---|---|
| Virtio display with the other virtio kexts | IOBSD matching timed out after 30 s (`!BSD`). Both UFS windows stayed busy and root failed to mount. The busy-service dump exposed an APFS module-lock wait when publication preceded IOBSD matching. | NativeUfs publishes after IOBSD matches. NativeGopDisplay supplies the display nub and GOP framebuffer path. |
| Stub GPU accelerator | WindowServer's type-`0x4147` opens failed with `kIOReturnBusy`, and WindowServer crash-looped every 30–40 s. | PianoA830 supplies the hardware accelerator. NativeSoftGpu provides a software Metal path. |
| Virtio Ethernet, audio, video, input and block | The kexts started without a corresponding virtio device. In the early comparison, SystemOS matching fell from 4 s to 0.5 s after removal. | Native drivers, including NativeUfs for root and NativeRamBlock for Data. [Driver index](drivers/README.md) |
| Built-in `en0` supplied by the VM Ethernet kext | Without a built-in interface, TimeSync lacked a clock ID and the login session stalled. | PianoNullEthernet supplied the interface and desktop startup resumed. The daily profile now has built-in Wi-Fi on `en0`. [Support status](STATUS.md) |
| NativeAES removal | AppleImage4 waited for an `IOAESAccelerator` and IOKit never went idle. | SoftwareAES restored the accelerator on 2026-11-13. [Identity notes](identity.md#8-the-240-s-network-wait-under-k114c-2026-11-13-and-its-cause-appleimage4) |

### 5.2 The boot arguments

These are the bring-up and diagnostic switches relevant to the daily profile and RAM tests. The
[legacy argument reference](k27-userland-args.md#legacy-names) documents the original `droidvm-*` names.

| Argument | Scope | Effect |
|---|---|---|
| `sheng-smp=1` | Native boot | Starts all eight ADT CPUs. |
| `native-ufs-root=1 native-ufs-root-part=linux_rootfs` | Installed root and reference-layout RAM tests | Publishes the OS and SystemOS windows on `linux_rootfs`. |
| `data-persist=2 data-store-part=linux_data data-store-lba=71859642` | Installed Data | Resumes the direct store at its bound LBA. `data-persist=0` keeps changes volatile, the 27.3 RAM builder's default. [Store guard](data-store.md#3-the-store-guard) |
| `data-store-concurrent=2` | Installed daily | Starts the store serial and arms post-mount promotion. The startup job switches a healthy resumed store to concurrent mode. [Data startup job](data-store.md#concurrent-mode-at-boot-piano-data-concurrent-2026-11-26) |
| `piano-usb=2`, `piano-usb-net=1`, `piano-usb-shell=1` | USB debug | Enables the XNU device-mode console, CDC-NCM network and userland root shell respectively. [USB debug arguments](drivers/usb-device-debug.md#6-boot-args-and-the-mac-side) |
| `piano-usb-net-txflow=1` | Installed daily | Enables TX backpressure on the USB debug network. [USB TX flow control](drivers/usb-net-reset.md) |
| `pstore-console=0x9cb00000 pstore-console-size=0x700000`, `piano-reset-reason=0` | Retained diagnostics | The address and size select the CVP mailbox. With 0071 the console is capped at `0x6f0000` bytes. The final 64 KiB holds the independent reset-reason record when the ADT layout validates. The `=0` switch disables recording and leaves the reservation in place. |
| `piano-boot-guard=180` | Installed boot | Checks launchd's exec milestone at 180 s and keeps the watchdog and CPU checks running. [Boot guard arguments](boot-guard.md#4-boot-arguments) |
| `piano-reset-after=<s> piano-reset-to=bootloader` | Timed RAM run | Schedules PSCI reset back to ABL fastboot. An active timer takes over from the boot guard. |
| `piano-halt-trace=1`, `piano-panic-hold-ms=<ms>`, `piano-panic-text=1` | Opt-in diagnostics | Traces halt/restart phases, delays panic restart, and renders panic text with a hold enabled and the copy display path. [Halt diagnosis](halt.md) · [Panic text diagnostics](../tooling/k27/piano-panic-text/README.md) |
| `sheng-gpu-addon=1`, `piano-uva47=1` | GPU plug-in and user address space | The add-on switch enables the App cryptex view when its source is present (0074). The 47-bit user VA is the kernel default (0075). [November 26 results](results-2026-11-26.md) |
| `piano-dpu-modes=120`, `piano-dpu-modes=144` | RAM qualification | Selects the fixed-rate scanout profiles. Daily stays at 60 Hz. [Refresh profiles](drivers/dpu-m3.md) |
| `piano-boot-args=NAME` | Split boot arguments | Selects `/chosen/piano-boot-args-NAME`. `=0` skips the tail. With no selector, 0066 looks for `/chosen/piano-boot-args`. |

DarwinBoot passes at most 1023 text bytes. By default, the builders keep its line under 1000 bytes and can move eligible
`piano-*` arguments to the ADT. `piano-reset-after`, `piano-reset-to`, `piano-boot-guard*` and the selector
stay on the line, as do `native-ufs-*`, `data-*`, `pstore-*`, `droidvm-*` and `sheng-smp`. The early
`static_if_init()` keys (`validation_disables`, `serverperfmode`, `lcks`, `wdt`) also belong on the line.

- [The RAM builder](../tooling/bootimg/build-ram-images.py) uses the `ram` set and accepts `PIANO_BOOT_ARGS_SPLIT=1`.
  [The 27.3 ESP builder](../tooling/k273/build-k273-esp.py) uses the `macos` set, accepts `--split` and
  reassembles an already split base before applying edits and splitting again.
- **Kernel and userland.** Patch 0066 appends the tail to `PE_boot_args()` after device-tree initialization,
  up to 4095 text bytes, before tunables or kexts read it. The USB monitor's `args` shows that merged line.
  `kern.bootargs` keeps its 1024-byte buffer and clips at a whole-argument boundary. Patch **0076** exposes
  the full line through read-only `kern.piano_bootargs`. An undersized buffer gets `ENOMEM`. The USB shell,
  USB network address helper, rotation and brightness sync use the full line with a legacy fallback on
  `ENOENT`. The builders check that the relevant staged readers can see their keys.
  [C reader helper](../package/include/PianoBootArgs.h) · [Shell reader helper](../tooling/native/piano-usbnet-address/piano-bootargs.sh)
- **Host checks.** [`boot_args.py`](../tooling/adt/boot_args.py) prints the ADT sets with `--show ADT` and
  previews the merged line with `--line LINE`. [`test-piano-bootargs.sh`](../tooling/k27/test-piano-bootargs.sh)
  covers split, merge and clipping. [`test-esp-split.py`](../tooling/k273/test-esp-split.py) covers rebuilding a
  split ESP with host fixtures.

`piano-long-boot-args=1` opts into a 1008-byte loader line and supports up to
4095 bytes after the ADT merge. [Boot argument length and reader placement](boot-args-length.md)

**Historical split-line test (2026-11-13).** The first long-line RAM test of 0066 used a 663-byte DarwinBoot line
and a 407-byte ADT tail, producing 1071 bytes in XNU. The test plan compared the console's full `args` output
with the legacy sysctl's 1017-byte whole-argument prefix. [Migration notes](k273-line.md#4-ram-images-and-the-historical-first-boots)

### 5.3 The ADT identity

The 27.3 profile uses `Mac17,11`, `K114c` / `K114cAP`, and `vmm-present=0`. Its ADT describes the
packed Data seed, the XNU RAM window, and the retained CVP reservation for the 0071 diagnostics. The
`gop-display` node uses the `paravirtualizedgraphics,gpu` compatible string for IOMobileFramebuffer.
[27.3 ADT](../configs/k273-adt.toml) · [GOP display node](../configs/k27-gop-display.toml)

**Historical 27.0.1 identity.** The VM-derived ADT used `VirtualMac2,1`, `VMA2MACOS`, and
`vmm-present=1` by default. Turning that last flag off in an early run let `watchdogd` run and exit
twice without a watchdog device, and launchd panicked about 150 s into the run. A 240 s run with
`vmm-present=1` had no panic.
[VM-era ADT](../configs/k27-adt.toml) · [Identity experiments](identity.md)

### 5.4 Historical macOS migration (2026-11-12 to 2026-11-14)

The first 27.3 root went on `linux_data` alongside 27.0.1. The 27.3-only ESP replaced the dual ESP on
2026-11-14, and the retired 27.0.1 root and store were cleared. The 27.3 root moved onto `linux_rootfs`.
The Data store grew to 200 GiB on `linux_data` with seed identity `9bd6f902`.
[Migration record](k273-line.md#7-historical-273-only-installation-2026-11-14) · [November 14 results](results-2026-11-14.md)

## 6. Historical panic investigations (2026-11-12 to 2026-11-13)

The native kernel runs unslid, so `atos` with the matching unstripped arm64e kernel can resolve the panic PC and
LR directly. Use the built collection's native kext addresses for kext frames. A kernel assertion may stop in
a `.cold.N` stub (here `brk #0xbffc`, ESR `0xf200bffc`). Both PC and LR can land in the asserting function.
With no backtrace, callee-saved registers and disassembly can identify the caller. Single-bit errors can
remain after decoding a retained mailbox.

**Contiguous allocation and `vm_page_unwire` (2026-11-12).** The 13 GiB investigation recorded
`vm_resident.c:6226 Assertion failed: !mem->vmp_gobbled` at `vm_page_unwire.cold.3`, with LR at
`vm_page_unwire+860`. Registers and disassembly pointed to the delayed-work unwire path
(`vm_page_do_delayed_work`). The disassembly matched the caller's retained `mov w26, #0x14` and
`adrp x28, vm_pageout_debug`. An earlier 8 GiB run counted 2,517,918 pages for 2,097,152 physical pages,
a difference of about 1.6 GiB.

In the old allocation path, `kmem_alloc_contig_guard()` took contiguous pages before `kmem_find_space()`.
If the map refused the allocation, the error path freed the pages with `vmp_gobbled` still set and left
them in the wire and gobble counts. A later `vm_page_grab()` could return a page in that state, and
`vm_object_iopl_wire_empty()` wired fresh pages without clearing the bit. The GPU's 2 GiB pool request
exceeded the kernel map's limit of 2 GiB less one page. PianoA830 capped its requests, and 0065 fixed both
the premature search and the free-path accounting.
[November 12 results](results-2026-11-12.md) · [0065 patch](../patches/k27/0065-k27-a-failed-contiguous-kmem-allocation-gives-its-pages-back-ungobbled.patch)

**Camera kernel stack overflow (2026-11-13).** The console reported `Invalid kernel stack pointer (probable
overflow)` and `MACH Reboot`. The reported PC was in `el1_sp0_synchronous_vector_long` at the store into
the exception frame below the 16 KiB kernel stack. The fault address was `sp + 8`, in the guard page. The
LR and callee-saved registers from the interrupted thread pointed to `vm_map_store_lookup_entry` while
PianoCamera mapped the 64 KiB DRV-2 range. The kext's own frames down to `mapRsc` used 13,456 bytes.
The host stack checker estimates a kext's frame usage from a named entry point. The runtime low-water mark came from
`ml_stack_remaining()`. [Camera investigation](drivers/camera.md#84-results) · [Kext stack checker](../tooling/k27/check-kext-stack.py)

On the original verbose console, panic photos could show doubled glyphs (for example, `ppaanniicc`).
Keeping every second character recovered the text. For the newer opt-in scanout diagnostic, see
[panic text diagnostics](../tooling/k27/piano-panic-text/README.md).
