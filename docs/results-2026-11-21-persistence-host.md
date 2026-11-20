# macOS host persistence regression (2026-11-21)

The clean `cc779c6` snapshot passed `tooling/persist/test-k273-only.py` on mac-lan using Python 3.11 and mtools 4.0.50. The suite reported zero failures. The host log is `/tmp/piano-persist-regression.MDvmkh/regression311.log`. UbuntuPiano stopped at the macOS-specific `F_NOCACHE` call during verification of a simulated backup. The deployment tool retains its uncached readback requirement.

The Mac suite exercised simulated ESP writes and readback, refusal on mismatched Data bindings, interrupted recovery, and rollback. Its fixtures use the maintainer layout. The candidate tablet layout and stock Android reinitialization still require their own qualification. The suite did not access the tablet.
