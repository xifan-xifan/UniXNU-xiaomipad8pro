# 2026-11-26 - 内核找 owner Data 卷只认 IOMedia 的 UUID：实现在 `IOKitBSDInit.cpp`，不在 NativeVirtioBlock；按上游 `--volume-uuid` 造了 `8748FE31…` 的 seed

## 判据链：谁在回答 `native_ram_block_volume_seed`

`kern_exec.c:8681` 的 `panic("offline scratch Data mount failed: %d", data_error)` 只有一个来源：`native_owner_data_prepare()`
（`bsd/kern/kern_exec.c:8450`）里 `native_ram_block_volume_seed(XNU_OWNER_DATA_VOLUME_UUID, ...)` 返回 0 的那一支（8460，先写
`error = ENOENT` 再 `goto done`）。`XNU_OWNER_DATA_VOLUME_UUID` 没被编译命令覆盖，取 8272-8274 的默认值
`8748FE31-7E7F-4BFA-A7E2-7A4F5E14D0F6`。往下：

1. `osfmk/kern/link_compat.c:74-85`（port 自己的兼容 TU）定义 `native_ram_block_volume_seed`，转调
   `native_verified_block_volume_device`；`nm osfmk/DEVELOPMENT/link_compat.o` 里后者是 `U`，所以定义在另一个 TU。
2. 全树只有两处候选定义：`overlay/droidvm/drivers/NativeVirtioBlock/NativeVirtioBlock.cpp:429`（kext 源码，要 provider 链上
   `NativeVirtioBlock`/`native_ufs_block_authenticates` 且签名策略的 `volume_uuid` 相等）和
   `iokit/bsddev/IOKitBSDInit.cpp:1988`（`#if XNU_OWNER_CACHE_MOUNT`，只做一次注册表匹配）。
3. 编进内核的是后者，反汇编为证：`xcrun objdump -d --start-address=0xfffffff00c27669c`
   （`out/k27/kernel-k273-0075-s0001-native.macho` 里 `_native_verified_block_volume_device`）的调用集是
   `IOService::serviceMatching`、`IOService::waitForMatchingService(OSDictionary*, uint64_t)`、`OSDictionary::withCapacity`、
   `OSString::withCString`、`safeMetaCast`、`_snprintf` —— 正好是 IOKitBSDInit 那份；NativeVirtioBlock 那份要调
   `getMatchingServices`、`makedev`、`strlcpy`、`native_verified_block_root_authorized`，一个都不在。

所以内核对 Data 卷的全部要求是：在走到这一步的 60 秒内，注册表里出现一个 `UUID` 属性等于该串的 `IOMedia`，取它的
`IOBSDNameKey/Major/Minor` 拼成 `/dev/…` 去 `mount("apfs", "/System/Volumes/Data")`。**不查签名策略、不看 provider 类别、不判唯一性。**
上游注释本来就这么写（`link_compat.c:70-73`：fork 里这活由 NativeRamBlock 驱动回答，本 port 把驱动放作 kext，于是按 IOMedia 的
UUID 找，像 SystemOS 那样）。

## 两条要纠正的记录

- 在 NativeRamBlock kext 里加 census/sweep 想验"内核调的是哪份实现"不可能有输出：内核的 `native_ram_block_volume_seed` 在自己的
  链接里就绑定了，kext 里的同名符号不参与内核内部调用。Mac 上 `upstream-tip` 的该文件已 `git checkout --` 还原（诊断副本
  `/private/tmp/NativeRamBlock-diag-backup.cpp`，上游原版 `/private/tmp/NativeRamBlock-upstream.cpp`）。
- "在 seed 里字节搜 `8748FE31` 得 0 次"不能当证据：APFS 超级块的卷 UUID 是 16 原始字节，`diskutil apfs list`/`fsck_apfs` 都按二进制读，
  ASCII 搜索必然 0 次。可靠读法是解开 packed seed 成 raw、`hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage`、
  `diskutil apfs list`。

## 缺的就是一个 UUID 对得上的卷：用上游工具补

