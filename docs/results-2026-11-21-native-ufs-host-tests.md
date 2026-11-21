# NativeUfs host tests (2026-11-21)

Ran `package/NativeUfs/tests/run-tests.sh` from its test directory under UbuntuPiano after stripping the Windows CRLF on the shell input. All eight combinations passed: RootCommands, RootPower, RootWindowRead and RootDirect, each plain and ASan/UBSan.

This verifies host command, power, window-read and direct-read behavior for the development (`NATIVE_UFS_UNVERIFIED_ROOT`) build. It does not qualify an authenticated root policy, a composed SystemOS image or tablet execution.
