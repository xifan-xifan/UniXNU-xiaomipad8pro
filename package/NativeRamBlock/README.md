# NativeRamBlock

RAM block storage driver. Provides block devices from RAM and a persistent Data overlay.

Current state: used for the RAM-backed Data volume and the optional persistent overlay.

Linked into XNU by `unixnu build kernel`.

UniXNU-piano: the Data disk of native macOS on piano: the seed from the payload plus the store in NativeUfs's window at the end of
`linux_rootfs` (`data-persist=2`). Store format, the store guard (another seed's store is kept, Data volatile), flatten
(self-contained stores) and the boot arguments: [docs/data-store.md](../../docs/data-store.md). Host tests: `Test/run-tests.sh`.
Across a system sleep (`native-ufs-suspend=1`, opt-in): `PowerHold.h`, [docs/drivers/ufs.md](../../docs/drivers/ufs.md) §12.

The [Disk M5 host test](tests/writeback_barrier_model_host.cpp) runs via
`python3 scripts/ci/self-tests.py --only package/NativeRamBlock/tests/`. It models provider sync and power loss
around the production admission, overlay, and UFS command headers under ASan/UBSan.
Daily write-back remains on **HOLD** pending an owner decision. `data-store-write-back=0` remains the default.
