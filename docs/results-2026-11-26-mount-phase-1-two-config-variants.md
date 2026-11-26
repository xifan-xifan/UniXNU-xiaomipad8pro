# 2026-11-26 - `mount-phase-1` 的两个配置口子都试过：ADT 换 `darwinos-ramdisk` 不改 phase 1 的建卷分支；deny-exec 掉 `apfs_boot_util` 让 `mount` 自己 `exit 77`

两个变体都从 `24656635…`（[上一轮](results-2026-11-26-k273-owner-data-mounts-launchd-stops-at-mount-phase-1.md)）派生，
只多一个变量，构建脚本 `out/build-verified-8748-ramdisk.sh` / `out/build-verified-8748-denyexec.sh`。

## 变体 A：ADT `/chosen/osenvironment = darwinos-ramdisk`

镜像 `51321d79601bfdaa3e8648d1bcf416581f98aa834dccab63d60c6f624867e8e9`，运行 `logs/native-runs/ram-k273-8748rd-20261126-232105`。

生效了：`com.apple.xpc.launchd <Notice>: boot-mode set by sysctl: darwinos-ramdisk`（原来是 `main`）。
但 `apfs_boot_util` 里读 `hw.osenvironment` 的那一处只挡 ExclaveOS grafting（串：
`os environment is darwinos-ramdisk, skipping ExclaveOS grafting`），不管 VM 卷，于是同一处死：
`mount_vm:692: No VM volume on /dev/disk3s1, creating` → `mount_phase_one:1887: VM volume mount failed` → SIGABRT，
152 s 回 ABL。firmlink 与 store 与上一轮一致（全 0 + state 3）。

## 变体 B：`k27-deny-exec` 加上 `apfs_boot_util`

镜像 `a67cb2ff2d4da50b0795ea779ae6489e978cf584bcfc18ca7317eb4adfe901cb`，运行 `logs/native-runs/ram-k273-8748deny-20261126-233017`。
命令行里确认带上了（`build-A-bundled.json` 的 `cmdline_8cpu`）：
`k27-deny-exec=/usr/libexec/mobileactivationd,/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_boot_util`
（`kern_exec.c:7490` 是 `TUNABLE_STR(..., 256, ...)`，两个路径放得下；`k27_exec_denied()` 比的是 `vn_getpath` 全等）。

结果：替身确实没起来，但 `mount` 把 exec 失败当致命错：

```
mount: apfs_boot_util exec failed
pid 4 exited - no exit beacon available -- (signal 0, exit 77)
com.apple.xpc.launchd <Error>: (mount-phase-1) ... / <Emergency>: Boot task failed
reboot: pid 1 launchd ... message boot task failure: mount-phase-1
```

104 s 回 ABL（比 152 s 早，因为少了那 33 s 的角色卷重试）。所以 `k27-deny-exec` 不能当"跳过 phase 1"用：
它只让 `mount` 更快失败，`RequireSuccess` 一样 panic。

## 现场勘查：SystemOS 里本来就有 `mount -P` 的替身，但实机跑的仍是 Apple 的那份

`out/k273/k27-tree/overlay/systemos-shims/sbin/`：

    sbin/fsck    #!/bin/sh  → exit 0                     （17 B）
    sbin/mount   [ "$1" = "-P" ] && exit 0; exec /sbin/mount.real "$@"   （358 B）
    sbin/mount.real = Apple mount（206,992 B）

注释原话：launchd 的 `mount-phase-1/2` 跑 `mount -P N` 会把只读的根容器读写挂上去找 VM/Preboot/Data 角色卷，
而内核自己挂 Data，于是 phase 直接成功，别的参数转给 `mount.real`。

在 Mac 上只读挂 `out/inputs/k273/root/systemos.img` 核对：卷根 `sbin/fsck`(17) / `sbin/mount`(358) / `sbin/mount.real`(206,992)
**都在**。也就是说上游设计里 `mount -P N` 根本不该走到 `apfs_boot_util`。而本机这几轮的日志显示走到了：
`mount-phase-1` 的 pid 4 是真正的 Apple `mount`（它 fork/exec 了 `apfs_boot_util`），并且 `fsck` 也是真的在跑
（日志里有 `** Checking the checkpoint...` / `CHECK ONLY; FILESYSTEM CLEAN`）。合理解释是这一层 `/sbin` 看到的是 OS 窗口那层，
SystemOS 的替身没盖住它；ADT 这边没写错（`out/adt/piano-k27.adt` 里 SystemOS 的 UUID 就是镜像真实的
`6678E466-2F88-4C56-9408-DC7361BAA0BA`，`out/inputs/k273-stock/root/root.json` 也是这个；上一轮记的
`A8D7C405…` 是 `out/inputs/k273/root/root.json` 那份旧装配，不是本轮在用的）。

## 下一轮第一个实验（能同时当判据和修法）

`PIANO_DENY_EXEC=/sbin/mount.real`（只禁 `mount.real`，不禁 `apfs_boot_util`）：

- 若 SystemOS 的 `/sbin/mount` 替身真的在生效，`mount -P 1` 会在 `exit 0` 那行返回，永远碰不到 `mount.real` →
  `mount-phase-1` 直接通过，桌面往前一大步；
- 若替身没生效（本几轮的观察），行为与现在一样，`apfs_boot_util` 照跑 → 结论是"根 `/sbin` 取的是 OS 层"，
  修法就要落在让 SystemOS 层覆盖 `/sbin`（或按 UniXNU `d830d23` 的 `apfs-boot-shim=1` 补内核覆盖，属新补丁）。

两种结果都有用，且都是一次构建 + 一次 RAM 启动。

## 记两件事

- 变体 A/B 的镜像在 Mac `out/ram-verified-8748-ramdisk/`、`out/ram-verified-8748-denyexec/`，Windows
  `out\k273-ram-8748-ramdisk\`、`out\k273-ram-8748-denyexec\`（各带 `.sha256`）。UEFI 侧仍是 `eb570025…`。
- 新 Data seed 的原料已就位并保留：Mac `/private/tmp/seed8748/scratch-data.img`（`fc86cb25…`，identity `8e1a87e1…`）；
  本轮临时切的 14.5 GB 系统卷镜像与解包的 raw 已删，三个 hdiutil 挂载全部卸载。
  下次重造 seed 前需要重切系统卷（`dd bs=1m skip=1 count=13822` 自 `out/inputs/k273/root/root.img`）。