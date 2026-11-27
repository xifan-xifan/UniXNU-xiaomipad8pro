# Project map — Xiaomi Pad 8 Pro (piano)

The reference tablet's daily install is **v13.1y12q**, running native macOS 27.3. [Current bring-up state](#7-current-bring-up-state-2026-11-27)

## 中文摘要

三个公开仓库都在 GitHub 账号 [lenzhang](https://github.com/lenzhang) 下，全部公开。

- [`UniXNU-piano`](https://github.com/lenzhang/UniXNU-xiaomipad8pro) 是小米平板 8 Pro（代号 piano，SM8750）的**设备仓库**，主线在 EL1 上原生启动 macOS。2026-11-27 的日用基线为 11 月 26 日验证的 **v13.1y12q**，运行 macOS 27.3（26C5035r），使用 k27 补丁 0053–0076 和 13 GiB 内存窗口。Data 挂载后切换到并发模式，USB 调试网络默认启用 TX 流控。[支持状态](STATUS.md) · [11 月 26 日结果](results-2026-11-26.md)
- 日常测试走内存启动，持久安装和分区维护遵循[持久启动文档](persistent-boot.md)。参考机的系统根位于 `linux_rootfs`，Data 位于 `linux_data`。固件、校准分区和所有 `_b` 分区保持只读。[参考机使用指南](USING.md)
- [`UniXNU-sm8750-common`](https://github.com/lenzhang/UniXNU-sm8750-common) 是骁龙 8 至尊版（SM8750）的**公共层占位仓库**。芯片通用代码暂时仍在 `UniXNU-piano` 里，稳定后再拆过来。注意：当前构建通过 `UniXNU-piano/feeds.toml` 钉住的是上游 [`IssacL04/UniXNU-sm8750-common`](https://github.com/IssacL04/UniXNU-sm8750-common)（OnePlus 13T 谱系）。这两个仓库不要混用，在拆分完成前不要改这个 pin。
- [`veil`](https://github.com/lenzhang/veil) 存放已于 2026-11-10 冻结的精简 Linux + Gunyah 客户机路线的**公开归档**。
- Ubuntu 只作为硬件行为与寄存器事实的参考。所有者于 2026-11-12 决定本机不再需要 Linux/Ubuntu；当前设备工作不继续这条安装路线，也不另建 Ubuntu 仓库。
- 权威备份在 NAS 上（具体路径只记在本机，不进公开仓库）。其他机器上的副本不是权威副本。
- 密码不要写进脚本、命令行参数或日志。厂商固件、分区镜像、Apple 内核 / KC / IPSW 派生产物不得进入公开仓库。
- 原生启动**只加载本仓库的 kext**：原生内核集合只链接本仓库 `package/` 里的驱动，VM 时期的半虚拟化 kext（显示、GPU、网卡、音频、输入、virtio 块设备等）一个也不链接，构建时由 `check-native-kexts.py` 强制检查（每次构建前先自检）。2026-11-11 的实测证据见 `docs/bringup-debugging.md` 第 5 节。
- GitHub 托管 CI 检查隐私、固定依赖及主机自测，包含 Adreno830 主机套件。依赖通过 [`scripts/fetch-pins.py`](../scripts/fetch-pins.py) 获取；另一台设备的构建和安装参见[安装指南](../skills/install-piano-macos/SKILL.zh-CN.md)。[CI 文档](ci.md)

---

## 1. Repositories and roles

| Repository | Role | Status |
| --- | --- | --- |
| [lenzhang/UniXNU-xiaomipad8pro](https://github.com/lenzhang/UniXNU-xiaomipad8pro) | UniXNU **device** repository for the Xiaomi Pad 8 Pro (`piano`, SM8750). Panel, touch, charging, keys, DTS/ADT, boot-image packaging, RAM-boot scripts, and device docs. Current **main line**: native macOS at EL1 (Gunyah remains at EL2). | Active |
| [lenzhang/UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) | SM8750 **SoC common layer** (CPU topology, GIC/AIC, memory map, UEFI SoC drivers, XNU SoC patches), following the UniXNU-sm8550-common / UniXNU-sheng split. | Placeholder (README + layout only). Code still lives in `UniXNU-piano` until the split is stable. |
| [lenzhang/veil](https://github.com/lenzhang/veil) | Thin Linux + Gunyah host / guest path (crosvm VM, macOS 27 guest on the Ubuntu host). | **FROZEN / ARCHIVED** as of 2026-11-10. Not the main line. |

Ubuntu work is **not** one of these three repositories:

- Distro / integration: upstream [yzddmr6/xiaomipad-8pro-mainline](https://github.com/yzddmr6/xiaomipad-8pro-mainline) (a public fork also exists at [lenzhang/xiaomipad-8pro-mainline](https://github.com/lenzhang/xiaomipad-8pro-mainline); send fixes upstream).
- Kernel: [yzddmr6/linux-sm8750-piano](https://github.com/yzddmr6/linux-sm8750-piano).

Keep Ubuntu work in its upstream projects. Keep build inputs, backups, and rollback packages outside the public repositories.

### What goes where (device vs SoC vs host)

| Kind of change | Goes in |
| --- | --- |
| piano panel, touch, charger, buttons, board identity, `dts/piano.dts`, ADT, `board.toml`, RAM-boot / packaging scripts, device docs and logs | `UniXNU-piano` |
| SM8750 CPU topology, GIC, memory map, shared UEFI/XNU SoC drivers and SoC patches | Today: still in `UniXNU-piano`. Later: `UniXNU-sm8750-common`, then pin `feeds.toml` at the new repo |
| Frozen Gunyah / crosvm / thin-Linux guest path | `veil` (public curated archive) |
| Ubuntu userland, flashing docs, mainline kernel fixes | Upstream `yzddmr6/xiaomipad-8pro-mainline` and `yzddmr6/linux-sm8750-piano` |

**Do not confuse the two SM8750-common repositories.** `UniXNU-piano/feeds.toml` and `unixnu.lock` currently pin [IssacL04/UniXNU-sm8750-common](https://github.com/IssacL04/UniXNU-sm8750-common) (commit recorded in the lockfile) as the SoC module used to *build* piano, originally extracted for the OnePlus 13T. `lenzhang/UniXNU-sm8750-common` is our future extracted common layer. Leave the pin alone until a real split lands.

## 2. Upstream and pinned dependencies

Pins live in [unixnu.lock](../unixnu.lock) and [feeds.toml](../feeds.toml). Keep device and SoC deltas in this checkout's `firmware/patches/`, `patches/`, `dts/`, and `configs/`.

| Project | Upstream | Notes |
| --- | --- | --- |
| UniXNU core | [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU) | CLI, firmware/payload/bootimg, UniXNUPkg, macos-27.3 profile |
| SoC module currently used to build | [IssacL04/UniXNU-sm8750-common](https://github.com/IssacL04/UniXNU-sm8750-common) | Temporary pin; see above |
| SM8550 reference split | [UniXNU-sm8550-common](https://github.com/UniXNU/UniXNU-sm8550-common) + device `UniXNU-sheng` | Model for the future SM8750 split |
| UEFI / BootShim | [UniXNU/uefi-sheng](https://github.com/UniXNU/uefi-sheng) | BootShim.S |
| UEFI platform | [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) | pianoPkg is composed on top |
| piano UEFI package | [xiaomi-pad-8-pro-development/android_device_xiaomi_piano-uefi](https://github.com/xiaomi-pad-8-pro-development/android_device_xiaomi_piano-uefi) (branch `unixnu`) | `pianoPkg`; our deltas in `UniXNU-piano/firmware/patches/` |
| DarwinBoot | [UniXNU/DarwinBoot](https://github.com/UniXNU/DarwinBoot) | XNU loader in the FD |
| k27 source tree | [Droid-VM/macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) | Reconstructed from the lockfile pin plus [this repository's k27 tree delta](../patches/k27-tree/README.md). The build reads it through `PIANO_K27_TREE`. |
| Ubuntu / mainline | [yzddmr6/xiaomipad-8pro-mainline](https://github.com/yzddmr6/xiaomipad-8pro-mainline), [yzddmr6/linux-sm8750-piano](https://github.com/yzddmr6/linux-sm8750-piano) | Send Ubuntu fixes here |

[`scripts/fetch-pins.py`](../scripts/fetch-pins.py) fetches the lockfile's checkouts by commit, including the reconstructed
`k27-tree`. `--verify` checks that the pinned sources can be fetched. [CI pin checks](ci.md#checksyml)

## 3. Checkout layout and host checks

| Path | Role |
| --- | --- |
| `patches/k27-tree/`, `patches/k27/`, `patches/k273/` | Pinned k27 tree delta, native kernel patches 0053–0076, and 27.3 kernel patches. [Kernel patch index](../patches/k27/README.md) |
| `package/`, `configs/`, `dts/`, `firmware/` | Native kexts, board and profile inputs, and firmware deltas. [Driver index](drivers/README.md) |
| `tooling/k273/`, `tooling/persist/`, `tooling/repro/` | 27.3 image and ESP builders, persistent-install tools, and host checks for reproducing builds. [27.3 line](k273-line.md) |
| `.github/workflows/`, `scripts/ci/` | GitHub-hosted privacy, pinned-source and host self-test checks. The Adreno830 host suite stages the pinned public ArmSmmu4K, AdrenoPM4 and AdrenoFirmware kits. Monthly dependency checks, builds and draft releases use separate workflows. [CI and releases](ci.md) |
| `skills/install-piano-macos/` | Second-unit guide for host preparation, building, RAM qualification, guarded install and rollback. [Install guide](../skills/install-piano-macos/SKILL.md) |

Keep build outputs, signing keys, vendor firmware, partition dumps, and Apple kernel collections and IPSW-derived payloads out of Git.

## 4. Backups

The owner's NAS holds the authoritative backup. Keep backup locations and inventories local. The persistent-install procedure
requires verified backups and read-back of changed regions. [Install design and rollback](persistent-boot.md)

## 5. Commit and push conventions

- Commit device work in `UniXNU-piano` (the local tree on the owner's Mac). Prefer small, indicative subject lines in English (what changed, not “WIP”).
- The GitHub `main` branch of `lenzhang/UniXNU-xiaomipad8pro` is a **cleaned export**: home-LAN addresses and machine-local author emails were removed from history before the first public push. The original local `main` was not rewritten. Do not force-push the uncleaned local history to GitHub.
- Before every push to a public repository, audit the outgoing commits (worktree and history) for: passwords, tokens, keys, `private/`, SSH/NAS credentials, home-LAN IPs and hostnames, vendor firmware, partition dumps, Apple KC / IPSW payloads, and large `out/` artifacts. If a secret or blob is already in history, publish a cleaned export; never rewrite the original local repository.
- Push only to the three repositories listed above (plus Ubuntu **upstream**). Do not publish `unixnu-research/`, backups, or rollback packs.
- SoC-common code is not split yet. When it is, move it to `lenzhang/UniXNU-sm8750-common` in a dedicated change and then update `feeds.toml` / `unixnu.lock`.
- `veil` is frozen. Do not add new Gunyah-guest features there; only documentation or archive fixes, and only after the same audit.

## 6. Hard rules

1. **RAM boot for routine tests; partition writes only under the approved persistent-install plan.** Use `fastboot boot` for test
   images. The maintenance procedure of [`docs/persistent-boot.md`](persistent-boot.md) may write the documented ESP, `boot_a`, GPT
   and Linux-era `linux_rootfs` / `linux_data` regions after verified backups and read-back checks. Never write `xbl`, `abl`, `tz`,
   `hyp`, `vbmeta`, `dtbo`, `persist`, firmware/calibration partitions or any `_b` partition. Never run `flash_all_lock`.
2. **Never boot or flash `-signed` images** as a way to persist a new OS. Signed RAM images, if built, are a verification artifact only.
   (Exception: the signed `boot_a` image of `docs/persistent-boot.md`, under rule 1's exception.)
3. **Never `systemctl isolate`** on the tablet. It has stopped `piano-charging` and `piano-thp` and drained the battery / killed the touchscreen.
4. **No secrets in files, argv, or logs.** The tablet sudo password is typed by the user in tmux (or read from a mode-600 file *outside* every repository). Do not write passwords into scripts, commit messages, or `tmux` send-keys.
5. **No vendor blobs or Apple payloads in public repos:** Xiaomi / Qualcomm firmware, partition dumps, IPSW-derived kernelcaches, trust caches, `.img` / `.fd` / `.kc` / `.ipsw` build outputs. Record SHA-256 in `artifacts/ARTIFACTS.md` instead.
6. **No home-LAN inventory in public repos:** private LAN addresses, the owner's machine and NAS hostnames, NAS backup paths, and machine-local author emails. Local working copies may keep operational defaults; public history must not.
7. Push only to the repositories named in this document (and Ubuntu upstream).
8. **Only this repository's kexts in a native boot (the native kext rule).** The native kernel collection links only this
   repository's kexts (descriptors in `tooling/k27/kext/`, sources in `package/`, objects in `out/native-kexts/`, the set in
   `configs/k27-native-kexts.txt`). None of the VM-era paravirtual kexts (display, GPU, Ethernet, audio, video, input, virtio
   block) is linked, not even for one test. VM-era code that native piano needs is taken over as this repository's own package
   (with `UPSTREAM`), never by linking the VM-era kext. `tooling/kc/build-kc-k27.sh` enforces this
   (`tooling/k27/check-native-kexts.py`: a self-test of its denylist, then the descriptors before the link and the collection
   after it), and `tooling/persist/build-esp.py` refuses an ESP whose collection has a VM-era bundle. Evidence, the boot
   arguments still read under legacy names, and the ADT identity: `docs/bringup-debugging.md` §5.

## 7. Current bring-up state (2026-11-27)

- **Installed daily system.** v13.1y12q booted from disk on 2026-11-26. The native 27.3 line runs as Mac17,11 at EL1 with
  Gunyah at EL2. The k273 build uses k27 patches 0053–0076 and the 27.3 kernel patches. The installed 13 GiB memory window
  measured 13310 MiB on v13.1y12l. [27.3 line](k273-line.md) · [November 26 results](results-2026-11-26.md)
  [v13.1y12u daily combination audit](y12u-daily-combo-audit.md)
- **Disk and USB.** The v5 BootShim and FD are on `boot_a`, with the UniXNU payload on the ESP. The 27.3 root is on
  `linux_rootfs` and the 200 GiB Data store is on `linux_data`. Data switches to concurrent mode after mount, with write-back
  and queue modes off. The USB debug network enables TX flow control by default (66/66 exact transfers in qualification).
  Patch 0076 provides the full merged boot line to the USB shell and other updated helpers. [Installed profile](USING.md#2-the-installed-profile)
- **Hardware.** The installed profile has 60 Hz display, native touch, the keyboard cover and trackpad, built-in Wi-Fi `en0`,
  speakers, automatic brightness, 5 V and 9 V PD buck charging, and accelerated Adreno 830 Metal. Fixed 120 Hz and 144 Hz
  have RAM scanout results. Night Shift routing, native panel sleep and refresh switching are off on the daily build.
  [Support status](STATUS.md) · [Hardware backlog](BACKLOG-hardware.md)

### Historical bring-up (2026-11-14)

The 27.3-only ESP replaced the dual ESP, and the old 27.0.1 volume and store were cleared. The root then moved to
`linux_rootfs`, and the existing Data store grew to 200 GiB on `linux_data`. Early Wi-Fi tests took the level-8 data path
through DHCP, DNS and HTTPS on `en2` before the RAM test handed the built-in `en0` to Wi-Fi. [Install design](persistent-boot.md) §13.8 · [November 14 results](results-2026-11-14.md)
