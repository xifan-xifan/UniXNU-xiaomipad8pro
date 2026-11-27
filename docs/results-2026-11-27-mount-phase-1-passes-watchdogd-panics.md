# 2026-11-27 - `mount-phase-1` passes on the shimmed root; the boot now dies on `watchdogd`

RAM boot of `out/k273-ram-8748-shim/boot-piano-macos-ram-A-bundled.img` (`6001bc1c06559e1f1434631c8824dedb744e3c47511da1b7b8b67399a449c70b`)
against the root written an hour earlier (`9e4ef505…` on `linux_rootfs`). Unattended: the image resets itself to ABL
(`piano-reset-after=420 piano-reset-to=bootloader`), the reader image pulls the pstore mailbox, and the tablet went back to a
booting system by itself. Evidence: `logs/native-runs/ram-k273-shimroot-20261127-110401/` (`run.log`, the 7 MiB mailbox,
`console.txt` decoded with `tooling/native/xnu-log.py decode`).

## The wall is gone

launchd's boot tasks all ran and completed, in order: `fsck`, **`mount-phase-1`**, `data-protection`,
`restore-datapartition` (optional, absent), `mount-phase-2`, `init-with-data-volume`. There is no
`(mount-phase-1) posix_spawn(): 1: Operation not permitted` anywhere in the console - the SystemOS window in this root carries
`/sbin/mount` and `/sbin/fsck`, so `mount -P N` is answered and the boot task exits 0 instead of panicking the kernel.
Upstream gap G1b, as the upstream text describes it, is closed on this unit by the shim window rather than by a rewritten shim.

Around it, the same run shows what the earlier attempts never reached:

* APFS mounted the `SystemOS` volume and the `Data` volume (`apfs_log_op_with_proc: … mount-complete volume Data, requested by: init (pid 1)`),
  and k27's firmlinks installed: `/private`, `/Applications`, `/Library`, `/Users`, `/Volumes`, `/cores`, `/opt`,
  `/usr/local` all `: 0` (`/System/Volumes/Hardware: 2`, which is the expected "no such volume").
* `NativeGopDisplay: display on`, `IOMobileFramebufferUserClient … UserClient display type is 1`.
* `en0` attached (`ifnet_attach: all kernel threads created for interface en0 have been scheduled at least once`), DriverKit
  `IOUserDockChannelSerial` started ok, and daemons ran: `authd`, `systemstatusd`, `PerfPowerService`, `mDNSResponderHelper`,
  `xpcproxy` children, `kernelmanagerd` absent (`KextLog: Loading FileSet KC(s)`).

## What stops it now

`watchdogd` cannot run natively (no `IOWatchdog` provider), exits `reason namespace 2 subcode 0x5` (SIGTRAP) and launchd treats
its exit as fatal: `panic(watchdogd (pid 182) exited - exit reason namespace 2 subcode 0x5, description: none)`, then the
`Kernel data abort` and the `pe_init.c:753 Assertion failed: panic_info` that follow from panicking inside the panic path.
Before it, `locationd` took a `FATAL exception: type=0x1 reason=0x1c` and was SIGKILLed; `watchdogd` churned as pids 57, 146,
182 with `AMFI: Denying core dump for pid …(watchdogd)`. ABL was back after 154 s, well before the image's own 420 s reset.

This is the failure the project already characterised for the desktop line, and the same file already carries the fix
(`docs/attended-desktop.md` §2: `watchdogd` SIGTRAPs, KeepAlive relaunches it every ~10 s, `droidvm-disable-jobs=` is inert
natively; `docs/userland.md` §5: disable it in the seed's launchd override database with
`tooling/k27/stage-boot-run.py --disable-job <label>`, or by `k27-deny-exec`). The desk images disable
`com.apple.watchdogd`, `com.apple.mobileactivationd`, `com.apple.security.xprotectd` and `com.apple.PerfPowerServices` in the
seed's override database. This root/RAM line has no such staging yet - the boot args still carry only
`droidvm-disable-jobs=…`, which does nothing natively - so the next iteration is to stage those four labels into the 8748 seed
this image is built from, rebuild, and boot again. Note for whoever runs it: the store on `linux_data` is this image's own seed
(`8e1a87e1…`), so `data-store-replace=1` is inert against it, and a page the store has not taken still reads the seed - which is
what makes seed staging readable here.

## Not verified by this run

A desktop, GPU, input, network traffic, audio, Data surviving a reboot, Android coming back, and switching in either direction.
What this run proves is narrower and specific: the shimmed SystemOS root gets launchd past `mount-phase-1` on this unit.
