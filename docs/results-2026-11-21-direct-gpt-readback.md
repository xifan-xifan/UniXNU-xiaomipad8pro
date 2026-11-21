# Direct GPT readback on piano (2026-11-21)

The frozen readback image matched manifest SHA-256 `e568d44958b9f47934d084b373d198d9e7ad7637a45f87c54381b4a7ac743225`. The packaged command read six primary and five backup sectors using direct I/O from `sda`, exited successfully and reported `gpt_preflight=ready`.

- Primary: `9c096761ee7b9c12c1a5c3ef3f100bc27abccbbafb798a141aa4320eb81af25d`.
- Backup: `f8b53b4538913ccf2ac385d8bd87eb8ad689aa3f439222480a95d05c625ca4fe`.

Both digests match the retained stock fragments. Explicit maintenance exit disconnected the USB shell and returned to ABL. Slot A remained successful and bootable with six retries and no snapshot update. Ordinary ABL reboot returned to HyperOS `OS3.0.309.0.WPYCNXM` on slot A with `sys.boot_completed=1`. No GPT write was issued. Post-write flush, rollback and Recovery on shortened userdata remain untested.