UniXNU `d830d23` 的 `scripts/make-macos-data-seed.py` 早就为此留了口子（docstring：`--volume-uuid` gives the Data volume the UUID
the kernel mounts (CONFIG_XNU_OWNER_DATA_VOLUME_UUID), so a new seed can replace an older one without a kernel build；实现是同文件
`set_volume_uuid()`，逐个带校验和的超级块副本改 UUID 并用 `fletcher64` 重算）。新发布的 k27 树同一件事写在
`scripts/make-data-seed.sh:18` 与 `make-macos-data-seed.sh` 的 `VOLUME_UUID=${VOLUME_UUID:-8748FE31-…}`。

本机输入（全部现成，未下载）：把 `out/inputs/k273/root/root.img` 的 OS 窗口切出来当系统卷模板源
（`root.json` 记 OS 窗口 offset 1048576、size 14493417472 → `dd bs=1m skip=1 count=13822`，22 s），只读挂载后
`sbin/launchd`、`usr/share/firmlinks`、`System/Library/Templates/Data` 三件齐；packer 用钉版 DarwinBoot `8544957`
（`PYTHONPATH`），root 权限用 Mac 的免密 sudo。命令（Mac `/private/tmp/build-seed8748.sh`）：

    sudo env PYTHONPATH=.../DarwinBoot/Test python3 scripts/make-macos-data-seed.py /private/tmp/osmnt \
        /private/tmp/seed8748/scratch-data.img --gib 24 --skip-setup --user piano:<见脚本内> --auto-login \
        --volume-uuid 8748FE31-7E7F-4BFA-A7E2-7A4F5E14D0F6

（口令只存在那台 Mac 的脚本里，不写进仓库；`--auto-login` 只为本轮无人值守，与上游文档一致：unattended test seeds only。）

## 离线验收（只在 Mac，未上机）

- `diskutil apfs list`：`Volume disk7s1 8748FE31-7E7F-4BFA-A7E2-7A4F5E14D0F6`，名字 `Data`（Case-insensitive，和 macOS Data 卷一致），
  容器 `Physical Store disk6`，用量 18.7 MB。
- `fsck_apfs -n`：`The volume /dev/rdisk7s1 with UUID 8748FE31-… appears to be OK`、`The container /dev/rdisk7 appears to be OK`。
- 骨架齐（`/private` firmlink 是 `d830d23` 的硬要求）：顶层 `Applications Library System Users Volumes cores opt private usr …`，
  `private/var` 下有 `db log tmp folders …`；`private/var/db/.AppleSetupDone` 在；`dslocal/.../users/piano.plist` 在（133 个账户文件），
  `/Users/piano` 家目录在；`Library/Preferences/com.apple.loginwindow.plist` 里 `autoLoginUser => piano`。
- 产物：`/private/tmp/seed8748/scratch-data.img` 20,992,144 B，file sha256 `fc86cb2510e345749d26321b061ba374c2bf711bf865308cb03d8c860ea2507c`，
  seed identity `8e1a87e1d588896064b3b3b88b9f3308d21201482bd8559e36f4c5213ff1ab54`（`tooling/persist/common.py:seed_info` 口径）。
  逻辑 25,769,803,776 B、pages 6291456、store_blocks 6291649 —— 与旧 seed（identity `7e9bd39b…`）完全相同，所以本机 store 窗口
  仍是 `117735999`，不用重画几何。

## 装配与待跑

`out/inputs/k273-8748/{root,kc}`：新 seed 是真文件，`root.json`/`kernel.macho.trustcache`/`kc273-mac17j.bin` 是指向既有资产的软链
（不复制、不覆盖旧 seed，`out/inputs/k273/root/scratch-data.img` 保持原样）。构建脚本 `out/build-verified-8748.sh` =
`out/build-verified-store.sh` 加两处：`K273_INPUTS` 指向新目录，`PIANO_DATA_STORE_REPLACE=1`。后者是必须的：盘上的 store 绑在
identity `7e9bd39b`，新 seed 是 `8e1a87e1`，_store guard 会"kept + Data volatile"_；`data-store-replace=1` 让窗口为新 seed 重开。
本机出厂即 3.0.309 且任务开始前已恢复出厂，盘上没有机主 Data（用户已明确"平板上已经没数据了，随便刷"）。

判读顺序（下一轮 RAM）：`NativeRamBlock: Data store state 1/3`（`seed 8e1a87e1`）→ 卷挂载 → `k27: firmlink …` → `owner cache`
stage 6 → launchd → 桌面。本轮仍 `PIANO_GPU=0`、`PIANO_RESET_AFTER=420`。**未在平板上运行过**：以上均为 Mac 离线结论。