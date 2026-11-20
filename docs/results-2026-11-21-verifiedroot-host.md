# VerifiedRoot host check (2026-11-21)

The pinned UniXNU core `d830d23b0d19c9322d906c21afeca1d6c54aeffe` resolves `kits/VerifiedRoot` to `974f043d0c71ba2e60331483a41c6c6d35274ee3`. That exact source was checked out on Mac and UbuntuPiano. The UbuntuPiano checkout and reports are under `/tmp/piano-vroot-src.bI7jsW`.

UbuntuPiano built the pinned `src/verified_root.c` with `tests/openssl_provider.c` using Clang and OpenSSL. The pinned `tools/verify_image.py` verified an RSA3072 signature and all four blocks of the 12,800-byte upstream fixture, including its short final block, and rejected a copy with one changed byte in block index 1. The upstream all-in-one test is Mac-specific. The Mac's SDK selection failed for the iPhone target, leaving the target crypto adapter and collection link unqualified.

OpenSSL separately accepted both retained fixture signatures for OS and the local unshimmed SystemOS. Their test anchor is suitable for host regression only. The normal device build still selects the development NativeUfs route. The deployment link needs the pinned verifier and XNU crypto adapter, the registered NativeUfs entry and the owner policy for the final SystemOS bytes and volume UUID. Tablet testing did not run.
