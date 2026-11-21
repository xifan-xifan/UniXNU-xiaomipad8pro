# Candidate layout visible in stock Recovery (2026-11-21)

After the [candidate GPT trial](results-2026-11-21-candidate-gpt-tablet.md), stock Recovery exposed the following geometry. Linux sysfs uses 512-byte sectors. The candidate GPT manifest uses 4096-byte LBAs.

| Partition | Node | Start | Sector count |
| --- | --- | ---: | ---: |
| userdata | `sda34` | 29057024 | 789100544 |
| linux_rootfs | `sda35` | 818157568 | 41943040 |
| linux_data | `sda36` | 860100608 | 134217728 |
| esp | `sda37` | 994318336 | 2097152 |
| metadata | `sda20` | 290816 | 131072 |

All four candidate ranges match the manifest. `/data` and `/metadata` were unmounted at the time of observation. Recovery wipe, filesystem creation, encryption, Android startup and rollback remain unverified.
