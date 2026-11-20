# Native collection link probe (2026-11-20)

The isolated build linked all 26 configured native descriptors. The input set includes the two provisional NativeGopDisplay objects built with recovered ABI 2 declarations. The builder reported zero problem slots for NativeGopDisplay versus IOMobileFramebufferLegacy and for the other ten subclass comparisons. The native kext and sysctl checks passed. The builder serialized 364,799 chained pointers. The output is 135,806,976 bytes with SHA-256 `47da294b467a3996ec8277728212566360b428f43292f94bee73533f0c8b72dc`.

The link checks cover the subclass vtables the builder compares. Full display field layout and reference provenance, DarwinBoot loading, and kernel startup remain unverified. The probe stays under `out/k27-independent-20261120/host-run/` with the archived driver object inventory and link log. Hardware testing and storage qualification remain pending.
