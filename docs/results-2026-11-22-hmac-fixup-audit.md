# First HMAC linked pointer audit

The retained entropy-split KC matches SHA-256 `adc409a58ec08f3d355998835d6e249aeac6bcef52845bbb4da0b716e7cc4002`. In its corresponding Mac `kernel.macho`, the SHA-512 descriptor at `0xfffffff007dd0f70` gives output, state and block sizes of 64, 64 and 128 bytes.

Object relocations at offsets `0x30` and `0x38` name `_ccsha512_ltc_compress` and `_ccsha512_final`. The linked words `0x8011c7a004bd0944` and `0x80011bda04bd0d78` encode those targets and use address-diverse IA discriminators `0xc7a0` and `0x1bda`, matching the `blraa` call sites in `_cchmac_init`. `_ccdigest_init` makes direct calls to `_memmove` and `_cc_disable_dit`. The encoding was read with `llvm-objdump` after `dyld_info -fixups` rejected the kernel's segment permissions.

The runtime fixups and authenticated dispatch remain unverified. The existing device marker 166 only establishes the point before the first HMAC call. The next diagnostic needs separate markers for HMAC entry, digest setup and compression, recording phase numbers only. Read-only ADB confirmed HyperOS `OS3.0.309.0.WPYCNXM` on slot `_a` with `sys.boot_completed=1`.
