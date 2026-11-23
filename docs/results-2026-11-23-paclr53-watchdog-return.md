# Gunyah watchdog returns the tablet unattended; the reset-after timer never fires

Date: 2026-11-23. Device `7ccdd855`. Both runs used `fastboot boot` only; no partition, GPT or userdata write. After each run the tablet returned to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, `sys.boot_completed=1`, without a manual power cycle. The user had rebooted the tablet by hand once before the first run because the previous root trial left it without USB.

## Short-argument timer check returned, but not through the timer

`out/no-demura-control/boot-piano-paclr53-timercheck.img`, SHA-256 `678f6729f9d2e6c8a9c59d58158cd99aa0ab7624b19de74da2553487fdca26f4`, carries `-v fips_mode=4 native-ufs-probe=2 data-persist=0 pstore-console=0x9cb00000 pstore-console-size=0x700000 vm_compression_limit=1572864 piano-reset-after=20 piano-reset-to=bootloader cpus=1`. `tooling/native/run-paclr53-timercheck.ps1` saw the fastboot serial return **80.1 s** after `fastboot boot` OKAY, booted `reader.img`, and staged the console.

The staged console contains both of these lines:

```
piano-reset-after: reset at 20 s, Gunyah watchdog armed (32 s bite)
NativeUfs: start on K114cAP probe=2 root=0 root-part=linux_rootfs
```

It contains no `piano-reset-after: 20 s reached, PSCI SYSTEM_RESET` line. The 20 s `thread_call` therefore never ran, while the Gunyah watchdog that 0056 armed at 30000/32000 ms with `GH_WDT_CONTROL=3` did: the serial came back about 32 s after the arm, which is later than the arm but far from 20 s. This is the first observed, unattended return of the k27 payload.

The same 80.1 s figure recurred on the second run, so it is the watchdog period, not a boot-time coincidence.

## Root path reaches the second DMA allocation and stops there

`out/no-demura-control/boot-piano-paclr53-rootshort.img`, SHA-256 `a324ebe1e17b8aa7b219757f844934bd229b10890aa7cc96eaf9abb12089c46f`, runs the same short argument line with `native-ufs-root=1 native-ufs-diagnostics=1 native-ufs-root-part=linux_rootfs`. `tooling/native/run-paclr53-rootshort.ps1` measured the same 80.1 s return, dumped the console and the panic zone, and HyperOS came back by itself.

The console shows the root path progressing past every stage that the earlier six-minute hang left unknown. It prints `NativeUfs: start ... root=1 root-part=linux_rootfs`, `GCC gdsc=fa22f800 axi=08000001 ahb=88000003 aggre=08000001`, `HC cap=d5c7031f ver=00000400 hcs=0000000f hce=00000001 is=00000400`, and both `NativeUfs: DMA attempt 0` and `NativeUfs: DMA attempt 1`. It contains no `NativeUfs: DMA virt=...` line and no `DMA region ... slots` line, so neither `IOBufferMemoryDescriptor::inTaskWithOptions` call returned with a usable buffer before the watchdog bit.

Other kernel work continued while that thread waited: `NativeGopDisplay` mapped the framebuffer and started its copy thread, `dlil_init` reported its threads scheduled, and `CoreAnalyticsHub::start` began. The root path is therefore blocked in the physically contiguous allocation, not in the controller, SMMU or a spin.

## What the backstop now gives

Every k27 run that carries `piano-reset-after` returns to fastboot in about 80 s with its DRAM console intact, so the next diagnostics need no manual restart. `piano-reset-after` should be read as "arm the 32 s Gunyah bite", not "reset after N seconds": the `thread_call` timer is dead before interrupts are enabled and cannot be relied on for any deadline.

## Next

Instrument the allocation loop in `package/NativeUfs/NativeUfs.cpp` around line 1402 so each attempt logs entry, allocation result, virtual address, physical address and contiguity before it is either used or released, then rebuild the kext object and the kernelcache. `/private/tmp/NativeUfs-writable.o` retains the 2026-11-20 object; the `unixnu-research` tree that `tooling/k27/build-native-ufs.sh` needs is no longer on the Mac and must be restored before that rebuild.