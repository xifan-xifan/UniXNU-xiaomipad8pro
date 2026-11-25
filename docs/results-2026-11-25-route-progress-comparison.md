# Route progress compared: the 2026-11-12/18 experiments vs the 2026-11-19/25 native line (2026-11-25)

Requested comparison of the two Codex runs on this tablet. Everything below is quoted from files on this host; no device action.

## Sources used

| run | workspace | evidence |
| --- | --- | --- |
| iPadOS 17.7.11 branch, then DroidVM macOS | `E:\codex\2026-11-01\ni` | `PIANO-IPADOS-CURRENT-STATUS-20261114.md`, `PIANO-MACOS-ACTIVE-20261114.md`, `PIANO-DROIDVM-MACOS27-RECIPE-20261117.md`, `outputs\piano-k273-native-ramboot-20261115\STATUS.md` |
| DroidVM macOS acceptance | `E:\codex\DroidVM` | `ACCEPTANCE-2026-11-18.md` and its proof files |
| native macOS 27.3 line (this line) | `E:\project\UniXNU-xiaomipad8pro` | `HANDOFF.md`, `docs/k273-line.md` (upstream author's claims), `docs/results-2026-11-2*.md`, `artifacts/ARTIFACTS.md` |

## Where each line stands

| Milestone | iPadOS branch (to r170) | DroidVM macOS (to 11-18) | native 27.3 line (this line, to 11-25) |
| --- | --- | --- | --- |
| Kernel handed off by DarwinBoot | yes | yes (edk2 -> UniXNU -> DarwinBoot -> XNU) | yes, and repeatedly |
| Furthest XNU stage observed | user-space attempts only via upstream report; local runs stopped at the Apple logo with no console | `launchd`, `DroidVMDisplay`, AMFI | `bsd_init`/`sd_init`, `bsd_autoconf`, IOKit registry walk, full UFS init (NOP, power mode query, START STOP UNIT, TEST UNIT READY), GPT + root manifest, media publication, the OS container's `dev_init` |
| Root volume mounted | no | yes (VM guest root, virtio-blk) | no; the root wait wants APFS volume UUID `84581E99-...` |
| WindowServer / login / desktop | upstream report only (home screen photo `2f4bfa67`, A830 Metal icons, ~25 fps), never reproduced locally | yes on the isolated copy: login window, then logged-in desktop with `Finder`/`Dock`/`SystemUIServer` at UID 501, touch input navigated Finder and created `DroidVM-Acceptance` | not started |
| Persistence proof (write, reboot, read back) | no | yes, three guest `shutdown -h now` cycles, digests matched (`57a2d467...`, `9a07160d...`, `5871c6b5...`) | not started; `linux_data` store still blank |
| Android coexistence | HyperOS untouched, but the runs left the tablet needing a physical power-off | verified each round: `sys.boot_completed=1`, launcher foreground, 3/3 LAN ping | verified every RAM run until 11-25: returns to HyperOS `OS3.0.309.0.WPYCNXM` by itself via `piano-reset-after` |
| OS version | iPadOS 17.7.11 (21H461) | macOS 27.0.1 (26A434) | macOS 27.3 (26C5035r) |
| Storage layout | none | VM disk files in Android `/data` | candidate GPT installed: `linux_rootfs` 20 GiB written and read back, `linux_data` 64 GiB, `esp` 1 GiB; slot A boot triplet write plan ready |
| Unattended recovery | no (r166+ lost serial/ADB/fastboot, needed a power button) | guest shutdown worked; host untouched | yes, and this is what the current outage is measured against |
| Observability | COM3 serial, then nothing | crosvm VNC of the guest scanout | pstore/panic-zone dumps through the reader RAM image; IOLog-level |
| Blocking wall | GPU user-space delivery and Data UUID mismatch | interactive session on a read-only root, plus Data store label | root volume publication, and real interrupt delivery so waits/timeouts return |

## What transfers and what does not

The DroidVM acceptance is the only real desktop-and-persistence evidence on this hardware, and it does not touch this objective:
the guest root is a virtio-blk device provided by crosvm, so it never exercised UFS, the APFS root match, GIC delivery,
GOP display bring-up or HID, and it is 27.0.1 rather than 27.3. Its transferable content is method-level: the proof file +
digest + reboot protocol, and the fact that HyperOS keeps working around a macOS guest.

What this line has over the earlier one is the native stack reaching `bsd_init` with its own drivers, unattended return to
HyperOS on every run, and a dump channel that survives a hung kernel. What it still lacks is exactly the last three rows of
the desktop column, and the two walls named there are the ones the next builds must clear.