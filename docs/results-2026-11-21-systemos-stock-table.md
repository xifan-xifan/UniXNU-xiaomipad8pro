# Stock SystemOS block table

On November 21, 2026, the retained Mac source `/private/tmp/piano-root-26C5035r.593r2m/141-22145-021.dmg` was read-only hashed in 4096-byte blocks. The generated table is `/private/tmp/piano-root-tables.LLOseQ/SystemOS-stock.vroot`, with size 22,790,144 bytes and SHA-256 `48402d8eccea26cd5e226830f97a08fe3c1d8d2265bdd38977c39b761a79d608`.

The expected table size is exactly `(2,917,138,432 / 4096) * 32 = 22,790,144` bytes. This is the correct table size for the retained reference SystemOS policy input. The existing `SystemOS.vroot` in the SHNGROOT v2 assembly is 50,331,648 bytes and describes the separate 6 GiB modified SystemOS, so it must not be reused for the stock policy.

This is a host-only input-generation result. It does not establish owner signature validity, native mount/fsck behavior, collection registration, XNU handoff, or device boot.
