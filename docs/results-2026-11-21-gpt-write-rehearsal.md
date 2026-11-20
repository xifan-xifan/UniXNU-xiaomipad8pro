# Candidate GPT write and restore rehearsal (2026-11-21)

On an UbuntuPiano sparse file sized to the tablet's LUN 0, candidate GPT boundaries were written and synced in backup-then-primary order. Each boundary matched its expected bytes on readback. `fdisk -b 4096 -l` recognized the stock table, the candidate layout and the restored stock table. Both restored boundaries matched the original captures byte for byte.

The first `sgdisk -v` pass used the regular file directly and assumed 512-byte sectors, so its output did not verify either GPT. A subsequent run on a read-only loop device with 4096-byte sectors recognized the restored stock GPT and reported no problems, with warnings about gaps around the entry arrays, the inherited 64-entry array and stock userdata alignment. An independent check on a read-only 4096-byte-sector loop device also recognized the candidate. `sgdisk -v` reported no problems and 251 free sectors, with the inherited entry-array warnings. `sgdisk -p` reported all four expected extents. The stock userdata alignment warning did not recur on the candidate.

The existing `maint-session.py` encodes the prior installed layout in its partition checks and requires verified backup entries before writes. A deployment tool for the proposed layout must check live geometry against the candidate and verify its recovery inputs from the retained captures. This rehearsal used only a host file. Reinitialization of encrypted Android userdata remains unverified on the tablet.

## Backup-copy-only interruption

A follow-up host-only fault injection changed the backup GPT's `userdata` ending LBA to the candidate boundary while leaving the primary GPT stock, modeling interruption before the primary-copy write. On the 4 KiB-sector sparse fixture, `fdisk -b 4096 -l` parsed the still-stock primary table successfully. The original 20,480-byte backup tail was restored in a `finally` path, flushed and verified byte-for-byte. No tablet block device was opened. This tests that the original primary copy remains parseable in this interruption state; it does not test ABL/Recovery selection between conflicting GPT copies or on-device rollback.
