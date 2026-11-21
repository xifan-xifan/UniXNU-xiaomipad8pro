# Stock-root policy object replay

On November 21, 2026, the retained Mac host rebuilt the VerifiedRoot policy assembly from the stock-window SHNGROOT v2 inputs, rather than the earlier fixture headers:

- `policies/OS/trusted-key.bin` and `policies/SystemOS/trusted-key.bin` are byte-identical;
- each `manifest.header.bin` is 544 bytes;
- the arm64e policy object was assembled from those headers and the shared 395-byte anchor;
- the retained verified NativeUfs object, VerifiedRoot implementation, XNU crypto adapter, and the new policy object linked successfully as an iOS arm64e relocatable object.

The resulting object exports `_verified_root_key`, `_verified_root_policies`, and `_verified_root_policy_count`, and defines `_vr_open`; the only unresolved symbol shown by `nm` is `_boot_progress_poll`, which is supplied by the existing boot-progress provider during kernel collection integration. This is a host-only link check. It does not qualify the collection or device boot.
