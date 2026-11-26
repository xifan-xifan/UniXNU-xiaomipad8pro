# 2026-11-26 - owner Data 挂载过了（store 新建 + firmlink + launchd 起来），停在新位置：`mount-phase-1` 要根容器的 VM 卷

镜像 `24656635efebd9710eefbb179811085d0c3fd3a2488d92dabfde98a93b21d66a`（159,686,656 B；Mac `out/ram-verified-8748/`，
Windows `out\k273-ram-8748\`，UEFI 侧仍 `eb570025…`）：`c0003910…` 那条装配，只换 seed（`8e1a87e1…`，卷 UUID
`8748FE31-…`，见 [按 UUID 找卷](results-2026-11-26-kernel-finds-data-volume-by-uuid-only.md)）并加 `data-store-replace=1`。
运行 `logs/native-runs/ram-k273-8748-20261126-225510`：22:55:30 `fastboot boot`，22:57:56 回 ABL（151 s，比镜像内的
`piano-reset-after=420` 早，因为下面是主动 panic），dump rc=0、7,340,032 B（非零 1,587,928），多数表决解码仍退化到 3/11，
按 `tooling/native/pstore-strings.py` 读。HyperOS 22:59:26 自己回来了。

## 过了什么（逐条原话）

```
NativeUfs: SystemOS volume 6678E466-2F88-4C56-9408-DC7361BAA0BA ready
NativeUfs: published SystemOS
NativeRamBlock: data-store-lba=117735999: window at LBA 117735999, store new, bound to it
NativeRamBlock: Data store state 3 (1 blank, 2 resumed, 3 fresh, 4 failed, 7 kept), header 3,
    first LBA 117735999, 6291456 pages, direct 1, seed 8e1a87e1, store seed 8e1a87e1
NativeRamBlock: the store was started over on request (data-store-replace or seed-data-reset)
k27: firmlink /private -> /System/Volumes/Data/private: 0
k27: firmlink /Applications -> /System/Volumes/Data/Applications: 0
k27: firmlink /Library -> /System/Volumes/Data/Library: 0
k27: firmlink /cores -> /System/Volumes/Data/cores: 0
k27: firmlink /opt -> /System/Volumes/Data/opt: 0
k27: firmlink /System/Library/Caches -> /System/Volumes/Data/System/Library/Caches: 0
k27: firmlink /System/Volumes/Hardware -> /System/Volumes/Data/System/Volumes/Hardware: 2
load_init_program: attempting to load /sbin/launchd
com.apple.xpc.launchd <Notice>: hello, launchd UUID: 96360FC3-2FDE-3EEE-A403-7DFCA7B44AC3
com.apple.xpc.launchd <Notice>: boot-mode set by sysctl: main
```

`offline scratch Data mount failed: 2` 从此消失：owner 链走到底（root 授权 → SystemOS 卷 → 按 UUID 找 Data 卷 →
读写挂载 → staged apps → firmlink → GPU addon），然后 exec 了 launchd，boot tasks 开始跑。
`/System/Volumes/Hardware` 那一条是 2（ENOENT）：seed 里没有这个目录，firmlink 允许缺项（其余全 0），不影响后面。
store 侧第一次真正建成：窗口 `117735999`、6,291,456 pages、identity `8e1a87e1` 与 store 记的一致（state 3 全新）。

## 停在哪：`mount-phase-1`（`apfs_boot_util 1`）SIGABRT

```
mount_by_role:451: failed to find volume with role = 8 in APFS container with volcnt = 0
    for root device /dev/disk3s1 with status 0924
