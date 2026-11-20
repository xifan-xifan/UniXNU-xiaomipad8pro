# ESP host regression on 2026-11-20

On device-tree commit `6da1992`, `tooling/k273/test-esp-split.py` passed in UbuntuPiano with GNU mtools 4.0.50. The builder section of `tooling/persist/test-k273-only.py` passed, including byte-identical images from identical inputs, which had failed with mtools 4.0.43. The full WSL run still failed in the fake-tablet phase, including the partition size fixture and macOS-specific `F_NOCACHE` path. It terminated when the fixture did not produce `installed.json`.

The complete `tooling/persist/test-k273-only.py` run passed on the Mac with CPython 3.11.18 and the existing GNU mtools 4.0.50 prefix. The log is at `/tmp/piano-esp-regression.eDOxn7/regression.log` on the Mac. All partition writes and rollback checks in this run targeted host fixtures. The proposed 3.0.309 device layout still requires live geometry and recovery validation.
