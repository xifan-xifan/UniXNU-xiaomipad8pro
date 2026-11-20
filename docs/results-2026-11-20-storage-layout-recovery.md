# Stock recovery and storage layout study (2026-11-20)

Two existing stock LUN 0 GPT capture sets passed hash, geometry, and CRC checks and matched byte for byte. The table below is an offline candidate using 4 KiB LBAs and 1 MiB partition start alignment. The study allocates 20 GiB for the root, 64 GiB for the Data partition and 1 GiB for the ESP. The root allocation has 537,919,488 bytes of headroom over the calculated default root image size. The usable Data store must be sized from its seed and store metadata before the plan is finalized.

| GPT entry | Name | First LBA | Last LBA | Size |
|---:|---|---:|---:|---:|
| 34 | `userdata` | 3,632,128 | 102,269,695 | 376.272 GiB |
| 35 | `linux_rootfs` | 102,269,696 | 107,512,575 | 20 GiB |
| 36 | `linux_data` | 107,512,576 | 124,289,791 | 64 GiB |
| 37 | `esp` | 124,289,792 | 124,551,935 | 1 GiB |

The remaining 251 usable LBAs precede the backup GPT. The offline check changed only the `userdata` ending LBA and three previously unused entries, recomputed both CRCs, parsed both GPT copies, and restored the original boundary bytes in memory. The candidate array SHA-256 is `ba67d6aa7863835bb019231cfe97054138f649e58e7e06d08abe858da3f9751e`. The reproduction script is under `out/k27-independent-20261120/host-run/recovery-audit/check-layout-offline.py`. The addresses remain provisional. The study did not emit flashable GPT files or touch the tablet.

The verified 3.0.309 fastboot archive contains a 45 GiB expanded sparse `userdata.img` and a 64 MiB expanded sparse `metadata.img`. Their sizes come from the sparse headers. Both fit their respective storage budgets as a matter of geometry. This does not establish that restoring the sparse images starts or grows Android `/data` on the new layout.

The 3.0.309 `recovery.img` (SHA-256 `4c9710a400100e5da89f6657ba9d7081742d03e8bca92579e9e113b334d24e12`) has an Android boot v4 header and a 29,216,071-byte legacy-LZ4 ramdisk. The extracted `system/etc/recovery.fstab` refers to `/data` and `/metadata` by GPT name, marks both F2FS and `formattable`, and gives no fixed length for either. It ties userdata encryption to the key directory on `/metadata`. The recovery ramdisk contains `make_f2fs` and `fsck.f2fs`. Xiaomi's recovery binary also contains pre-wipe and post-wipe paths for preserving and restoring a vendor `cust.img`, with a block-map read and SHA-1 check. Neither the fstab nor the embedded strings establish how the vendor hooks behave after the boundary changes.

For the write plan, verify the recovery entry and return paths, then specify how to confirm the shortened filesystem is usable, the encryption and checkpoint state is healthy, and the expected HyperOS build boots. A rollback rehearsal must also verify the restore order for both GPT copies, the A-slot boot triplet, and `/data` and `/metadata` initialization. A confirmed failed shortened-layout boot should return to the stock recovery procedure instead of retrying blindly. The owner reports the device was already factory-reset and has no personal data. No new tablet backup was created.
