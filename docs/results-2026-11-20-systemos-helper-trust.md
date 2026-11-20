# Apple boot helper trust in 26C5035r

On the Mac, a read-only attachment of the retained OS image supplied the original `/sbin/mount`, `/sbin/fsck` and `/sbin/fsck_apfs`. All three passed `codesign --verify --strict --all-architectures`. The attachment was detached after inspection. Mac logs are under `/tmp/piano-helper-abi.A46rmu`.

| Helper | arm64e CDHash |
| --- | --- |
| `mount` | `902ebcec8d8cffcfda65e423c250e8ef2739e27f` |
| `fsck` | `a739080f5d0d5214091729b644b4846d03c1c9dd` |
| `fsck_apfs` | `6e9904ce3462f8698fc8f89b28c7bd6af4876bce` |

All three hashes occur at entry boundaries in the first v2 module (18,568 entries) of the retained Apple trust bundle used by the firmware addon RAM candidate. This establishes that the original Apple executables can be kept byte for byte as helper inputs. It does not qualify their execution in the composed root or authorize Apple fsck repair flags. The production shim still needs an explicit device selection and write policy, and its own signature and trust evidence.
