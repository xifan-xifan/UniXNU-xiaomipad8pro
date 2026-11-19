# Host Validation Results

Date: 2026-11-19. These are host-only checks; no tablet command, device write, backup, or native image boot was performed.

The following checks ran on `mac-lan` in `/Users/zhangqi/Codex/UniXNU-xiaomipad8pro`:

- `tooling/native/test_piano_input.py`: 13 tests, all passed.
- `tooling/native/test_rtc_offset.py`: 9 tests, all passed.
- `package/NativeRamBlock/Test/run-tests.sh`: sparse, persist, direct, seeddisk, guard, volatile, volatile-stress, volatile-pool, and power suites passed with 0 failures.
- `package/include/tests/run.sh`: include, lock, WCN rails, MM votes, and SPMI checks passed; the locked SPMI run reported 6,000/6,000 reads, 1,500/1,500 writes, and 11,569/11,569 commands without failure or drop.
- `bash tooling/k27/test-piano-bootargs.sh`: passed.
- `tooling/k27/test-piano-guard.sh`: passed.
- `tooling/k27/test-piano-pm.sh`: passed, including 1,015 s2idle policy checks.
- `tooling/k27/check-native-kexts.py --self-test`: passed; VM-era descriptors/collections were refused and native descriptors accepted.
- `PATH=/tmp/codex-mtools-4.0.50-src/install/bin:$PATH /tmp/cpython311-3.11.18/python/bin/python3.11 tooling/persist/test-k273-only.py`: `PASS: 0 failure(s)`.

The first direct invocation of `test-piano-bootargs.sh` lacked the executable bit and returned permission denied; rerunning it explicitly through `bash` passed. The first `test-k273-only.py` invocation lacked `mformat` in `PATH`; rerunning with the existing verified GNU mtools 4.0.50 prefix passed. These are invocation-environment details, not source failures.

The tests validate host logic and fake-tablet maintenance flows only. They do not provide the missing native k27 tree, native root/Data layout, signing configuration, or device acceptance evidence.
