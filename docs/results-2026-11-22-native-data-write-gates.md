# Native Data and UFS write gates (2026-11-22)

NativeRamBlock skips opening the persistent Data store in mode 0. NativeUfs independently rejects store writes unless the mode is 2 and the request fits the opened extent. The pre-init writable APFS Data mount can still write to the RAM backing. NativeUfs log and export writes use a separate window controlled by `log=1` and root geometry. Verified root builds reject the development root-write configuration at compile time.

UbuntuPiano Python 3.12 ran `tooling/persist/test-data-store.py` with the retained UniXNU scripts on PYTHONPATH and reported zero failures. `tooling/persist/test-abl-state.py` also passed. These tests cover host tooling, including seed and trust pairing and slot preflight logic. The tablet checks still need to establish the effective boot arguments, live slot state, and actual I/O.
