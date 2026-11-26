# 2026-11-26 - 复核设备侧记录：27.3 根早已写进 linux_rootfs，Data store 的空白是设计；私有头目录也可复现

本轮没有任何写入，也没有重启设备。全部结论来自既有证据的复核：`out/persistent/session/session.log`、`out/persistent/session/state.json`、
本机重算的镜像首块哈希，以及 WSL 发行版 `UbuntuPiano` 里的构建现场。三条被本轮推翻的记录写在前面，因为它们直接决定下一步做什么。

## 一、装机盘上已经有 27.3 根：上一轮"根从未写过"是错的

`session.log` 里 11-24 22:44:31（`RUN: map`，维护 RAM 镜像只读）原文：

```text
ok    linux_rootfs block 0 is the SHNGROOT manifest (the root of esp-device.img): 17547919360 bytes inside the partition
```

同一份日志 23:07:45、23:10:00 两次重复同一结论，`state.json` 的 `root_manifest` 也记着 `partition=linux_rootfs`、
`sha256=d286c86cd939916455d9e09f6b0141d523ad6c7b5ef611ccc73447765ab40b23`、`image_bytes=17547919360`。本机对
`out/piano-stock-root-26C5035r.img` 的头 4096 字节重算：`SHNGROOT` 魔数、自声明大小 17,547,919,360、sha256
`d286c86cd939916455d9e09f6b0141d523ad6c7b5ef611ccc73447765ab40b23` —— **与设备读回的第 0 块逐字节相同**。
那次会话里没有 `piano-309-root` 的真实写入记录（该步骤只有 dry run），所以写入发生在更早的会话，日志已被 11-24 的会话覆盖；
但设备介质本身就是权威，不需要日志来证明。

后果：`docs/results-2026-11-26-root-image-and-esp-line.md` 里"ESP 要挂载根的那个分区这台盘上从来没写过东西"以及
"下一步是按 profile 写根镜像"**都不成立**，那一份写计划（`out/unit-profile/piano-7ccdd855-root-on-rootfs.json`）留作校验用，
不再当作待执行动作。`tooling/persist/maint-session.py` 放宽 `linux_rootfs` 写门槛（8509e8d4）仍然正确，但对本机不再必要：
`map` 读到的第 0 块不是全零块，而是我们这份镜像的 manifest，走的是"先备份再 clear"那条既有门槛，也不会被误放行。

## 二、`linux_data` 第 117735999 块的空白 store 是设计，不是故障

同一份日志 11-24 22:44:34 原文：

```text
ok    the store window at LBA 117735999 on linux_data is blank: the first data-persist=2 boot creates the 6291649-block store for seed 7e9bd39b72da851d there
```

于是 `state.json` 里 `store_matches_esp: false` 与 `esp_store.header.state = blank`（`fresh: true`）是那次 `map` 的正常结论：
store 由**第一次 `data-persist=2` 启动**创建，不是事先写进去的。HANDOFF 里"117735999 处 store 必须是 seed `48dbc08d…` 那份"
混淆了两个不同的东西：`48dbc08d5955a20255268aadc3f79660a62c395a68b358e349bbc3ea8f8b25e0` 是 initrd `scratch-data.img` 的
文件哈希（G3 里我们用它替换参考机 seed pin `cb50ceac…`），`7e9bd39b72da851d` 才是 `map` 报出的 store seed 身份。
两者都记在这里，验收时按 `map` 的 seed 身份判读。

## 三、三个"私有头目录"是可复现的：`e879aa64` 的结论作废

`e879aa64` 记的"两个公开源都不是 `storage-abi`，`xnu-m` 只是命名别名"只对了一半，因为它是在当前工作区和上游里找的，
而不是在产生这些目录的那台机器上找。WSL 发行版 `UbuntuPiano` 里的现场：

