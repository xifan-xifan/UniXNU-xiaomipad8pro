# Verified NativeUfs collection without the host poll object

A temporary copy of the frozen NativeUfs source omitted the optional poll callback in standalone mode. Source-runtime mode retains the callback. Two direct read helpers were marked `maybe_unused` because this verified configuration uses the authenticated read path. Apple clang compiled both kext objects with the retained recipe and its normal `-Werror` policy.

Apple ld combined NativeUfs, VerifiedRoot, the XNU crypto adapter and the stock policy object as iOS 27 arm64e kernel objects without linker warnings. The result defines `_vr_verify_block` and `_verified_root_policies` and has no `_boot_progress_poll` reference. Its SHA-256 is `c5e6e1c163001548321d743cd7a48adac2dcf7d144d99eade563fd0ea165efc9`.

The retained 26-driver collection recipe linked without `host-poll.o`. The pinned DarwinBoot parser accepted the resulting KC and passed the capability, malformed range, fileset and entry checks. The KC is unsigned and has SHA-256 `72e5240f8a3286d9233136a540c7129dab4c66fc3d2789c8c5287f6e18332601`. The local output directory `out/verified-no-poll-23aca0b8/` retains the source diff, replay arguments, descriptors and link log. Final payload pairing and unattended recovery after handoff require separate verification before this can be a device candidate.
