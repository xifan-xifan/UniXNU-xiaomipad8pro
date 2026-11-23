# NOTICE: firmware/selector

Our own code (BSD-2-Clause-Patent), written for piano's combined boot_a (T-16 UEFI OS switch).

It follows the entry and packer contract of piano-uefi's selector (`selector/entry.S`, `selector/boot.c`, `tools/pack.py`), which
is BSD-2-Clause-Patent. Pin: checkout `unixnu` 31545f9, our FD's pin (`tooling/firmware/setup-mu.sh`). What it follows:
- the metadata block ("PIANO-UEFI-SEL1", the same 80-byte layout);
- the redirect of the kernel's second instruction;
- the register save/restore around the decision;
- the splash framebuffer clean.

piano-uefi's `selector/keys.c` is GPL-2.0-only and was not used. The key, SDAM and SPMI arbiter access here is ours, written from
this repo's own register facts: `package/PianoInput/Buttons.h`, `package/PianoPower/SpmiObserver.h`, `package/include/PianoSpmi.h`
and PianoDwc3's `setRestartReason`.