wait_for_node:266: APFS IOKit node lookup timed out...      （重试 10 次，约 30 s）
get_apfs_node:351: Timed out waiting for node with role 0 on 52637672-7900-01AA-AA11-00306543ECAC
mount_vm:692: No VM volume on /dev/disk3s1, creating
create_vm_volume:641: Cannot create VM volume in /dev/disk3 - Read-only file system
mount_vm:701: Cannot create new VM volume in APFS container /dev/disk3s1 - error 49182 (errno 30)
mount_phase_one:1887: VM volume mount failed error 4924 (errno 30)
AMFI: Denying core dump for pid 4 (apfs_boot_util)
com.apple.xpc.launchd <Error>: (mount-phase-1) exited due to SIGABRT
com.apple.xpc.launchd <Emergency>: Boot task failed: mount-phase-1 - exited due to SIGABRT
com.apple.xpc.launchd <Emergency>: Panicking in 3 seconds.
```

phase one 的职责是把根容器的角色卷挂上（VM、Preboot、Update、xART、Hardware），VM 缺失就**现建**；我们的根容器是只读的，
只有 System 卷（而且 SystemOS 联合挂载后 `/` 来自 cryptex 那条窗口，所以它盯上的容器是 `disk3`），于是建 VM 卷必失败，
`RequireSuccess` 的 boot task 直接把机器 panic 掉。这与内核无关：launchd 已经正常起来了（`exclave-boot`、`mount-phase-1`
都在跑），是用户层的角色卷假设与本布局冲突。

## 已有出路与本树缺的东西

- UniXNU 钉版 `d830d23` 有正解：`scripts/macos-boot-shims/apfs_boot_util.c`（作者替身，注释原文："the root container
  is the unchanged, owner-verified OS image: it holds the System volume only and cannot be written, so creating the VM volume
  fails and launchd stops the boot … Every phase therefore has nothing to mount and succeeds. The kernel shows this file over
  the original only with `apfs-boot-shim=1`"），`scripts/build-boot-runtime.py:build_boot_shims` 把它 ad-hoc 签名
  （`--pagesize 4096`，id `org.unixnu.apfs_boot_util`）打进 addon 的 `bootshim/`。
  但新发布的 k27 树里没有 `apfs-boot-shim`/`bootshim` 任何痕迹（`grep -rn` 覆盖 `patches` 与准备好的 xnu 源码），
  也就是本 port 的内核不提供这个覆盖；走这条路要先补内核补丁，属于新补丁，暂不作首选。
- `docs/boot-time.md` §4 已记：`droidvm-disable-jobs` 在本机线**完全无效**（读它的内核逻辑不在本仓库），所以不能靠它跳过任务。
  而 `mount-phase-1/2`、`exclave-boot` 是 launchd 内建 boot task：OS 卷的 `System/Library/LaunchDaemons` 426 个 plist 里
  没有一个匹配（在 Mac 上对已挂载的 26C5035r 系统卷 grep 验证），所以 seed 的 `disabled.plist` 也管不到它们。

## 本轮要试的两个纯配置口子（不改代码、不新打补丁）

1. **ADT `/chosen/osenvironment` 由 `main` 改 `darwinos-ramdisk`**：`bsd/kern/kern_csr.c:124`
   `_csr_is_darwinos_ramdisk()` 就是比较 `chosen` 里这个字符串；`apfs_boot_util` 读 `hw.osenvironment`，符号表里有
   `boot_is_darwinos_ramdisk`，串里有 `os environment is darwinos-ramdisk, skipping ExclaveOS grafting`。
   变体 `out/build-verified-8748-ramdisk.sh` + `configs/k273-adt-ramdisk.toml`（同一文件的副本，只改这一行）。
2. **`PIANO_DENY_EXEC=/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_boot_util`**（`k27-deny-exec=`）：
   替身根本不被 exec。待验的是 `mount -P 1` 对 exec 失败怎么处理（`mount` 的串里有"非 APFS 就跳过"和"执行失败"两条不同分支）。
   变体 `out/build-verified-8748-denyexec.sh`。

## 装配上的两处修正（已提交）

- 主镜像建好之后，`build-native-image.sh` 还要为 out/ram-dump 再跑一次 `build-ram-images.py`，那次固定
  `PIANO_DATA_PERSIST=0`，而 `data_store_args()` 见到 `PIANO_DATA_STORE_REPLACE` 就按设计拒绝（dump 镜像不该重置盘上 store）。
  修法是那次调用不带这两个变量：`tooling: build the dump image without the store-replace request`（`d941d2f0`），
  Mac 的 `upstream-tip` 同步改了同一行。
- `tooling/k273/build-k273-image.sh` 里 `PIANO_ADT_IDENTITY` 改成 `${PIANO_ADT_IDENTITY:-configs/k273-adt.toml}`，
  这样换 ADT 身份只要给环境变量。

桌面、GPU、输入、网络、音频、Data 写后重启读回、Android 恢复、双向切换：仍未开始验收。