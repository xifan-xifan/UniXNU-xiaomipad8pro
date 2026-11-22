# SHA-512 cache-clean entry test

The diagnostic preserves the authenticated call and adds `dc civac` and `dsb sy` after writing entry stage 192. Disassembly confirms these instructions follow the store, before stack allocation. The KC passed the DarwinBoot host parser and matched the FAT readback. Boot image SHA-256: `7ee477dd90e8088da5ab7785e081698d00e176e628f5fda5c7a06bba5942d76c`.

One temporary boot recovered stage 186 after watchdog reset. There is no evidence that the entry probe completed. This does not distinguish a failure at the call from a failure before or during the probe. For the next candidate, add separate cache-clean witnesses immediately before the authenticated branch and after its return.

ABL preflight passed. HyperOS OS3.0.309.0.WPYCNXM returned unattended on slot A with boot completed. Local evidence: `out/no-demura-control/sha-clean-*`.
