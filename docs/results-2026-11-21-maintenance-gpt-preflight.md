# Read-only maintenance GPT check (2026-11-21)

The frozen manual-exit RAM image matched SHA-256 `932be704d5450fe7fbe722d074a96c2764862627862496ad7dd82fd82d1799e2`. The maintenance shell identified `/dev/sda` as UFS LUN 0 with 4096-byte logical sectors and 510165778432 bytes. No UFS mounts, active swap or LUN 0 holders were observed. `blockdev` was available; `python3` was not found. The packaged preflight command was not run.

Reads into `/run` returned 24576 bytes from LBA 0 and 20480 bytes from LBA 124552187. SHA-256 matched stock: primary `9c096761ee7b9c12c1a5c3ef3f100bc27abccbbafb798a141aa4320eb81af25d`, backup `f8b53b4538913ccf2ac385d8bd87eb8ad689aa3f439222480a95d05c625ca4fe`.

After explicit maintenance exit, ABL again reported product piano, userspace=no, snapshot=none and slot A successful=yes, unbootable=no, retries=6. Fastboot reboot returned to HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, `sys.boot_completed=1`. No persistent partition writes were issued. This does not qualify the maintenance writer or stock Recovery on shortened userdata.