- `/root/piano-native/k273-compat/out/x12377/storage-abi` 是指向
  `UniXNU/out/piano/macos-27.3-26C5035r/piano_macos/tree/build/storage-abi` 的符号链接，由 `scripts/prepare-storage-headers.py`
  （sha256 `e7015024faa4dfdca5e596b7ca73c85efca07309e448613120a5a0a86f377cef`）从钉死的公开 feed
  `out/feeds/IOStorageFamily-a4f5240a`（origin `apple-oss-distributions/IOStorageFamily`，HEAD `7edb88fbae296fb7c8ce2f64e115e116e566d51c`，
  工作树干净）生成；
- `xnu-m -> /root/piano-native/xnu`（本机 xnu 源树），`inc-gfx -> /mnt/e/codex/2026-11-01/ni/work/external/IOGraphics/IOGraphicsFamily`
  （Windows 侧历史工作区），`inc-hid`、`inc-net-piano` 是同族**真实目录**，不是符号链接。

在 Mac 上复现：feed 从 WSL 导出 + 上述脚本 → `python3 prepare-storage-headers.py --source feed --revision 7edb88fb… --output out`
→ `PASS: 19 public IOStorageFamily headers prepared`，与本机制定的 19 个 `headers.json` 比对 **19/19 逐字节相同**；
唯一差异是 `TargetConditionals.h`（本机 SDK `af113bd4…` 对留存 `2dbd9a68…`，留存那份取自该树 `.cache/fakeroot`）。
复现物已放进 Mac 的 k27 树 `out/k273/k27-tree/out/x12377/storage-abi/include`，`xnu-m` 按同一命名接好。
把 `inc-hid`、`inc-net-piano` 也接上之后，`tooling/k27/build-native-*.sh` 全量跑 **PASS=20 / FAIL=10**，产出 128 个对象 23 MB；
剩余失败是环境缺件（`gen-piano-gpu-inputs.py` 的 f-string 需要 Python 3.12、`kc27.bin`、`ffprobe`），不是头目录缺失。
所以"原生 kext 对象只能靠维护者手写 ABI"这句话不再成立，自建内核集合这条路没有硬阻塞。

## 四、当前真正的阻塞（复核之后重排）

装机链条本身是齐的：候选 GPT、装机 ESP（`esp-device.img` sha `68c28df15bcf55a5…`，`[boot] default = "macos"`，
`native-ufs-root-part=linux_rootfs`、`data-store-part=linux_data`、`data-store-lba=117735999`、`data-persist=2`）、
`linux_rootfs` 里的 27.3 根、`boot_a` 上 11-25 已回退成 HyperOS 三元组（`slot-successful:a yes`、`slot-retry-count:a 6`）。
缺的是把原生启动推过存储发布那一步：`docs/results-2026-11-25-nativeufs-start-static.md` 与
`docs/results-2026-11-25-ufs269-base-kernel.md` 停在 `IOBlockStorageDriver::start` / `acceptNewMedia`，
根因是基座内核没有可用的定时器/GIC 中断路径，而 `package/NativeUfs/NativeUfs.cpp:2904` 起的发布序列整段是等待式
（`waitForMatchingService` 60 s、`IOSleep(100)` 轮询 300 次），`docs/results-2026-11-23-native-ufs-root-window.md` 记着中断使能前
`IOSleep`/`IODelay` 永不返回。因此下一步产物按 11-25 的更正清单：把带 `piano_gic*` 的 ufs266 谱系与上游 `NativeUfs` 对象合成一个集合
（上游 `NativeUfs` 比集合内那份小，原位替换空间可行），并把 PPI 27 路由到 Group 0 且在解 `DAIF.I` 处解 `DAIF.F`。

XNU 侧证据的通道也要一并恢复：`docs/results-2026-11-25-dump-channel.md` 之后 `oem dump` 一律
`Status read failed (Too many links)`，只有 `oem log` + `get_staged` 可用（`out/uefilog-20261125-025522/debug.log` 983 行、
`console.txt` 就是那条路径读回的）。`tooling/native/test-dump-channel.ps1` 是本轮加的仪器测试：reader 会话的**第一条**命令就是一次
`oem dump 8c0004000 100`，成功即说明通道随设备重启自行恢复，失败则本轮所有只能产内存转储的实验都不必做。