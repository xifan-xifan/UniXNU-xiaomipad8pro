# First HMAC compression checkpoint

The frozen Mac build replaced only `cchmac.o` and `cchmac_init.o` in the entropy-split link. The diagnostic records phase numbers, with 185 before copying the outer digest state, 186 before the first outer compression call and 187 after it returns. `_port_crumb` retained its linked address `0xfffffff00c89be50`. Build commands and sources are retained on the Mac in `$TMPDIR/piano-hmac-stages-tfw540i3`.

| Artifact | SHA-256 |
| --- | --- |
| Kernel Mach-O | `d01cf639a4d445cd718cced9b7d8f1ebea9db78a34a20ab3974d547ce7d79ce3` |
| Kernel collection | `830a2d9a4ddeb46393e11d12feab2ca861cbec727d07ead366729554d4545ade` |
| Temporary image | `0e2360e48239464aa01f4d1c456b1fd397c69e991d5a573910597c9d6df337d9` |
| Eight-byte stage capture | `e4fa8ef4ad09b88a5871dd5af7e9749351bbe56d184a8934badc9383088ca842` |

The DarwinBoot host parser passed and the KC extracted from the candidate FAT matched byte for byte. The initial three-second USB enumeration window expired before the device appeared in ABL. A fresh enumeration confirmed ABL and the preflight verified slot A successful and bootable, six retries and snapshot status `none`. The candidate booted once via `fastboot boot`.

The watchdog returned to ABL. The temporary reader recovered stage **186** from `0x8c589be50`. Execution reached the first outer SHA-512 compression call after copying the initial state. A checkpoint inside the compression routine is needed to distinguish failure before entry from a failure inside the routine or on return. The device returned unattended to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, with boot completed. Local artifacts and the run log are under `out/no-demura-control/hmac-stages-*`.
