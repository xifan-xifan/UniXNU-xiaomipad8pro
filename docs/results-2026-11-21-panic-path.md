# UFS probe panic path audit (2026-11-21)

The tablet reported HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, and `sys.boot_completed=1` over ADB. This audit made no boot attempt.

In the prepared XNU source at `/tmp/k27-source-trial-fw22zrr9/src`, `osfmk/arm/model_dep.c` calls `PEHaltRestart(kPEPanicSync)` while saving the panic log. In `iokit/Kernel/IOPlatformExpert.cpp`, the base `haltRestart` returns zero for that action, but `PEHaltRestartInternal` first synchronizes the panic buffers and dispatches the platform panic action queue. `iokit/Kernel/IOPlatformActions.cpp` dispatches that queue to registered services. The base return value therefore gives no evidence about registered driver callbacks.

`osfmk/kern/debug.c` has a `panic-spin` tunable that stops the ordinary `kdp_machine_reboot_type(kPEPanicRestartCPU)` path when enabled. It acts after panic logging and does not cover the direct `kPEPanicRestartCPUNoCallouts` path for `DEBUGGER_OPTION_PANICLOGANDREBOOT`. The same file can invoke `sync_internal()` when `DEBUGGER_OPTION_SYNC_ON_PANIC_UNSAFE` is set. The retained mailbox candidate needs inspection of the actual linked panic handlers and active options. The full mailbox readback from the menu-only test qualifies the transfer, with XNU log production and reset retention still to verify.
