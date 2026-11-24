## 2026-11-25 — 构建环境审计：本机和 Mac 都没有 native k27 树，源码级候选暂时无法产出

查了能否把 2026-11-24 留下的 ufs185 发布序列改动编成镜像：本工作区没有 `PIANO_K27_TREE`、没有 `~/.config/piano/env`、没有 `out/kc/kc27.bin`、没有 `out/x12377` 生成头；Mac（192.168.31.183）同样没有 k27/xnu 树、没有 kc27.bin，只有 `~/Codex/k273-esp/inputs/piano-ufs264.kc` 和 `~/ufs151/NativeUfs.o`，而 `~/Codex/UniXNU-xiaomipad8pro` 还停在基线 `2438556` 且带本地改动。所以 `build-native-image.sh` / `build-native-ufs.sh` 都会停在 `k27-tree.sh`。

含义：源码级改动（包括对照上游得出的"回到生产发布序列"候选）现在编不出来；能继续产出的只有像 ufs265/ufs266 那样在既有 KC 零填充区里打桩/跳板的二进制补丁。要恢复构建，需要 xnu-12377 的 k27 树（11ac316 + 基线 0001-0052）、`out/x12377` 生成头和 `out/kc/kc27.bin`。详见 `docs/results-2026-11-25-build-environment-audit.md`。设备仍未在 USB 上。
## 2026-11-25 — 与上游 ShengUfs 逐行对照：偏差只在 publishMedia/startMatching

把 pin 的上游生产驱动（`UniXNU/ShengUfs` `fd420df0`，即 `CrossDarwin/UniXNU_macOS` 的 `docs/porting/ufs.md`、`docs/drivers/sheng-ufs.md` 指向的版本）读完了：它发布根卷只做 `IOBlockStorageDevice::start` + `registerService`，然后 `wait()` 里以 300×100 ms 轮询 `mediaWithUuidExists(uuid, window)`，`IOMedia` 由 `IOStorageFamily` 自己创建，**全程不调用 `IOService::startMatching`**。本地 `package/NativeUfs/RootPublication.h` 与上游该头文件逐字节相同（3,224 B），所以差异只在 `NativeUfsBlock::start` 和 `publishMedia()`：我们故意不设 `device-type=Generic`、自己 `new IOMedia` 并手动 `startMatching`。

也排除了"kext 不在 KC 里"这种解释：pin 的 RAM 内核集合（ufs266，147,095,552 B）里 `com.apple.iokit.IOStorageFamily` 出现 29 次、`IOBlockStorageDriver` 329 次、`IOMedia` 464 次、`AppleAPFSContainer` 120 次（`com.apple.iokit.IOAPFSFamily` 标识符 0 次，APFS 类别在别的 kext id 下）。所以 2026-11-24 06:35 那份未提交的 `NativeUfs.cpp` 改动（窗口 `registerService`、去掉 monitor 循环、跳过 `SystemOS`）正是回到上游序列的第一步，留着作为设备回来后第一个候选，探针继续当仪表。详见 `docs/results-2026-11-25-upstream-publication-diff.md`。设备仍未在 USB 上，本段未启动、未写入。
## 2026-11-25 — Ubuntu v1.1.0 槽 A 写集已完整落地并校验

上一轮记的"`vendor_boot.img` 后半段仍在续传"已经作废：`out/inputs/v110/release/boot/vendor_boot.img` 现为完整的 100,663,296 B，`7cbb06b9…` 与发布包 `SHA256SUMS` 一致。整个 `piano-ubuntu-v1.1.0-boot.zip`（153,123,957 B、16 个成员）通过 `zipfile.testzip()`，`release/boot/` 下 `boot.img` `d8d81279…`、`init_boot.img` `96057eb2…`、`boot-ram-check.img` `9071c5af…`、`piano-boot.dtb` `50636eff…`、`boot-ram-maintenance.img` `54001a50…` 全部对上发布包哈希。槽 A 的持久写集因此离线齐备，ARTIFACTS 已补录这四条。

顺手澄清一条通道事实，避免以后重复试：`oem log` 导出的是固件 `ConOut` 环形缓冲（`PianoFastboot.c` 里 `LogOutputString` 包了一层 `OutputString`），历史抓包在 "starting menu" 就断了，XNU 的 `IOLog` 不经过它，所以内核探针值不能靠 `oem log` 取；`oem debuglog` 读的是 `PIANO_LOG_BASE` 的 DEBUG 构建环形缓冲，需要 `PIANO_LOG_MAGIC`。两条都不能替代 `oem dump`。

设备仍不在 USB 上（`adb devices` 与 `fastboot devices` 均为空），ufs265/ufs266 两发探针仍未启动，平板无任何写入。工作区里 2026-11-24 06:35 的 `package/NativeUfs/NativeUfs.cpp`、`tooling/k27/build-native-kext.py` 改动是 ufs182–185 的旧实验，已被 ufs264/265/266 取代，保持未提交、未修改。
## 2026-11-25 — 备用探针：`IOService::startMatching` 八点插桩

平板仍不在 USB 上，所以把"acceptNewMedia 探针若显示 0xA0 缺席"时需要的下一发镜像先做出来了：`out/k27-smprobe/piano-ufs266.kc`（147,095,552 B，`9f276dff…`）在 ufs264 的 `__ZN9IOService13startMatchingEj`（`0xfffffff00c1450ec`）上插 0xB0-0xB7 八个点——入口、`doServiceMatch` 前后、循环退出条件 `cmp w21,w9 / b.hs`、`thread_block` 前后、退出路径、最终返回路径。8×192 B 桩 + 两个条件跳板共 1,544 B，仍在同一 1,657 B 零洞里；首指令 `pacibsp` 保留，替换的是其后的 `stp`。

反汇编确认了判据：busy 计数是 `[x20,#0x4c]` 的低 10 位，`w21` 是进入时的值；`0x14529c` 的 `b.hs` 表示"进入计数已不低于当前计数"就退出循环，否则置 `0x18000000`、`assert_wait`、解锁、`thread_block` 后回到 `0x145250` 再匹配。因此表里能直接分开审计文档留下的两案：有 0xB1 无 0xB2 = 卡在 `doServiceMatch` 内；有 0xB4 无 0xB5 = 卡在 `thread_block`。

镜像 `out/k27-smprobe/boot-piano-ufs266-smprobe.img`，185,733,120 B、`a56f132d…`、boot id `a79d3a17…`；与 ufs264 镜像差 1,552 B / 51 段（boot id、8 个位点、桩块），回读 ramdisk 内 `kernel.macho` 哈希等于新 KC，洞内其余 113 B 仍为零。runner `tooling/native/run-ufs266-smprobe.ps1` 固定了镜像哈希、用 `--codes b0 b7` 扫表，无设备时退出 2。`decode-slots.py` 顺带加了 `--codes LO HI`。详见 `docs/results-2026-11-25-startmatching-probe.md`。未启动、未写入平板。
## 2026-11-25 — ESP 记录审计：哈希、输入、几何、store 绑定全部自洽

只读复核 `out/persistent/esp-device.json` 与本地 `out/esp-device.img`：本地/Mac/sda37/LUN0 四路都是 `68c28df1…`、1,073,741,824 B；KC/ADT/trustcache/seed 四个输入哈希与 pin 一致；`store.extent=[107512576,124289791]`（16,777,216 块=64 GiB）、`first_lba=117735999`、窗口 6,291,649 块落到 124,027,647、尾部 262,144 块=1 GiB 保留，与 `WRITE-PLAN.md` 第 77 行推导完全一致，`test-esp-geometry.py` 15 项全过。

纠正上一轮我自己记错的一点：`7e9bd39b…` 是 seed identity（kernel 按页表算的哈希），文件哈希是 `48dbc08d…`、22,200,480 B / 逻辑 24 GiB。ESP 里装的正是这份 first-install seed，所以 ARTIFACTS 原文并不算错，只是把两个哈希的角色混在一起；已在该行标明 file 与 identity。

新增一个必须带进刷写步骤的顺序约束：记录里 `data_store.installed_seed=null`、`same_seed=false`、`kc_has_store_guard=true`。主机测试覆盖"空白 store 窗口允许首次 `data-persist=2` 启动"，但一旦记录为已安装，空白 store 会被拒。所以写 Data 窗口的同时必须写入/记录 seed identity，否则后续启动会被 store guard 判成缺盘。详见 `docs/results-2026-11-25-esp-record-audit.md`。本审计未启动、未写入平板。
## 2026-11-25 — boot_a v4 已用实机跑过的 FD 重建；A 三件套写入计划就绪

缺签名私钥不再阻塞：从 `out/no-demura-control/boot-piano-blendfix.img`（同 FD 共 353 份）取出 3 MiB FD `44531624…`，LZMA 解码后确认同时带 `unixnu/0001`（`config: volume %s`）和 `darwinboot/0001`（`taken (largest run)`），即 13 GiB 窗口，配当前 8 GiB ADT 合规。用同一 kernel field 的 192 字节 BootShim、本机找回的 `piano-boot.dtb`（`50636eff…`）和 pin 住的 `mkbootimg.py`（`37d84b3d…`）拼出 v4：3,301,376 B、`b267c758…`，头部与历史 v4 完全同构，未签名。详见 `docs/results-2026-11-25-boota-v4-rebuild.md`。

Ubuntu v1.1.0 boot 包下载缓慢，但半截 zip 已用本地头解析完整解出 `init_boot.img`（`96057eb2…`）、`boot.img`、`piano-boot.dtb`、维护镜像，均与包内 `SHA256SUMS` 一致；只差 `vendor_boot.img`（96 MiB，`7cbb06b9…`）后半段仍在续传。`WRITE-PLAN.md` 已增补槽 A 三件套写入、逐分区读回、槽位计数与回灌方案，备份哈希齐全。
## 2026-11-25 — dump 重试仍失败；签名私钥与 v5 镜像确认不在本机

一次无人值守 reader 启动后，第一条 `oem dump 8c0004000 100` 仍返回 `Too many links`，平板自行回到 HyperOS 3.0.309 且无写入，通道需主机 USB 重新枚举。签名链复核：历史 `keys/` 只有公钥记录，`piano-macos-c146/private` 为空，ARTIFACTS 写明私钥留在签名主机；Mac 上也没有 `persistent-v5` 或任何大于 100 MiB 的镜像。持久 boot 的写入前提仍缺签名 v5。详见 `docs/results-2026-11-25-dump-retry-and-signing-audit.md`。
## 2026-11-25 — 布局复核：候选 GPT、ESP、root 均已落地，Data 仍空白

只读复核确认当前设备运行的是候选 GPT：`linux_rootfs` 20 GiB（sda35）、`linux_data` 64 GiB（sda36）、`esp` 1 GiB（sda37），`/data` 376 GiB，与候选表一致，不是原厂 461 GiB。此前的“当前没有 linux 分区”是 11-19 的旧状态。

ESP 已于 2026-11-24 23:23 写入 1 GiB 镜像并做双路读回：`sda37` 与 LUN 0 偏移 485507 块均得到 sha256 `68c28df1…`。root 已于 11-22 写入 `linux_rootfs` 17,547,919,360 B，直接读回 `db3629e5…`，尾部与两份 GPT 未变。`state.json` 的 Data store 仍是 blank、`store_matches_esp: false`，Data seed 尚未写入。

Mac `~/Codex/k273-esp` 下没有 root 镜像，也没有签名 v5 持久启动镜像。剩余阻塞仍是签名 v5、ufs264 同步异常、dump 通道需 USB 重新枚举、native k27 锁定提交无来源。详见 `docs/results-2026-11-25-layout-recheck.md`。
## 2026-11-25 — 切换工具审计：v1.6.3 电脑端脚本未变，设备内成功位方案可复用

上游 `yzddmr6/xiaomipad-8pro-mainline` 最新 release 已到 v1.6.3（2026-11-23），tools 包 sha256 `680377bf2eee…`；`tools/piano-switch.py` 的 blob 仍是 `c504b603…`、3,111 B，与 v1.6.1 逐字节相同。该脚本只做 `getvar` 预检、哈希校验、顺序 `fastboot flash` 槽 A 三件套和失败回灌，没有成功位与重试门控、没有写后整分区读回；`INSTALL.en.md` 仍只支持 3.0.303 且会擦除 Android 数据。结论不变：电脑端安装器不采用。

设备内 `piano-boot-switch`（整分区 SHA-256 判占用者、只碰槽 A 三件套、失败回灌）、`piano_bootslot.py`、`piano-slot-success`（fail-closed 只改 b54，两份 GPT 一致才写、逐字节读回）与 KernelSU 模块值得复用，并与我们 `maint-session.py slot-success`、`piano_309_gpt_write.py` 对齐。冷启动门控可参考 `tools/piano-cold-boot-accept.py` 的三态判据（PASS/FAIL/UNAVAILABLE，缺失即 UNAVAILABLE，第二次采集 UPTIME ≥ 200 s）。详见 `docs/results-2026-11-25-switch-tool-audit.md`。本审计未在平板执行任何切换。## 2026-11-25 — 上游复核：无新变化

`git ls-remote`：`lenzhang/UniXNU-xiaomipad8pro` 的 `main` 仍是 `480c661a1ffeaf5b0a8e3ca25ff7a62d02aea9b4`；`CrossDarwin/UniXNU_macOS` 的 `main` 仍是 `392a41a298eaf804507d9598ae92e2b2a99277c5`。与 v2 计划比较时的基线一致，本轮没有需要重新锁定的上游变化。CrossDarwin 仍是 SM8550/sheng 平台，其 `memory/handoff-diagnostics` 只作参考。
## 2026-11-25 — `oem dump` 由可用退化为持续失败；`oem log` 仍正常

实机顺序（每条都是 reader 启动后的第一条设备命令）：

| 会话 | 请求 | 结果 |
| --- | --- | --- |
| 冷启动 | `8c0004000 100` | OKAY，256 B 全 0 |
| 冷启动 | `9cb00000 100` | `Too many links` |
| 冷启动 | `8c0004000 100` 复测 | `Too many links` |
| ufs264 热会话 | `8c0916880 1800` / `8c0004000 100` | `Too many links` |
| ufs264 热会话 | `9cb00000 700000` / `2000` | `Too many links` |
| ufs264 热会话 | `8c5912880 2000` 三次重试 | `Too many links`，reader 掉线 |

对照：同一 reader 版本里 `oem log` + `get_staged reader-fw.log` 正常（100 B），所以暂存/批量通路没问题，只有 `oem dump` 失败；同一地址同一长度早些时候成功、晚些时候失败，所以不是地址属性。`pnputil /restart-device "USB\VID_18D1&PID_4E11\7ccdd855"` 即使提权也是 `Access is denied`，沙箱内无法重置该接口。

结论：RAM 读回卡在主机 USB 层。需要重新建立平板的 USB 连接（换端口或重新插拔）后才能读 ufs264 的同步异常寄存器。设备现处 HyperOS OS3.0.309.0.WPYCNXM `_a`，`sys.boot_completed=1`，无任何写入。

另外纠正一处记录：`_piano_stage_slots` 的 PA 就是 `0x8c5912880`（换算 `VA - 0xfffffff007004000 + 0x8c0004000`），之前写的 `0x8c0916880` 是我算错的，该地址未映射。
## 2026-11-25 — `oem dump` 通道规则查清：每次 reader 启动只能发一条命令，未映射地址表现为 `Too many links`

实机验证（详见 `docs/results-2026-11-25-dump-channel.md`）：

- `oem dump` 必须是 reader 起来后的**第一条** fastboot 命令；之前发过 `getvar`/`oem help` 的会话里 dump 必失败。dump 成功后的 `get_staged` 正常。
- `Too many links` = 目标地址不在当前 stage-2 映射里（EP0 复位导致 Windows 报这个错），不是大小限制。`0x8c000000`、`0x9cb00000` 失败，`0x8c0004000` 返回 256 B。
- 内容有效的唯一时机是测试镜像自身热复位后的同一会话；从 Android 重启进 bootloader 再起 reader，`0x8c0004000` 读回全 0。地址用 Gunyah IPA 空间：kernel `__TEXT` 基址 `0x8c0004000`。
- 因此早前那份 `ufs264-slots.bin`（`0x8c5912880`，全量重启后取的）是页表样数据而非探针值，不能用来判断探针是否执行；静态 `_piano_stage_slots` 地址结论不变。
- 新增 `tooling/native/dump-first.ps1 -Addr <hex> -Size <hex> -Out <file>`，自动完成"启动 reader → 只发一条 dump → get_staged"，dump 失败时不再进入上传等待。

设备仍在 HyperOS OS3.0.309.0.WPYCNXM `_a`，`sys.boot_completed=1`；无任何分区/GPT/boot/userdata 写入。
## 2026-11-25 — ufs256/ufs264 __DATA 完全相同；符号名在、地址表被剥离；dump 通道需重新校准

本轮两次无人值守 `fastboot boot`（ufs264 再次 37.3 s 自行回 fastboot，死亡点未变）后做了纯主机分析，结论写入 `docs/results-2026-11-25-kc-layout.md`：

- 两个 RAM 镜像里嵌的静态 kernel.macho 段布局一致，`__DATA` **逐字节相同**（5,210,112 B）。所以 ufs264 没有移动 `_piano_stage_slots`；之前“地址需重算”的前提不成立，`0xfffffff00c912880` 仍是该系列的静态地址。
- `piano-ufs264.kc` 的 `__LINKEDIT` 里有两份按字母排序的本地符号名字符串（含 `_piano_stage_slots`），但各 `com.unixnu.*` 的 `LC_SYMTAB` 的 `nlist` 被剥离为 0 条，名字无法给出地址。
- `__TEXT_EXEC` 有效（首字节 `paciasp; mov w0,#0; ret`），ADRP+ADD 全扫得到 8,478 个 `__DATA` 目标，此前“0 个引用”是解析错误。
- `oem dump` 本轮对 5 MiB（`8c5844000`）和 4 KiB（`8c0004000`）都在第一次请求就 `Too many links`，而早前 8 KiB（`8c5912880`）成功。失败不是单纯大小限制，通道或可读范围尚未校准。

设备每次自行回到 HyperOS OS3.0.309.0.WPYCNXM `_a`，无任何分区/GPT/userdata 写入。下一步：先复测 `8c5912880` 4 KiB 作对照，再读 `8c0004000` 64 B 与静态头比对，判定运行时是否 slide。

## 2026-11-24 23:5x — 设备已回到无人值守可控状态；ufs264 实机跑完但未前移

ADB 已授权（`adb devices` = `7ccdd855 device`），`adb reboot bootloader` 可直接进 fastboot，维护镜像也能远程进入和退出，不再需要按键。**fastboot 必须用 `out/pt33` 的 33.0.3**：37.0.3 会把 `boot-ram-maintenance.img` 判成 `Failed to load/authenticate boot image: Load Error`，33.0.3 正常 `OKAY`。

`boot-piano-ufs264cfg.img`（`d74fad3a…`）两次 `fastboot boot` 均在 37.3 s / 51.9 s 自行回 fastboot，与 ufs256 38.6 s、ufs263 44.9 s 同量级，说明删掉 PPI27 分支里的 `CNTV_CTL_EL0=2` 还没改变死亡点；无任何分区/GPT/userdata 写入。

读回通道恢复，但规则是：**每启动一次 reader 只能做一次 `oem dump`**，第二次会 `Too many links` 并把设备踢出 fastboot。槽位表和 console 要分两次 reader 启动读。`0x8c5912880` 的 8 KiB 已取回（`out/no-demura-control/ufs264-slots.bin`），但内容不是探针值，而是每条目 32 字节、单调递增 `0x200000004` 的页表样数据——该地址是按 ufs256 KC 推的，ufs264 的 `_piano_stage_slots` VA 需要重算，不能据此判断探针是否执行。

设备当前在 HyperOS OS3.0.309.0.WPYCNXM，`_a`，`sys.boot_completed=1`。ESP（`68c28df1…`）与 root（`db3629e5…`）的写入记录未变。boot_a 仍无签名 v5 产物，签名私钥不在本机/Mac，仍是持久启动链的硬阻塞。详细证据见 `docs/results-2026-11-24-ufs264.md`。

## 2026-11-25 — 维护会话接受设备 ESP 布局；几何测试全绿

`tooling/persist/maint-session.py` 的 ESP 记录校验从硬编码历史 LBA 改为 `check_esp_placement()`：LUN 0、分区 37、262144 块、1 MiB 对齐、窗口在 LUN 0 内；没有 `partition` 字段的旧记录仍按历史 `common.ESP_FIRST` 解释。`map` 现在把 GPT 状态分为 `old`/`candidate`/`new`/`unknown`，并加入候选表哈希。新增 `esp-candidate` 步骤：要求 live map 的 esp 分区与构建记录的 LBA/大小一致、GPT 为 `candidate` 或 `new`，写前对整块 1 GiB 做 SHA-256（`esp_before`，按无备份授权不保留原字节），写入后经分区和 LUN 0 两路读回比对，并写 `installed.json`（`layout: candidate`）。

`check` 现在接受 `map` 记录的候选 GPT 表，并按 `esp_first_lba` 校验 ESP 区域；`rollback esp-candidate` 明确拒绝伪恢复（没有字节副本，只能按记录重建或先回滚 boot-a）。`tooling/persist/test-esp-geometry.py` 修正了 `reject()` 对 `maint.Stop` 的处理，现在 25 项全过（`esp geometry: all checks passed`）。`test-persist.py` 的 `tablet.sh` 主机模拟在 Windows 上因 `dd` 不接受 `E:/...` 路径而失败，属既有平台限制；`test-data-store.py` 缺 `unixnu` 模块、`test-k273-only.py` 缺外部工具，均未在本机运行。设备侧仍是 ADB `unauthorized`，没有任何写入。
## 2026-11-25 — 设备布局 ESP 已离线构建并复现

`tooling/persist/build-esp.py` 新增 `--esp-first-lba`、`--rootfs-last` 和 `--data-extent`。给定平板候选布局（root 102,269,696-107,512,575；Data 107,512,576-124,289,791；ESP 124,289,792-124,551,935）后，脚本自动把 macos 条目绑定为 `native-ufs-root-part=linux_rootfs data-store-part=linux_data data-store-lba=117735999`，并拒绝 ESP 与 Data 重叠或 24 GiB store 放不下保留区的情况。`tooling/persist/test-esp-geometry.py` 覆盖 LBA、`dd seek=485507`、双向重叠和窗口算术。

Mac 上以固定 `--utc 1795478400` 构建两次，`esp-device.img` 与 `esp-device-repro.img` 哈希一致：1,073,741,824 B，SHA-256 `68c28df15bcf55a589441df37e9c5fd94614e2771c437e955f190e3eb0aab898`，FAT hidden sectors 124,289,792。记录 `~/Codex/k273-esp/out/esp-device.json` 仍标 `OFFLINE_BUILD_NOT_DEVICE_TESTED`，完整命令和校验见 `WRITE-PLAN.md` 的 “ESP build record”。没有向平板写入任何字节。

设备侧仍只有 ADB `unauthorized`：恢复出厂后需要在平板上点一次“允许 USB 调试（始终允许）”。`fastboot devices` 为空。授权后先 `adb reboot bootloader` 记录槽状态，再 RAM 启动维护镜像跑只读 `maint-session.py map`，与候选 GPT 表逐项比对后才讨论写入。所谓 “native k27” 锁定提交 `11ac316b…` 至今没有权威来源；本轮 ESP 使用的是已核对的 `piano-ufs264.kc`（含 k27 0066、boot guard、data-store-lba、PianoA830 标记），不能把它称为那个缺失的提交。

## 2026-11-24 23:xx — 上游 v13.1y12d 锁定完成；设备侧仍卡 ADB 授权

上游 `lenzhang/main` 经 SSH `git ls-remote` 复核仍为 `480c661a1ffeaf5b0a8e3ca25ff7a62d02aea9b4`，比计划基线 `2438556c` 多 506 个文件，但只是文档/验证批，依赖 pin 未变。完整树已抓成 `upstream-tip-480c661a.zip`（SHA-256 `cdac94d502b226d97d85c781680f2e153205bf0d8dddb06f45cf2fc2ec260009`），`scp` 到 Mac 后解压于 `/Users/zhangqi/Codex/upstream-480c661a`；本地副本移入被忽略的 `out/upstream-audit/`。结论已写入 `REPRODUCIBILITY.md` 的 “Upstream recheck 2026-11-24” 与 `MIGRATION-DIFF.md` 的 “Switching gap against v13.1y12d”。提交 `dfd3360c` 已推送，分支 `main`。

关键事实：v13.1y12d **没有 tag 或 release**，锁定依据是 README 的描述加构建记录；README 自认 OS switcher 只有 RAM 只读验证，“A full switch to Android has not been tested”，所以两轮双向切换在本机必须自己实现与验收。上游日常布局是 root 在 `linux_rootfs`、Data 是 `linux_data` 上 200 GiB（LBA 71859642），且依赖作者私有的 v1.1.0 维护镜像、签名密钥和 200 GiB seed，无法照搬。

本机已有可复用资产：候选 GPT 早已写入并读回（`WRITE-PLAN.md` 顶部：userdata 截止 LBA 102269695、root 102269696-107512575、Data 107512576-124289791、ESP 124289792-124551935），`out/piano-stock-root-26C5035r.img`（16.34 GiB，能放进 20 GiB root 区），`out/inputs/k273/first-install/scratch-data.img`（22,200,480 B），以及 `out/no-demura-control/*.kc`（129.5 MB 级内核集合）与对应 `.macho`。Mac 侧 `/private/tmp` 里有稀疏 root 表镜像（`piano-root-26C5035r.593r2m`、`piano-root-tables.LLOseQ`、`piano-vroot-layout-yiktynqv`、`piano-stock-root.1ubLtk`）和 `native-driver-checkpoint/piano-boot.dtb`。

Apple 27.3 素材其实在本机：`D:\Download\piano-macos-273-inputs` 里的 `kernelcache.release.mac17j`（32,709,948 B，SHA-256 `66bbe2fa…4720`）、`DeviceTree.k114cap.im4p`（`7810505c…`）、`141-22145-021.dmg.aea`（`00f80636…`）、`141-22233-021.dmg.aea`（`b7dbb5c6…`）及两份解密 DMG 全部与相邻 IPSW member JSON 一致（本轮逐一重算）。缺的是派生成套 `out/inputs/k273`（kc/root/trust）和可构建的 k27 树。成套 ESP/启动集仍没有：本机与 Mac 全盘都没有 `esp-k273*.img` 或 build record，`build-k273-esp.py` 需要 `--base`；`out/firmware` 只有 `fs19xx.fsm` 与 `vpu35_4v.mbn`；ADT 与 trust bundle 尚未按 27.3 集合生成。Mac 只剩 9.8 GiB 空闲，放不下 13.8 GB 解密 root。

设备侧：`adb devices` 仍是 `7ccdd855 unauthorized`（ADB server 重启后一样），`fastboot devices` 空。恢复出厂清掉了 ADB 授权，必须在平板上点一次“允许 USB 调试（始终允许）”；这是当前唯一无法由主机完成的动作。ufs264 已构建未运行，ADB 恢复后可直接跑 `tooling/native/run-ufs264.ps1`。fastboot 通路也需要同一次重启进入 bootloader 才能验证。
## 2026-11-24 21:00 — IRQ 链路已通，死亡点是同步异常；ufs264 已构建，读回通道待 ADB 授权

实机已验证（RAM 启动，`piano-reset-after` 自动回 fastboot）：

- ufs256（只加 VBAR_EL1 修复，无探针）自然返回 38.6 s；ufs257-260 在第 1 个 tick 各处加 2 s spin 后返回 41.0 s，证明 PPI27 的 IRQ 全流程（prologue、IAR、rtclock_intr、epilogue）都执行到了。
- ufs261（第 4 tick）38.8 s、ufs262（第 2 tick）38.8 s：第 2 个 tick 从未发生。
- ufs263（sleh_synchronous 入口 2x3 s spin）44.9 s：死亡是同步异常，而且连中两次。

由此确认一个独立缺陷：手工 PPI27 分支里写了 CNTV_CTL_EL0 = 2（IMASK=1, ENABLE=0），把虚拟定时器整个关掉；XNU 自己的 FIQ 路径（sleh.c:3353 起）并不这么做，只依赖 ml_set_decrementer() 写 TVAL 重新装弹，ENABLE 由 fiq_context_init() 一次性打开。ufs264（/private/tmp/boot-piano-ufs264cfg.img，SHA-256 d74fad3a4b8425bbc3fe929c776a985eaa2a69aadba30770cc7baf63b1e83a4a）删掉这行，并在 tick 里写 piano_dbg_kv(0x50, 计数)。尚未运行。

读回通道改为小块：同步异常的 ESR/FAR/PC/x16/x17 由 sleh_synchronous 的探针写进 piano_dbg_kv 槽位 0x7b-0x7f，也就是镜像内的 piano_stage_slots 表（PA 0x8c5912880，全部槽位 0x0b-0xca 共 0x1800 字节）。tooling/native/run-ufs256-dump2.ps1 只 dump 8 KiB，并在跑完测试镜像后先 fastboot reboot bootloader 重新枚举 USB 再起 reader ——上次 FAILED (Status read failed (Too many links)) 就是紧接着复用刚返回的那个 fastboot 会话。tooling/native/decode-slots.py 按每槽 4 份做按位多数解码。

当前阻塞：平板已经回到 HyperOS（USB VID_18D1 PID_4E11 序列号 7ccdd855 在 USB 上），但 adb devices 是 unauthorized，adb reboot bootloader 和所有 fastboot 都进不去。需要在平板上点一次“允许 USB 调试”。ufs264 与读回脚本都已就绪，授权恢复后可直接跑，不需要再构建。
The [masked GIC polling note](docs/results-2026-11-24-timer-masked-polling.md) closes the timer question for the delay path: `piano_delay_timer_service()` now arms `CNTV` once, polls `ISTATUS`, acknowledges PPI 27 through the CPU interface and calls `rtclock_intr(0)` with IRQs masked, and never writes DAIF back from a saved register (that write parked ufs206-209). ufs210/ufs211 (`52f5e277...`/`9952514c...`, paclr168/169, RAM only) show 773-879 GIC acknowledgements, `rtclock_intr` returning, and the media walk completing. The next wall is real interrupt delivery: ufs212 (`25c25225...`, one-second media wait after `startDeferredMatches()`) never returns, because `waitForMatchingService` blocks the thread and nothing in the tree clears DAIF.I/F, so no exception ever reaches `piano_gic_irq_handler`. The documented FIQ route (`piano-gic-irq=0`, `docs/results-2026-11-23-paclr71-gicv3.md`) plus unmasking DAIF.F is the prerequisite for `thread_block` timeouts, launchd and the idle loop. The UUID the setconf wait asks for (`84581E99-...`) is not published by NativeUfs either, so `Wait for root failed` follows any delivered timeout. Every run was `fastboot boot` only and returned to HyperOS OS3.0.309.0.WPYCNXM with boot_completed=1; no desktop, userland, switching or Data acceptance has started.
[ufs193](docs/results-2026-11-24-ufs193.md): the 147 MB kernelcache rebuilds again once native kexts supplied through `--native` replace the base's copies (`patches/k27-kc/0002`), and re-applying the `_corecrypto_kext_start` -> `mov w0,#0; ret` patch makes it boot: ufs193 (`78219992...`, RAM only) returns to fastboot after 85.1 s with 318 `NativeUfs` lines, the full media chain to Root, `OS media register entering (synchronous)`, the OS container's `dev_init:299` and `bsd_init`/`sd_init` calls. Without the corecrypto patch (ufs191/ufs192) the same build stops at `PE_init_iokit` and returns at ~67 s. No launchd, GPU or userland yet; Android came back with boot_completed=1 every run.

[ufs172-181](docs/results-2026-11-24-ufs172-181.md): the OS window's first block is the APFS container superblock, so the media is published as Apple_APFS (UUID A7672058-...) instead of being probed as a disk. ufs177 proves a synchronous startMatching drives APFS far enough to start swcrypto and dev_init for the SystemOS container, but it blocks the publisher for the rest of the boot. ufs178-181 tried asynchronous register, a dedicated kernel thread, and driving the match from the publisher after both windows exist: the thread object is created yet never runs this early, and all three stop logging at `monitor 0`. Every run returned to HyperOS OS3.0.309.0.WPYCNXM with boot_completed=1 under fastboot boot only; no desktop, userland, switching or Data persistence is verified. Next: find the point after BSD starts at which matching can be driven without freezing IOLog.

﻿The [paclr71 GICv3 note](docs/results-2026-11-23-paclr71-gicv3.md) adds the missing interrupt route: the k27 FIQ path already ends in `rtclock_intr`, but no kext here programs a GIC, so `piano_gicv3_init()` now sets `GICD_CTLR.ARE_NS|EnableGrp1NS`, each CPU's redistributor frame for PPI 27, and `ICC_SRE/PMR/CTLR/IGRPEN1` before `fiq_context_init(TRUE)`. `boot-piano-ufs49gic.img` (`085ed127...`, RAM only) also keeps paclr70's spin delay and the `piano:cfg` probes; `out/no-demura-control/sup-ufs49.ps1` runs it unattended with `piano-gic=0` available as the A/B control. The tablet has still not enumerated on USB.
﻿The [paclr70 spin-delay note](docs/results-2026-11-23-paclr70-spin-delay.md) fixes the mechanism behind the parked IOResources thread: arm64 `fiq_context_init()` only enables `CNTV_CTL_EL0` and nothing in the 79-kext set programs a GIC or FIQ route, so timer callouts never fire while `mach_absolute_time()` keeps advancing. paclr70 turns `delay_for_interval()` into a bounded spin and the disassembly confirms it; `boot-piano-ufs48spin.img` (`03ee3301...`, RAM only, `io=0x20`, `piano-reset-after=90`) is built with the same `piano:cfg` probes as ufs47cfg and is waiting for the tablet to appear on USB. `out/no-demura-control/sup-ufs48.ps1` runs it unattended; the tablet has not enumerated since the manual restart.
﻿The [native UFS root window result](docs/results-2026-11-23-native-ufs-root-window.md) records the first run where the UFS block layer published media at all: ufs21 (503c3151鈥) completes NOP, the power mode query, START STOP UNIT and TEST UNIT READY, then reads the GPT header, GPT entries and root manifest and publishes two windows (UNVERIFIED development root) before scheduling the root publisher thread. Two corrections carry forward: a single 64 KiB data slot is the reproducible DMA layout (adding slots exhausts contiguous pages, and the old size probes consumed the pages they measured), and kernel IOSleep/IODelay never return before interrupts are enabled (the wake sleep line simply stopped while other threads continued). A bounded loop reading cntvct_el0 directly measured 1200000 ticks over 50 ms and unblocked the path; the architected counter runs, only timer-callout waits are dead. The kernel still does not reach sd_init or launchd inside the 80 s watchdog, so the next target is kernel-side arch timer interrupts through the GICv3 that ArmCpu drives. Every run was astboot boot only and returned to HyperOS OS3.0.309.0.WPYCNXM with oot_completed=1 unattended.

The [watchdog return result](docs/results-2026-11-23-paclr53-watchdog-return.md) shows that the tablet now comes back from a hung k27 boot with no manual power cycle: two short-argument runs each returned the fastboot serial **80.1 s** after `fastboot boot` OKAY and HyperOS reached `boot_completed=1` on its own. The console proves 0056 runs and arms the Gunyah watchdog (`piano-reset-after: reset at 20 s, Gunyah watchdog armed (32 s bite)`) while the 20 s `thread_call` never fires, so the return is the 32 s hardware bite. The root-path run then showed exactly where it stops: after `NativeUfs: DMA attempt 0` and `NativeUfs: DMA attempt 1` and before any `DMA virt=` line, while `NativeGopDisplay`, `dlil_init` and `CoreAnalyticsHub` continue in other threads. Next is per-attempt logging of that allocation in `package/NativeUfs/NativeUfs.cpp`, whose kext rebuild first needs the `unixnu-research` tree restored on the Mac. Both images were `fastboot boot` only; no partition, GPT or userdata write.

The [paclr53 probe 2 and probe 11 result](docs/results-2026-11-23-paclr53-probe11-probe2-noreturn.md) shows that neither image came back: probe 11 (723e871a?, read-only, piano-reset-after=240) ran after a clean ABL preflight and `fastboot boot` OKAY, then stayed silent for over five minutes until a manual power cycle brought HyperOS up on slot `_a` with `bootreason=bootloader`, and probe 2 (6a8f7c1?, piano-reset-after=45) did not return within 150 s. Re-reading `paclr53-fips4-live.log` also shows the older run returned inside one 15 s poll after the payload started, far sooner than its `piano-reset-after=90`, so that return was a panic reset and 0056 has never been observed to fire. Two short-argument controls are built to settle it without the DroidVM line: `boot-piano-paclr53-timercheck.img` (678f6729?, probe 2, piano-reset-after=20) and `boot-piano-paclr53-rootshort.img` (324ebe1?, root=1). `run-paclr53-timercheck.ps1` records the seconds to the fastboot serial and stages the pstore console and panic zone. The tablet was off USB after the manual restart, so both are still unrun.
The [paclr53 UFS root backstop note](docs/results-2026-11-23-paclr53-ufsroot-backstop.md) shows that the image which hung (`fc9cfa24鈥) had no `piano-reset-after` and no `piano-boot-guard` in either `args` line, verified by reading `/EFI/UniXNU/unixnu.toml` back out of it, so the empty six minutes never had an unattended return to miss. `boot-piano-paclr53-ufsroot3.img` (`e8d21614鈥) adds `piano-reset-after=180 piano-reset-to=bootloader piano-boot-guard=150` with a byte-identical payload, and `run-paclr53-ufsroot3.ps1` dumps the pstore console after the reset. The rooted stage lines are already in DRAM at `9cb00000`. `tooling/native/check-boot-image.py` now fails any k27 RAM image whose live `args` ask for a native UFS run without `piano-reset-after`/`piano-boot-guard` or without `pstore-console`, reading the ramdisk FAT so orphaned copies of an overwritten `unixnu.toml` do not fool it. Next are probe 11 (`723e871a鈥, read-only, panics with the bring-up result) and then ufsroot3; both need the tablet power-cycled with power + volume-down for 15-20 s first, because the current hang cannot be ended over USB.

The [paclr53 pthread registration trial](docs/results-2026-11-23-paclr53-pthread-registration.md) captured both pthread_kext_register calls through physical kmem slots. Both arrive from _pthread_start+0x20 with the same functions table and callbacks. The first call's store is lost while the slot still reads xfffffff007f40000; the second call's store persists as xfffffff007ec8848. The idempotent overwrite removes the re-initialisation panic. Boot now reaches NativeUfs start ... root=0 root-part=linux_rootfs, so the next blocker is UFS root preparation. astboot boot only, no writes; HyperOS A returned with oot_completed=1.

# Native macOS Deployment Handoff

The [MMU transition tablet trial](docs/results-2026-11-22-xnu-mmu-live.md) read back source-defined XNU markers through the arm_init return point after enabling the MMU. The first arm_init path reached the pre-`_arm_vm_init` marker; post-call slot 0x12 was absent. Diagnose VM initialization using its early markers before proceeding to root storage.

The pre-SCTLR diagnostic KC passed the host DarwinBoot parser. It records the intended SCTLR value at CVP 0x9d1f56a0 before enabling translation. The Mac recipe requires the frozen osfmk/DEVELOPMENT working directory and the candidate kernel.files with its replacement start.o. Candidate inputs and KC are retained in out/no-demura-control. Device validation is pending.

The [XNU early marker audit](docs/results-2026-11-22-xnu-early-marker-audit.md) traced the actual packed entry to the retained Mac start.s. Prepare the next control without the pre-MMU Display_Demura writes.

The [DarwinEnter mailbox trial](docs/results-2026-11-22-enter-mailbox-abl.md) recovered nine matching EBS success records, with no XNU log. HyperOS A returned. Instrument the assembly handoff and kernel entry separately.

The [XNU entry trial](docs/results-2026-11-22-xnu-entry-abl.md) returned to ABL and recovered a complete pre-Android CVP capture without an XNU log. Preserve the EBS mailbox checkpoints in the next DarwinEnter candidate.

The [DarwinBoot mailbox trial](docs/results-2026-11-22-darwinboot-ebs-abl.md) captured eight matching EFI_SUCCESS records before Android started. HyperOS A returned. The probe stopped before DarwinEnter. Next isolate the kernel branch and use the same return route.

The [watchdog ABL retention control](docs/results-2026-11-22-watchdog-abl-retention.md) read back the CVP marker before Android started and returned to completed HyperOS A. Apply the qualified route to the separate DarwinBoot ExitBootServices mailbox image and capture its result before Android starts.

The [live firmware ring](docs/results-2026-11-22-enter-ring-live.md) confirms unattended menu selection and the final marker before ExitBootServices. HyperOS returned on slot A. The next device trial needs an independent checkpoint after ExitBootServices; XNU entry remains unproved.

The watchdog-gate reset build links ArmPsciResetSystemLib. Its complete piano platform source has no caller of PianoResetHookInstall, so the stock block flush hook is not installed by that path. ResetSystemRuntimeDxe still invokes registered callbacks before PSCI SYSTEM_RESET. The older watchdog-status image returned to HyperOS through firmware Fastboot reboot using the same UEFI cold-reset API, but that does not demonstrate the gate failure branch before Fastboot starts. Keep this candidate separate from hardware-qualified recovery.

The [retained k27 baseline replay](docs/results-2026-11-22-k27-baseline-replay.md) reproduced the clean c2936ef tree from the 47 local patches. Native 0053 still requires three different file revisions. Preserve the replay as a separate reconstruction input.

The [CrossDarwin profile comparison](docs/results-2026-11-22-crossdarwin-profile.md) verified the same Apple XNU base and 26C5035r target. The public patch series is a reconstruction reference. The prepared k27 tree and piano runtime compatibility remain unverified.

The [ABL slot and snapshot readback](docs/results-2026-11-22-abl-snapshot-readback.md) confirmed A active/successful, retry 6, and snapshot status none in userspace Fastboot and ABL; the tablet returned to HyperOS. Slot B reports unbootable and was not changed. No native Darwin recovery evidence yet.

The [Native Data and UFS write audit](docs/results-2026-11-22-native-data-write-gates.md) distinguishes the mode-0 RAM Data path from the separate UFS log/export gate. Data store tooling and ABL state preflight tests pass in UbuntuPiano. Verify the effective boot arguments and live slot state before device qualification.

The [Data startup binding audit](docs/results-2026-11-22-data-startup-binding.md) links the retained kernel build recipe to the first-install Data UUID. Its DEVELOPMENT/DEBUG startup path requests a writable APFS mount before init, so review mount writes independently of the later fsck shim. Runtime order and recovery remain open.

On 2026-11-22 the Mac mounted the first-install Data candidate read-only after matching UUID 1B4CEC84-35B3-4AAD-88D0-200DD0956637 with the current image selector. Explicit fsck_apfs -q on its raw volume returned 0 and reported a clean filesystem. Cleanup detached the image and its raw SHA-256 still matched the manifest. This covers the Mac's read-only mount and quick check. Target mount phases, repair and persistence remain open.

The [SystemOS explicit APFS checks](docs/results-2026-11-22-systemos-explicit-fsck.md) verified the pinned fixture UUID before running offline fsck_apfs -n and -q against its raw volume. Both exited 0 and the image hash matched before and after detachment. Boot-time device selection, repair and mounted-volume checks remain open.

The [stock APFS phase audit](docs/results-2026-11-22-stock-apfs-phases.md) resolves `mount -P 1/2` dispatch: phase 1 invokes `apfs_boot_util 1` on an APFS root; phase 2 invokes `apfs_boot_util 2` only when `vfs.generic.apfs.rosp` is nonzero. That utility discovers Data by APFS role in the root device's container, without a separate Data UUID. The native independent Data seed therefore still needs its own explicit mount path and a qualified write policy; do not treat stock phase 2 as mounting it.

The 2026-11-22 read-only device check found serial `7ccdd855` online in completed HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`. The by-name links resolve `esp` to `sda37`, `linux_rootfs` to `sda35`, and `linux_data` to `sda36`; ordinary ADB shell cannot read their sysfs geometry, and `bootctl` is unavailable. This confirms the current OS and names only, not live GPT boundaries, ABL success/retry state, or write authorization. See [current device baseline](docs/results-2026-11-22-device-baseline.md). No reboot or write command was issued.

The host image selector now accepts an expected APFS volume UUID and rejects ambiguous matches. On the Mac, the SHA-256-verified first-install raw candidate was attached read-only without mounting. UUID `1B4CEC84-35B3-4AAD-88D0-200DD0956637` selected the Data volume, a mismatched UUID was rejected, and explicit `fsck_apfs -n` on the selected raw volume reported it healthy. The image was detached. This qualifies offline target selection and integrity checking only; native mount phases, repair dispatch and Data persistence still require validation.

The [UEFI debug log audit](docs/results-2026-11-21-uefi-debuglog.md) identifies a separate DEBUG ring export while firmware Fastboot responds. The retained BDS lookup already confirmed loader registration, and the later watchdog witness reached the final ExitBootServices preparation. SEC clears the ring on a new UEFI boot. The remaining gap is evidence after that call and after entry into XNU.

The [packed DarwinBoot audit](docs/results-2026-11-21-mailbox-packed-driver.md) confirmed that the mailbox trial marker is in the packaged PE32 executable. The [packed DTB audit](docs/results-2026-11-21-cvp-ownership.md) identified the CVP consumer, and stock HyperOS exposes a live msm_cvp driver binding for that node. Qualify retention before HyperOS initializes the region or use an independently qualified reservation.

The [DarwinBoot APRIORI control](docs/results-2026-11-21-darwin-apriori.md) built successfully and enumerated as UEFI Fastboot in an action-only RAM boot. HyperOS returned on A with the original marker. A follow-up captured the staged log before reboot. The capture stops at menu launch and does not establish loader registration. Check the protocol directly at the BDS boundary. The retained candidate is `out/boot-piano-darwin-apriori-menu.img`.

The November 15 local k273 run retained both RAM images and its compact KC. SHA-256 verification matched the historical run record: release image `bbf48ce4d25e89ecff78febb8d92bdc863580eb12d8b3e4e182f72ef08a38298`, watchdog image `285f36fce4104910efa631677f16c4bab1d0133bf7fd7d4f85cccb2e4eed23c0`, and compact KC `ef80d7d3a16f06944676c4e7cf017d8a7ce6edf94abbea016c3b99fc04e5dd59`. The release FD `eb8bb611110cc08b5f729cd7d60e2d2a3f11119038cbb55f87d432028707e02b` is recoverable byte for byte from the release image at file offset `0x1070` for `0x300000` bytes. The standalone FD is the watchdog variant. Use the release FD as the historical DarwinBoot comparison. Historical `ParseKernel()` logs establish loader execution on that configuration. The final release screen transition was indirect evidence of XNU handoff. The watchdog candidate includes a PMIC restart reason write and its automatic recovery was pending in that run record. The old `xnu-k27-final` tree exists, is based on `c2936ef`, has local changes and did not resolve either original pinned source object.

The pinned DXE dispatcher requires all architectural protocols before scheduling a driver with no DEPEX. The diagnostic isolation patch removes CapsuleRuntimeDxe, while the DXE core includes the capsule protocol in its required table. DarwinBoot has no DEPEX in the retained FFS and is absent from diagnostic APRIORI. This gives a concrete dispatch gate to test without restoring capsule support. The menu's driver count is for configured injections only. Use the USB-capable diagnostic build for a focused comparison against the historical release FD, and capture loader registration before another XNU attempt.

The revised Probe 2 container reached RAM UEFI, uploaded the pre-XNU mailbox, and returned to completed HyperOS on slot A. See [the Probe 2 UEFI result](docs/results-2026-11-21-probe2-uefi-entry.md). Physical menu input did not move the selection. Fastboot reboot returned to completed HyperOS on slot A with the persistence marker intact. See [the menu input result](docs/results-2026-11-21-probe2-menu-input.md). The manifest now records an offline verified RAM candidate with an eight second countdown to `macos-8cpu`. FAT readback differed only in the two menu settings. The Ubuntu reference verifier passed, and neither the packed kernel field nor DTB changed. A fresh [ABL preflight](docs/results-2026-11-21-auto8-abl-preflight.md) confirmed A-slot success, six retries, no snapshot, and a normal HyperOS return with the test file intact. The WRITE-PLAN.md review of ABL persistence and recovery after XNU handoff remains open. The previously tested RAM base and this candidate both omit the AVB footer. FAT readback confirmed it uses the same KC and Probe 2 settings with `sheng-smp=1`. The single-CPU entry uses `cpus=1`, which has a documented `recount_switch_thread` abort in `docs/bringup-debugging.md`.

The manifest also records a verified derivative that adds `piano-reset-after=90` only to `macos-8cpu`. The frozen KC contains the timer diagnostics. Patch 0056 requires EARLY_BOOT and the SMC conduit before it can schedule PSCI reset. The image omits `piano-reset-to=bootloader`, so patch 0057 does not request a PMIC SDAM restart reason write. Recovery before timer initialization still needs the physical path, and the ABL persistence review still applies.

The [automatic SMP probe ran on the tablet](docs/results-2026-11-21-auto8-handoff.md). The eight second countdown selected the expected entry, then the diagnostic firmware reported that the XNU loader protocol was missing. UEFI Fastboot reboot returned to HyperOS. The retained firmware build inventory contains the DarwinBoot driver GUID and is the next candidate for dispatch verification.

The [DarwinBoot firmware trial](docs/results-2026-11-21-auto8-darwinboot.md) was accepted by ABL. USB did not enumerate during observation beyond the configured XNU timer interval. The operator photo shows the 8-CPU menu entry returned `Unsupported` and the menu reported `drivers 0 loaded / 0 failed`. The follow-up ADB and Fastboot checks found no device. ADB subsequently confirmed completed HyperOS 3.0.309 on slot A and the original persistence marker hash. The recovery trigger and post-run ABL success and retry values remain unverified. Firmware dispatch and XNU execution remain unverified. Both firmware inventories contain DarwinBoot and have the same 174 GUIDs. Thirty-seven file payloads differ, so the FD swap cannot isolate the dispatch failure.

Candidate GPT was written and independently verified on the tablet. Stock Recovery [enumerated the candidate layout](docs/results-2026-11-21-candidate-recovery-geometry.md). HyperOS subsequently completed boot on slot A, and a synced Android test file survived a new boot. See the [GPT trial](docs/results-2026-11-21-candidate-gpt-tablet.md) and [HyperOS persistence result](docs/results-2026-11-21-candidate-hyperos.md).

The frozen writer image verified `primary=stock backup=stock` on the tablet using its read-only command. See `out/maintenance-gpt/writer-readback.log`. Explicit exit returned through ABL to completed HyperOS on slot A. The subsequent candidate write passed. Stock GPT rollback remains pending.

Stock Recovery entry succeeded, but remote reboot attempts returned to Recovery. ADB subsequently observed completed HyperOS on slot A; the specific Recovery exit action was not observed. See [the Recovery entry result](docs/results-2026-11-21-stock-recovery-entry.md).

`out/maintenance-gpt/readback-direct.img` embeds `piano-309-gpt-readback`. The deployment manifest records its repeatable build and host fixture results. The packaged command passed on the tablet: both GPT digests matched stock, and ABL reboot returned to completed HyperOS on slot A. See [the direct GPT result](docs/results-2026-11-21-direct-gpt-readback.md). The readback bypasses the Linux page cache with O_DIRECT. Post-write flush and UFS readback still need device qualification.

After `/bin/piano-tablet piano-309-gpt-readback` exits successfully, save its output and run `python tooling/persist/check-piano-309-gpt-pair.py --readback-log LOG`. Require `primary=stock backup=stock` before a candidate layout trial.

The separate `out/maintenance-gpt/writer.img` embeds `piano-309-gpt-write stock|candidate`. It stages the pinned fragments from `/run/pp/gpt-head.bin` and `/run/pp/gpt-tail.bin`, checks the source layout for candidate writes, and verifies both copies with direct reads. Host shell rehearsal applied, rejected reapplication, and reverted the candidate. Qualify it on the tablet only after the stock Recovery return and direct readback checks.

The maintenance image with the packaged checker passed on the tablet. The maintenance writer and stock Recovery remain separate storage gates. See [the packaged preflight result](docs/results-2026-11-21-packaged-gpt-preflight.md).

The frozen maintenance RAM image read both stock GPT copies from unmounted LUN 0 and returned to HyperOS 3.0.309 with the same ABL baseline. See [the maintenance GPT result](docs/results-2026-11-21-maintenance-gpt-preflight.md). The packaged checker, maintenance writer backend and stock Recovery entry still need qualification.

On 2026-11-21 ABL acknowledged the frozen diagnostic RAM image. Menu execution remains unobserved. A subsequent stock ABL check matched the preflight slot and snapshot values and the tablet returned to HyperOS 3.0.309 on slot A. [First diagnostic RAM boot attempt](docs/results-2026-11-21-diagnostic-boot.md)

Updated 2026-11-20. The independent Mac kernel links and passes the 26C5035r mac17j segment layout check. The preflight for the 26-driver profile resolves retained Apple kext pointer targets. The host probe links all 26 configured drivers, including two provisional display objects. A corrected host relink passes the pinned DarwinBoot parser and preserves the source `__LINKINFO` byte for byte. The owner reports HyperOS 3.0.309 was factory-reset immediately before this task and contains no user data; no tablet data backup was made. The objective remains deployment while preserving HyperOS and verifying switching and recovery.

The Mac archive inputs, positioned kernel, isolated KC builder, and three native kext builds are preserved under `out/k27-independent-20261120/host-run/`. Resume from the [driver build result](docs/results-2026-11-20-mac-native-drivers.md). The exact author k27 pin remains unavailable. Keep this independent build on its frozen inputs.

The isolated Mac checkpoint is reachable again. The resumed build verified 98 arm64e objects from 15 of the 26 configured drivers. The Thermal import check passes after an arithmetic compatibility fix. See [the resumed build result](docs/results-2026-11-20-native-driver-resume.md) for the remaining input gaps.

After restoring the family headers and pinned SM8750 kit, the Mac checkpoint has 115 verified objects across 22 drivers. See [the header checkpoint](docs/results-2026-11-20-native-driver-headers.md) for the four remaining build inputs.

The pinned keyboard firmware brought the checkpoint to 23 drivers and 118 objects. The GPU board inputs and firmware matched their pins. Historical ABI 2 display headers also built a separate two-object probe pending target validation. See [the keyboard and GPU input result](docs/results-2026-11-20-native-keyboard-gpu-inputs.md).

The GPU checkpoint now covers 24 drivers and 172 objects, with zero unresolved GPU imports. The display ABI probe and video fixture build are the two remaining driver gates. [GPU build result](docs/results-2026-11-20-native-gpu-build.md)

The video build passed its host suites and import checks. The checkpoint covers 25 drivers and 183 objects. The display declaration probe awaits ABI verification before final collection integration. [Video build result](docs/results-2026-11-20-native-video-build.md)

A temporary KC header experiment passed the pinned DarwinBoot parser after matching the CPU subtype and separating two member ranges. Byte comparison showed the linker overwrote source `__LINKINFO` with Apple `__LINKEDIT` bytes. The corrected relink preserves both ranges, passes the pinned parser, and matches the source payload. The display declaration now matches the independent vma2 regeneration, and the three target vtable comparisons pass. Continue with boot packaging and qualify target storage and recovery. [Collection link result](docs/results-2026-11-20-native-collection-probe.md) and [display ABI result](docs/results-2026-11-20-native-display-abi.md)

The storage study now has an offline GPT candidate and a byte-exact simulated GPT rollback. The 3.0.309 recovery ramdisk was checked for its F2FS and vendor wipe paths. Confirm the root and seed footprints and qualify recovery on a shortened userdata before proposing GPT writes. [Storage and recovery result](docs/results-2026-11-20-storage-layout-recovery.md)

The Mac built and read back a 20,936,916,992-byte SHNGROOT image, confirming the candidate root footprint. Host artifacts and logs are in `/tmp/piano-root-26C5035r.593r2m` on the Mac. This test image lacks the native mount/fsck shims. The three Apple trust modules and pinned BootShim also passed host checks. The A-slot recovery hash matches the stock package. [Root assembly result](docs/results-2026-11-20-root-host-assembly.md)

The resumption audit reconfirmed the upstream device baseline, the preserved root hashes, and the missing shim files. The historical c146 checkout documents an Android switch procedure but no dated Android-return or round-trip result; its exact-pinned kernel binary is absent. The user-supplied historical desktop result remains separate from present 3.0.309 qualification. No tablet command or backup was made. [Resumption audit](docs/results-2026-11-20-resume-audit.md)

The follow-up hash audit tied historical `kc-k273-native.bin` (`663e6ef7鈥) to six RAM build manifests, but all are marked offline/not device tested, use `data_persist=0`, have `same_seed=false` and unknown installed seed, and record no firmware entries. The sibling checkout has no k27 source tree and its pins differ from the current baseline, so the KC remains provenance-only and is not eligible for the current build. See the added provenance section in the [resumption audit](docs/results-2026-11-20-resume-audit.md).

The alternate piano fork audit found a reported device-tested 160 MiB `fastboot stage` + `oem ramdisk` payload publication and working UEFI menu/input, but no successful XNU handoff: its reports stop at FAT read and EL2/EL1 failures, and its README still says XNU has not booted. The fork is an iPadOS 17.7.11 experiment. Its UEFI patch raises a 64 MiB download cap and is not compatible by assumption with our locked UEFI. The 177,512,448-byte historical RAM candidate has now been located by exact SHA-256. Its Android v2 container holds a 166 MiB ramdisk, and its build record requires native UFS root and uses the excluded var-only seed. See the historical RAM container readback in the manifest. See [alternate payload audit](docs/results-2026-11-20-alternate-uefi-payload-audit.md). No device action was taken.

The local iPadOS history points to a same-model upstream home-screen report (iPadOS 17.7.11, touch and A830 Metal), which is bounded hardware evidence only. That repository's separate macOS 27.3 profile uses another UniXNU checkout/XNU lock and its own README reports the kernel does not link or boot because Piano storage/display board inputs are missing. Do not treat its iPadOS result as a native macOS or k27 build result; details are in the [resumption audit](docs/results-2026-11-20-resume-audit.md).

The offline comparison of the official LUN 0 GPT template confirms matching partition geometry after the capacity adjustment, with 34 differing partition GUIDs and four differing attributes. Use the verified device captures for exact GPT rollback. [GPT template comparison](docs/results-2026-11-20-stock-gpt-template-comparison.md)

The strict FD staging audit found an independent filesystem policy callback in Silicium BootManagerLib. Remove it from the diagnostic profile and constrain generic boot dispatch before repeating host verification. [RAM firmware audit](docs/results-2026-11-20-ram-firmware-audit.md)

The diagnostic-only filesystem policy patch passed application and source checks against locked Mu. The staged firmware rebuild passed. The new FD, prior FD and build log are in UbuntuPiano under `/tmp/piano-ram-build.0YIaUj/policy-build.fn26ff0_/`. The packed FVMAIN matches the generated volume byte for byte. The menu container reuses the original shim, DTB and FAT payload with the rebuilt FD and updated section ID. The Ubuntu reference verifier passed with zero failures. Generic BDS dispatch and ABL remain open.

The diagnostic-only BDS patch directs execution to the UniXNU application in the BDS firmware volume and stops if it returns or fails. Exact packed BDS bytes and the RAM boot container passed host checks. The firmware, image and logs are in UbuntuPiano under `/tmp/piano-ram-build.0YIaUj/menu-bds-final.ac77mrn7/`. Stock ABL and menu execution remain unverified.

The stock 3.0.309 archive ABL is fingerprinted, and its ELF hash matches the on-device A-slot prefix. ABL review now uses Qualcomm CodeLinaro tag `KERNEL.PLATFORM.3.0.r23-02400-kernel.0` at `aef4e860` with matching source-file hashes, identified by OpenABL pinned fetch script. That source shows `fastboot boot` can write primary/backup GPT if `ClearUnbootable()` changes the active-slot flag; the previously captured successful A-slot state would make that operation a no-op if still current. Xiaomi binary build provenance/configuration and remaining non-GPT persistence paths remain unverified, so no diagnostic boot was attempted. [Stock ABL inspection](docs/results-2026-11-20-stock-abl.md)

The RAM failure clause in WRITE-PLAN.md now reflects the menu-only BDS halt behavior. Do not expect automatic return to Fastboot if the diagnostic menu fails.

## Current state

The retained strict RAM menu image still matches its recorded SHA-256. The live Android baseline was reconfirmed through Windows ADB on HyperOS 3.0.309, slot A. The newer menu-only FD and container match the manifest hashes. That candidate narrows BDS dispatch to the UniXNU menu. The stock ABL write path and actual menu execution remain open. [RAM firmware audit](docs/results-2026-11-20-ram-firmware-audit.md)

- **User data:** the owner reports that HyperOS 3.0.309 was factory-reset before work began and contains no personal data. This is an owner report, not a new device-side verification. No tablet data backup was made.
- **Upstream:** a 2026-11-20 read-only Git/API check from this workstation confirmed live `main` remains the requested baseline `2438556c77c90c22c7b76e57eb8b40c44b35f1af`; Issue #1 remains an open second-device RAM-validation request and PR #2 an open staged packaging draft. The exact native k27 pin is still unavailable from public commit search, the commit endpoint, and eight listed public forks. The Mac checkout's local `origin/main` ref is divergent at `286f5a594563f863aa448ab21c8d160fdcb95eba`; its 14 commits after the baseline change only six audit/documentation files, not source or dependency pins. Details are in `REPRODUCIBILITY.md`, the dated source/storage result, and the manifest.
- **Maintainer-reported hardware status:** the exact baseline README/docs report a 27.3 desktop and persistent install on the author's tablet. That install is macOS-only, and the README reports a restart panic; neither demonstrates preservation of this device's HyperOS 3.0.309 or the requested round trips. The Mac checkout lacks the locked k27 Git object, has no configured `PIANO_K27_TREE`, and contains no other Codex Git checkout with the external source. See the [resumption audit](docs/results-2026-11-20-resume-audit.md).
- **Alternate upstream evidence:** current UniXNU device records list a separate OnePlus-13T-derived piano port (`donyanchen/UniXNU-piano`, revision `a596eb2e`) reporting staged payload publication and a UEFI menu, but explicitly no captured XNU boot. The separate code002 records are CI-only/not booted. These are research leads, not replacements for this repo's 27.3 source set; details and exact revisions are in `REPRODUCIBILITY.md`.
- **Tablet:** read-only ADB audit confirms `piano`, `SM8750P`, `OS3.0.309.0.WPYCNXM`, Android 16, unlocked bootloader, root available, active slot A. No device writes or reboot occurred.
- **Layout:** all six UFS LUN primary/backup GPT CRCs pass and their entry arrays match. First/last 1 MiB captures from all six LUNs are stored on E: and F:; SHA-256 and offline GPT CRC/array checks pass. These are GPT boundary backups, not disk images. Stock `userdata` fills LUN 0 through its last usable LBA; native `linux_rootfs`, `linux_data`, and ESP are absent. The A-slot boot triplet and `misc` have length-checked, SHA-256-matched copies on E: and F:, with fresh device rereads matching. Read-only Fastboot reported A current, both slots successful, retries 6/6, and partition sizes matching the captures. Normal reboot returned to HyperOS Android 16 build `BP2A.250605.031.A3`, slot A. This does not qualify full recovery.
- **Storage feasibility:** read-only ADB confirmed `/data` is F2FS on `/dev/block/dm-56`; the displayed mount options do not include `resizable_tail_secno`. Both shell and `su` were denied access to `/sys/fs/f2fs/features`; `resize.f2fs` was not found in PATH, `/system`, or `/vendor`. A static audit of the already-existing verified `boot_a.img` found kernel `6.6.118-android15-8-gc44b714366cc` built 2026-06-04, with `CONFIG_F2FS_FS=y`, but no `resizable_tail` markers in the embedded kernel. Upstream f2fs-tools 1.17.0 has a separate offline `resize.f2fs -s` shrink path; its basic shrink/grow path passed on a simple synthetic image, with a checkpoint warning during the first post-shrink check. This is not an Android-format or data-readback test and does not qualify userdata shrink. The USB-host and mass-storage stack is a separate candidate: A2 is built/host-tested but not booted; H6/A3 stick mount, USB root selection, and persistent Data are unverified. More importantly, current root publication is an early UFS path and Data persistence is provided by NativeUfs; USB root+Data requires a boot-architecture change, not just a USB partition selector. Offline analysis of the existing, independently verified E:/F: GPT boundary captures found no free LUN 0 extent and no useful secondary-LUN space; the largest secondary gap is 16,175,104 bytes on `sde`. Thus native partitions still require a proven internal layout or a separately qualified external root+Data architecture. No new device access, capture, or backup was performed. The exact vendor kernel source and userdata feature set are unavailable; shrinkability remains unverified and cannot be assumed. See `docs/results-2026-11-19-f2fs-kernel.md`, `docs/results-2026-11-19-f2fs-tools.md`, `docs/results-2026-11-19-storage-backends.md`, and `docs/results-2026-11-19-gpt-space.md`.
- **Live storage recheck (2026-11-20):** ADB confirmed that HyperOS remains on slot A. The live userdata start and length match the captured GPT exactly. Root could read the F2FS feature list this time. The mounted filesystem reports encryption and inline crypto, and exposes no reserved-tail shrink control. The earlier offline shrink test used an unencrypted fixture. See [the live userdata audit](docs/results-2026-11-20-live-userdata.md).
- **Recovery:** the official Xiaomi 3.0.309 China Fastboot package is at `mac-lan:/tmp/piano_images_OS3.0.309.0.WPYCNXM_20260906.0000.00_16.0_cn_d5071e52da.tgz`. All 32 Range parts matched their exact requested lengths; the 11,346,114,406-byte archive matches published MD5 `d5071e52da7c5dff4479d45e2775c437`, SHA-256 `c7b0446a4704611d1d562e3b27cdd01fdd14bdee7b979973b76992c2200836e8`, and a complete tar read listed 110 entries. Static review found `flash_all.sh` erases/flashes `metadata` and writes `userdata.img`; `flash_all_except_storage.sh` skips those but erases `boot_ab`, rewrites firmware, `super`, boot chain, `misc` and recovery partitions, sets slot A and reboots. Neither script was run or qualified as a safe recovery flow. The archive is verified; application/recovery procedure is not. It is not a userdata/personal-data backup. The prior external backup remains untouched and is not treated as a verified userdata archive: its separate DCIM and Pictures archives parse (5519 and 571 entries), but are not compared file-by-file with the tablet; the 52.8 GB `data-data.tar` has a 39-byte warning prefix and about 52.6 GB unexplained after the parser's end marker. No new tablet data backup was made or requested.
- **Historical deployment evidence:** preserved checkout `piano-macos-c146` at `c146dfc6` reports native 27.3 desktop and persistent UFS/Data success on 2026-11-13/14. Current device GPT has no `linux_rootfs`, `linux_data`, or ESP, so this is historical, not current qualification; how the device returned to stock Android is unknown. Its Git database lacks the pinned `2438556` commit and its lockfiles differ. Sibling checkout `piano-macos-latest` has 6.25 GB of k27/RAM build outputs and limited generated inputs, but no k27 source tree, persistent root or ESP; its lockfiles also differ. Its `artifacts/ARTIFACTS.md` records an exact-pinned `kernel-k27-0054-native.macho` fingerprint (`20a5b4fd...`), but the binary is not present in the preserved output tree, so this is provenance evidence only. One RAM image is explicitly offline-only, `data-persist=0`, and uses a mismatched seed. No historical outputs are being used. The `macos27` files in `tmp.tar` are explicitly launched with `crosvm --hypervisor gunyah` and are DroidVM guest assets, not native boot inputs.
- **DroidVM success records:** the preserved `macOS-DroidVM` local notes report macOS 27.0.1 (`26A434`) desktop/login, GPU/Metal use, Safari/Bilibili video playback on 2026-11-04 (`23` sessions; `in=2131`, `out=2111`, `dropped=20`), and user-confirmed audible, synchronized playback on 2026-11-05. This is useful implementation and diagnostic evidence from Android/Gunyah/crosvm; it is not native Pad boot evidence or validation of the 27.3 device source set. The same notes record unresolved microphone, scheduling, and GPU/resource limits. Details and bounds are recorded in `REPRODUCIBILITY.md`.
- **Build/assets:** Apple `26C5035r` root/SystemOS AEA and decrypted DMGs, mac17j kernelcache, k114cap DTB, and SystemOS trust cache were found under `D:\Download\piano-macos-273-inputs`; their SHA-256 values match adjacent IPSW-member JSON records. The full IPSW, exact local k27 pin `11ac316b`, prepared XNU tree `de16623d`, matched native Data seed/root, full firmware/calibration set, and signing profile remain unavailable. WSL legacy k27 trees are dirty and off-pin; their RAM manifests say offline-only and `same_seed=false`, so none are reused. Mac has Python 3.9.6 (an isolated 3.11.18 runtime exists); NASM 3.02 and ACPICA iASL 20260930 are now built and locally tested in `/tmp/piano-host-tools-20261119`. Native image build has not started because the locked source and matched image/signing inputs remain missing.
- **Native candidate gate:** a disposable checkout replayed all 47 public DroidVM patches on the verified XNU base, then failed the first native `0053` patch check in `bsd/vfs/vfs_subr.c` and `osfmk/arm64/machine_routines.c`. The VM replay is therefore explicitly rejected as a native substitute. See `docs/results-2026-11-19-native-candidate-gate.md`.
- **Historical native evidence:** preserved `piano-macos-c146` results report native 27.3 desktop and persistent UFS/Data success on 2026-11-13/14. Current live GPT is stock Android with no native partitions; the transition back is unknown. This is distinct from the DroidVM 27.0.1 guest notes and neither validates the requested baseline or current 3.0.309 state.
- **Switching:** the 2026-11-20 release/API check confirms Ubuntu v1.6.1 (published 2026-11-18) remains latest; `tools/piano-switch.py` is unchanged at blob `c504b603鈥, last modified in `ec551c5`. Its release targets 3.0.303 and its installer erases Android data, so its installation assets are excluded for 3.0.309. The previously verified archive is reused; no download was made. Fastboot preflight fields and partition sizes were confirmed on this tablet; success/retry values were queried separately (yes, 6/6). No matched macOS image set or 3.0.309 recovery set exists; the external script lacks success/retry gating and write readback, so it has not been run. The repository's read-only `abl-final` gate requires slot A current, successful and bootable, and its retry count valid (0-7) and unchanged from `abl-check`; host-only fake-response tests pass. This does not qualify the external switching tool or authorize any device write.
- **Processes:** the Windows host has two existing ADB service processes and no build/VM process. The Mac has no matching compiler, QEMU/CrosVM, or Fastboot process. No build or guest is currently running.
- **Interrupted collection:** at the user's direction, no tablet backup is to be made. An ADB `/data` archive attempt was interrupted; the only matching files are identical 4,202,692,608-byte partial outputs at `E:\piano-android-backup-3.0.309-20261119\userdata-files.tar` and `F:\piano-android-backup-3.0.309-20261119\userdata-files.tar`. They are incomplete, are not recovery copies, and must not be resumed, read as a backup, or used for restoration. Process inspection found no active ADB transfer/capture command; two ordinary ADB server processes remain. The partial files are preserved in place and have not been deleted.
- **Validation:** Mac host checks passed on 2026-11-19: include/SPMI, bootargs, guard and PM suites, native-kext-rule and Metal bundle-path self-tests, userland staging, Data-store, and 27.3 ESP/maintenance fake-tablet tests. Python-dependent tests used isolated CPython 3.11.18 at `/tmp/cpython311-3.11.18`; persistence tests used the exact locked UniXNU checkout `d830d23` at `/tmp/codex-unixnu-d830d23`. GNU mtools 4.0.50 was built from a previously signature-verified GNU archive in `/tmp/codex-mtools-4.0.50-src/install`; it enabled the Data-store ESP pin subtest. Staging still skips tests 4鈥? because the locked k27 tree is absent. No source pin changed. No macOS image was built or booted; no device feature or switching result is claimed.
- **Host validation rerun:** On 2026-11-19, input/RTC, NativeRamBlock, include/SPMI, bootargs, guard, PM, native-kext self-test, and `test-k273-only.py` all passed on `mac-lan`; the latter ended `PASS: 0 failure(s)` with the existing CPython 3.11.18 and mtools 4.0.50 prefixes. This remains host/fake-tablet evidence only. See `docs/results-2026-11-19-host-tests.md`.

## Milestones

1. **Read-only baseline and source lock:** device identity and upstream commit confirmed; all six GPTs validated; A-slot boot triplet and misc read-only captured twice and reread-verified; Fastboot slot state/capacities read and normal Android return confirmed. Full recovery qualification remains open.
2. **Reproducibility and migration reports:** first versions created in this change; missing inputs and stock-to-upstream layout differences recorded.
3. **Complete rollback set and write design:** the official 3.0.309 OS package and GPT-boundary recovery captures are verified. The owner reports userdata is empty; this has not been independently re-read. A concrete partition layout and write-specific recovery sequence remain open. No userdata backup is planned.
4. **Kernel and matched build:** the independent Mac kernel and segment layout pass. Its frozen driver checkpoint has 183 verified objects across 25 of 26 fully built drivers; a separate two-object display probe has host ABI evidence. A host-only 26-descriptor KC probe passes the pinned DarwinBoot parser after relinking, but its source is not verified against native pin `11ac316b`, and it has not been loaded on-device. The locked source, mount/fsck shims, matched Data seed/trust, and qualified root layout remain open.
5. **Diagnostic boot and desktop qualification:** upstream maintainer reports same-model 27.3 desktop success, but no current-device boot; resolve the locked source/root dependency and restart failure first.
6. **Persistence, switching, recovery and daily-use acceptance:** not started; requires an approved concrete write plan and paired image sets.

## Next actions

- The [draft WRITE-PLAN](WRITE-PLAN.md) records the offline LUN 0 ranges and the limits of GPT rollback after F2FS tail overwrite. Final image hashes and stock GPT rollback qualification remain pending.

- Host reproducibility update: GNU mtools 4.0.50 is built in WSL and in a temporary Mac prefix; on the Mac, `test-data-store.py` and `test-k273-only.py` both pass with their ESP fixture paths enabled. The persistence suite's remaining red cases are Linux/test-fixture limitations (`F_NOCACHE`, fake sysfs size, and expected backup-stop continuation), not tablet results. Mac host checks passed on 2026-11-19 under isolated CPython 3.11.18. Rerun staging tests 4鈥? once the locked k27 tree is available. See `REPRODUCIBILITY.md` for test-by-test results.
- F2FS storage research: upstream f2fs-tools `83fd39a` offline shrink compiled and passed a basic synthetic shrink/grow exercise in `UbuntuPiano`; a checkpoint warning and lack of payload readback/crash testing limit the result. The tablet's Android feature set is not matched, so no device shrink or WRITE-PLAN follows from this host test. See `docs/results-2026-11-19-f2fs-tools.md`.
- External storage research: USB mass storage is included in a gated host-stack build, but its first tablet probe and stick-mount test remain unrun; NativeUfs root and NativeRamBlock persistent Data still depend on UFS. Do not treat USB as a current no-GPT solution. See `docs/results-2026-11-19-storage-backends.md`.
- k27 source audit update: the device lock requires local pin `11ac316b789d81fc9f734e15645456f536c05141`, branch `el1/native-variant`, XNU baseline patches `0001-0054`, prepared tree `de16623d957cccff6dfb78d8eca4a79842af9702`. Public DroidVM refs checked are `main`, `macOS`, and `wip/hackintosh`; the exact commit is unavailable through the upstream API, global commit search, or any of the 9 public forks checked. A full read-only `git fsck` on the preserved partial clone found 260 unreachable commits, including `71405bb` (2026-11-10), which re-exported a 49-patch, single-volume VM k27 series. Its own README identifies it as the macOS 27.2 Gunyah VM tree; it is not the locked native-variant source. The complete locally present object listing did not contain either locked `11ac316b` or prepared-tree `de16623d`, and promisor lookup confirms they are not available from the configured remote. The clone's sparse checkout includes `configs`, `k27`, `overlay`, and `scripts`, not `patches` or `sources`; the separate current public replay at `524d27b` also remains unsuitable. On `mac-lan`, `~/.config/piano/env` and `PIANO_K27_TREE` are absent; a bounded home/Codex search found no external k27 source tree. The identified Mac checkout under `~/Codex` is the clean device repo at baseline `2438556`.
- Alternate piano upstream follow-up: the public catalog now includes a OnePlus-13T-derived piano project whose documented Fastboot-staged payload reaches the firmware menu, but whose current record has no captured XNU handoff; it targets iPadOS 17.7.11 and its own UEFI/kernel pins. The code002 kernel project is CI-only and its other piano repo is still marked not booted. Compare its payload transport with the pinned UEFI only if source diffs establish a relevant ABL fix; do not substitute its artifacts or change the lock.
- DroidVM source reconstruction: exact public `main` snapshot `524d27bf57ae92375aa39e3a81e11727c08973ff` was downloaded to `D:\\Temp\\DroidVM-macOS-DroidVM-524d27bf.tar.gz` (SHA-256 `D3B0C2A3C0C657C09D87673CAD1E7E3A7989EABFAA9B93E0E510B5F5DFB87F8E`). All 48 numbered patches replayed cleanly with `git am` onto the verified Apple base archive (`D:\\Temp\\xnu-ac9718.tar.gz`, SHA-256 `577f4a220304296332e28c802779e7b5e6a27a46812315aacf61c0269a03e12a`); temporary Git tree `3faa7ce551e462a93f34808eaf78549768c67f5a`, 49 commits including synthetic archive baseline, clean status. The earlier `9b31c23` patch archive remains a separate failed attempt at 0008 due its `kern_exec.c` preimage expecting `XNU_NATIVE_RAM_BLOCK` / `XNU_OWNER_DATA_VOLUME_UUID`; current 524d27b patch 0008 has a different preimage and applies. This reconstructs the public DroidVM series only, not device pin `11ac316b`.
- Local DroidVM-success source candidate: `D:\Temp\k27-reconstruct-20261119-b\patches\README.md` explicitly identifies its 27.0.1 tree as a k27 VM build. The associated XNU checkout is clean at `17f4433a795587e8d8b468c14bf05218e7f35d1d` (tree `7373bc69be30fe86af90bedb1324ec87aced9a17`) and sits on parent `2303f45443b49c0ae52f29fec18566a841393941`; the directory has 48 patch files, including `GUNYAH_LINUX` build paths and DroidVMDisplay/GPU references. The locked native commit `11ac316b789d81fc9f734e15645456f536c05141` is absent. This is the source context for historical DroidVM successes, not a native source candidate; see `REPRODUCIBILITY.md` and the manifest.
- Follow-up k27 lookup (2026-11-19): GitHub's current fork API returned 8 public DroidVM forks; exact-commit API lookup for `11ac316b789d81fc9f734e15645456f536c05141` found it in none. Current upstream refs remain `main` (`524d27b`), `macOS` (`854aac2`), and `wip/hackintosh` (`f7d070f`); the all-state PR query returned 0. An isolated bare-repository `git fetch` by the full commit hash also returned `upload-pack: not our ref`. This adds no source candidate and does not change the native source lock. Exact fork names and query scope are recorded in `docs/results-2026-11-19-k27-lookup.md` and the manifest.
- Historical asset follow-up (2026-11-19): an additional read-only Git-object scan of the retained `E:\codex\2026-11-01\ni\work` checkouts, including `downloaded-repos`, `piano-macos-c146`, `piano-macos-upstream-main-49c589`, and `sources/UniXNU-xiaomipad8pro-latest`, found neither the native k27 pin nor its prepared tree. Existing k27 patches and outputs remain provenance only.
- Blocked gate (2026-11-19): the required k27 commit `11ac316b789d81fc9f734e15645456f536c05141` and inner XNU commit `de16623d957cccff6dfb78d8eca4a79842af9702` are absent from this repository's object database, the checked reconstruction repositories, configured Mac search paths, live public refs, all 8 current forks, PRs, and direct Git object fetch. The 26C5035r Apple members and Mac host compiler are present, but no matched native root/Data layout is qualified. For exact upstream reproduction, resume when the exact k27 source/archive or an authoritative new upstream ref is available; verify its hashes and patch base before build. No private signing key or tablet backup is requested. Device-write gates remain unchanged.

- UEFI upstream comparison (2026-11-19): piano-uefi `main` `5b0ddb7` adds an 11-file fastboot payload path. Its Mu-Silicium pin `e51cd87` matches our lock, but the current builder uses separately pinned `uefi-sheng` BootShim `66421cb` and carries UniXNU's FAT payload in the Android boot ramdisk. Upstream instead patches Mu's BootShim to append/carry that payload and forwards it through x2/x3, SEC's HOB and `/chosen` initrd. It is a distinct integration, potentially relevant to the reported ABL ramdisk replacement, but not a drop-in pin update. No device validation or lock change. The old checkout's unrelated local `piano.dsc` modification remains untouched.

- Keep the A/B successful/retry values as a timestamped Fastboot observation; recheck only before any future switching plan because bootloader state can change.
- Do not collect or back up tablet data. Treat the two interrupted `/data` outputs above as unusable partial files; continue only with host-side and public-source research.
- Historical c146 search: the deployment procedure documents an Android boot-set exchange, but no result captures that exchange or the return-to-stock event. Its exact-pinned kernel binary is absent; the sibling's RAM outputs are not a persistent image set. Keep the event unknown and do not infer compatibility with HyperOS 3.0.309.
- The exact Ubuntu v1.6.1 `piano-switch.py` was compared with its public release blob. Before any device use, add success-bit/retry gating and readback to a matched switching implementation and test both directions on the host.
- Determine whether the exact 27.3 Fastboot boot image receives an ABL-replaced ramdisk; if so, prototype the `5b0ddb7` payload handoff against the existing `uefi-sheng` build inputs or use the matching Mu BootShim path, then require a reproducible host build and non-persistent diagnostic-boot plan before device testing.
- Continue searching for the locked k27 tree only if a new source location or ref appears; current checked local/public sources and direct Git fetch do not expose it. The exact 27.3 Apple IPSW-derived members are already present and SHA-256-verified under `D:\Download\piano-macos-273-inputs`; do not download duplicates. The complete IPSW archive is absent, but this is not the active native-build blocker. Matched native root/Data inputs and device signing configuration also remain unavailable; no private key is requested or copied.
- Compare the verified XNU base snapshot and the replayed public DroidVM series against the locked k27 build recipe; keep them out of the device build until equivalence to pin `11ac316b` is established.
- Request user physical interaction only if access to the Mac developer-tools installer or tablet bootloader is required; continue independent source and recovery research meanwhile.

See [DEVICE-BASELINE.md](DEVICE-BASELINE.md), [REPRODUCIBILITY.md](REPRODUCIBILITY.md), and [MIGRATION-DIFF.md](MIGRATION-DIFF.md). The dated read-only device record is [docs/results-2026-11-19.md](docs/results-2026-11-19.md); the machine-readable source lock is [outputs/manifests/native-macos-27.3.json](outputs/manifests/native-macos-27.3.json).

- The [RAM firmware audit](docs/results-2026-11-20-ram-firmware-audit.md) found UFS fallback during configuration discovery. Qualify variable storage and the final FD's UFS behavior before a RAM boot under the no-persistent-write authorization.

- The independent WSL FD build succeeded with the populated pinned EDK2 feed. Its SHA-256 is in the deployment manifest. Generated VariableRuntimeDxe settings show emulation enabled and a zero reserved-store address in the staging build tree. Device qualification and RAM-only configuration enforcement remain open.

Ubuntu v1.1.0 RAM reference archive verified at `D:\Temp\piano-v110-ranges-bc75e4c7-38d0-4bd0-8eb7-f04886abad71\verified.zip`. The UEFI-only candidate passed `verify-ram-images.py` with zero failures under UbuntuPiano. Next gate is firmware storage isolation. The locked native k27 input is still missing.

The pinned piano platform sources show early dispatch of binary UFSDxe, ESP and Android fallback outside the UniXNU menu, and a reset callback that flushes present writable Block I/O devices. Treat the existing candidate as host-only evidence pending a separately verified diagnostic build with physical storage dispatch disabled.

The isolated RAM diagnostic FD built under UbuntuPiano at `/tmp/piano-ram-build.0YIaUj/deps/Mu-Silicium-piano/Build/pianoPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`. SHA-256 is `0a6a0cd3cc38dff2cfb0d6fe687396fe2a4cd036a5d84284aa4cf0091eacb00e`. The fifth build log and the firmware volume manifests remain in the isolated workspace. Verify the packed volume against the generated manifest and review remaining fallback and reset behavior before assembling any device test image.

The connected tablet answered ADB with product `piano`, build `OS3.0.309.0.WPYCNXM`, slot `_a`, and `sys.boot_completed=1`. The stock shell did not expose `bootctl`, so neither slot success bits nor retry counts were obtained from Android. The diagnostic FD now has packed-volume verification. Its retained `PianoDtbInstall()` still enumerates Simple File System handles, so check the source of those handles before authorizing a temporary boot.

The menu-only revision removes PianoDtbInstall and its filesystem scan. The rebuilt FD in the same staging path has SHA-256 8a0a2e8cf2a6c8f23fbdbed08d8146c97000bde095400a62cb5705cdd4669c87. Its packed volume matches FVMAIN. Device behavior remains unverified.

Menu-only image: /tmp/piano-ram-build.0YIaUj/menu-image.KdcWfS/boot-piano-uefi-ram-menu-isolated.img. SHA-256 e730e57e14a050634ad9ae3c68ce5aea0639e7b45e9b94db3508f3dbafa7e577. The Ubuntu reference verifier passed with zero failures. Generic boot option discovery and reset remain under review before tablet testing.

The current diagnostic build omits the Qualcomm extra application include. The rebuilt image is /tmp/piano-ram-build.0YIaUj/menu-no-extra.IYarRw/boot-piano-uefi-ram-menu-isolated.img. Its manifest and verifier log are in that directory. Generic boot option processing and reset still require review.

The --diagnostic-menu image at /tmp/piano-ram-build.0YIaUj/menu-actions.04qt9t passed the Ubuntu reference verifier. Its FAT configuration exposes restart and shutdown only. Built-in fallback and generic BDS are still pending review.

The strict RAM configuration build in /tmp/piano-ram-build.0YIaUj/menu-strict.yAtgPM passed the Ubuntu reference verifier. Apply firmware/diagnostic/ram-config-only.patch to the pinned UniXNU checkout after the ordinary core patches. The menu connects only the RAM payload block handle and skips filesystem EFI fallback discovery. The generic firmware BDS path and tablet behavior remain unverified.

The official 3.0.309 boot triplet has been extracted and compared with the retained A-slot captures. Boot and vendor_boot match. The stock and captured init_boot ramdisks differ in content and length. See docs/results-2026-11-20-storage-layout-recovery.md before choosing a rollback input.

Official recovery scripts were fingerprinted without execution. The `flash_all_except_storage.sh` asset is a broad system/firmware restore that erases `boot_ab` and reflashes both slots' boot inputs, `super`, firmware and recovery, but leaves GPT, userdata and metadata untouched. It is not a triplet rollback or GPT recovery. `flash_all.sh` also erases/flashes metadata and writes userdata.img, so it is excluded. Exact details and hashes are in WRITE-PLAN.md, the deployment manifest and the storage recovery result.

The maintenance manual-exit candidate passed host inspection. See [the dated maintenance result](docs/results-2026-11-20-maintenance-manual-exit.md) for the idle restart findings and remaining ABL and hardware checks.

Android userspace Fastboot returned `snapshot-update-status=none`, slot A, successful=no and unbootable=no. Retry count was unsupported in that mode. Fastboot acknowledged reboot. The default Windows ADB server listed no device even though Windows enumerated the WinUSB interface. A separate ADB server on port 5038 with `ADB_LIBUSB=1` enumerated the tablet and confirmed product `piano`, build `OS3.0.309.0.WPYCNXM`, slot `_a`, `sys.boot_completed=1`, and `/data` mounted as F2FS. The separate host server was stopped after verification. The maintenance tool exposes a separate read-only `snapshot-check` step for this transport.

Stock ABL Fastboot reported `is-userspace=no`, `snapshot-update-status=none`, slot A, successful=yes, unbootable=no, retry=6. Standard Fastboot reboot returned to `OS3.0.309.0.WPYCNXM` on `_a` with `sys.boot_completed=1` verified over ADB. This was a getvar session without a downloaded boot image.

The ABL preflight and final checks now require `is-userspace=no` and `snapshot-update-status=none` alongside slot checks. UbuntuPiano fixtures passed for clean ABL state and rejection of userspace transport, active merge and missing status.

The historical 4 GiB DBRDISK1 seed passed host structure and identity checks. Both the file digest and seed identity match 28 historical manifests, all marked offline with Data persistence disabled. Its store needs 1,048,609 blocks, or 5,368,844,288 bytes with the 1 GiB reserve. This qualifies the footprint only. Keep the seed as a provenance lead until the target profile and trust inputs are matched.

The historical 4 GiB seed was expanded into a sparse host copy at `/tmp/piano-seed-review.HVxAnP/seed.raw` and mounted read-only on the Mac. APFS mounted one volume named Data (UUID `98FD0A57-D19D-4C68-815D-60C6CF1459EB`). Its visible root contains var-style directories such as `db`, `mobile`, and `tmp`. Probes found no top-level `Users`, `Library`, `System`, or `private/var`. This confirms host readability and records the layout for comparison with the native mount contract. The image was unmounted and detached. Native mount routing and desktop suitability still require verification.

The pinned UniXNU `d830d23` owner Data patch mounts at `/System/Volumes/Data` and requires a successful `/private` firmlink. The retained 26C5035r OS image lists `/private`, `/Users` and `/Library` with same-named targets inside Data. A read-only mount of the historical 4 GiB seed shows a var-style root without those directories. Exclude that seed from the full-volume desktop profile. The pinned `scripts/make-macos-data-seed.py` documents the compatible template layout. An existing seed with matching provenance or a separately identified first-install seed is still required.

Built a separate 24 GiB first-install Data candidate on Mac with UniXNU `d830d23`, DarwinBoot `8544957` packed_ramdisk and the 26C5035r OS template. The build required the pinned packer on PYTHONPATH and creation of the ignored build scratch directory. No account or Setup Assistant override was passed. The packed file is at `mac:/tmp/piano-seed-review.HVxAnP/first-install.dbr`. A read-only APFS mount verified `private`, `private/var`, `Library` and `Users`, then detached. The candidate fits the proposed 64 GiB region, including the overhead and reserve. Its identity is in the manifest. Preserve this seed as a separate first-install input pending a matched kernel, SystemOS and native boot qualification.

The verified packed first-install seed also lives at `out/inputs/k273/first-install/scratch-data.img` in the Windows workspace. The packed SHA-256 and NativeRamBlock seed identity match the Mac copy. Use this preserved input when assembling the matched first-install set.

The 27.3 ESP builder supports an explicit target `linux_data` extent and derives its store LBA from that extent plus the seed size. Host integration tests pass for the provisional 64 GiB region and reject an undersized extent. The ordinary maintained-layout behavior remains unchanged when no extent is supplied.

The Qualcomm ABL reference computes the ramdisk and DTB relocation addresses from the reserved kernel window and actual payload size. The historical v2 header address gap does not establish an overlap. The Xiaomi binary path and its reserved window remain open. See [ABL relocation audit](docs/results-2026-11-20-abl-relocation.md).

The retained OS image supplies the original mount, fsck and fsck_apfs binaries. The staged SystemOS contains a byte-identical mount.real and lacks the other three paths. See [SystemOS boot helper comparison](docs/results-2026-11-20-systemos-helpers.md).

The independent kernel's pre-launchd owner root check does not mount SystemOS where the pinned reference does. Check the complete boot path for equivalent mount behavior before pairing that collection with the composed root. See [SystemOS startup audit](docs/results-2026-11-20-systemos-startup.md).

Both pinned SystemOS patch variants fail the independent source apply check. The verified volume resolver and Data setup are absent from the inspected directories, and manifest publication has a different interface. See [SystemOS patch compatibility](docs/results-2026-11-20-systemos-patch-compatibility.md).

The independent positioned kernel lacks the owner startup symbols checked with nm. Its conditional link compatibility functions also differ from the authenticated storage path required by the Piano configuration. See [owner startup configuration audit](docs/results-2026-11-20-owner-startup-configuration.md).

A retained Piano driver submodule contains the three verified volume interfaces and calls the UFS provider during media lookup. Its exact source identity and the remaining verified-root build dependency are in [the volume resolver audit](docs/results-2026-11-20-volume-resolver.md).

The retained VerifiedRoot kit passed all seven host test groups under UbuntuPiano. The report and test scope are in the volume resolver audit.

The host SHNGROOT manifest generator can now describe v2 policy table objects such as OS.vroot. Fixture checks verified v1 and v2 encoding, byte lengths, hashes and duplicate rejection. The 27.3 image assembler still emits v1 and the deployment still needs signed policies paired with the final root volumes.

The 27.3 assembler accepts paired OS.vroot and SystemOS.vroot objects through K273_VROOT_DIR. It requires a fresh root image and omits historical write coordinates for this layout. Host fixture readback matched all four ranges. Final signed policies and deployment volume integration remain pending.

The Mac generated the unsigned 113229824-byte OS.vroot table for the hash-matched 26C5035r OS input. Its SHA-256 is f10b77a0b2f0475c296a66c69ec3c1d26598dbca3e6b09751a0b1cc31af533b0. The file is at /var/folders/s0/0m27h1wx2_v4zz11d66_hpxm0000gn/T/piano-vroot-os-rdvwczz0/OS.vroot. Generate the SystemOS table from the final shimmed volume. The currently measured windows and both projected tables total 21100494848 bytes, leaving 374341632 bytes in the proposed 20 GiB root partition. Recalculate with the final SystemOS image.

Mac host assembly passed for the 21,100,494,848-byte SHNGROOT v2 image at /tmp/piano-vroot-layout-yiktynqv/root.host-test.img. The manifest and all four ranges passed readback. The companion verified.json records sizes, offsets and hashes. This image uses the unshimmed SystemOS and unsigned hash tables for host layout verification.

NativeUfs production findRootLayout accepted the actual Mac SHNGROOT v2 header in an UbuntuPiano host harness with a synthetic 20 GiB linux_rootfs GPT. It classified OS and SystemOS as windows and both vroot tables as objects at their expected offsets and lengths. Harness and input are at /tmp/tmp.CyAC1QKrpJ.

On the Mac, the retained VerifiedRoot signer generated an RSA-3072 policy for the actual 26C5035r OS volume using a host fixture key. OpenSSL verified the signature and the signed table SHA-256 matches the previously generated OS.vroot. The report is /tmp/piano-os-policy-test.4cuYw7/signed/manifest.json. The fixture anchor is for host qualification only.

The same host fixture anchor also signed the unshimmed SystemOS volume. OpenSSL verified the signature and the generated table matched the assembled image table byte for byte. The paired host-only reports are in /tmp/piano-os-policy-test.4cuYw7/signed and /tmp/piano-os-policy-test.4cuYw7/systemos-signed.

The Mac assembled both 544-byte fixture policy envelopes and their shared public anchor into an arm64e policy object. The required three symbols, the two pointer relocations, and a relocatable link passed. The object is /tmp/piano-policy-object.3k6bzo/policies.o with SHA-256 d12109bf41f59bbbfeca3adbffc3debc264f795b45fa2df69c3a98dd823c65a1. The verified NativeUfs kext and kernel collection link still require qualification.

The Mac compiled a separate NATIVE_VERIFIED_BLOCK=1 NativeUfs probe for linux_rootfs and linked it with the paired fixture policy object using the arm64e kernel ABI. The temporary compiler wrapper downgraded two unused-function diagnostics. The three policy symbols resolve. vr_open, vr_verify_block and vr_xnu_crypto still require the VerifiedRoot implementation and XNU adapter at collection link. Logs and objects are in /tmp/piano-policy-object.3k6bzo.

The retained VerifiedRoot C sources and XNU adapter compiled with the independent kernel recipes. The combined arm64e object /tmp/piano-policy-object.3k6bzo/ufs-verified.o resolves the policy and vr imports and hashes to df7d1cf7da92b2983ac275ebbdbc9edc61d2ba951023ae29f00e9dd1dfd41f43. This is a fixture relocatable link. Collection integration and the owner startup path remain open.

Compared the verified UFS probe against the development object and positioned kernel. Twelve of thirteen added imports are present in the positioned kernel. The remaining import, boot_progress_poll, is a weak reference with an arm64e authenticated-pointer relocation. The import lists are in /tmp/piano-policy-object.3k6bzo. The collection linker still needs to prove it handles that reference or resolve it through the existing boot progress provider contract.

The isolated collection relink stopped before NativeUfs relocation at the Apple import _os_log_coprocessor_with_handle, both with the pinned stock KC and with the standard codeless normalization. Logs and descriptor are in UbuntuPiano /tmp/piano-verified-collection.68bZb6. Recover the exact prior relink inputs before comparing NativeUfs variants.

Restoring ARMPlatformStandIn to the collection probe resolved the RTBuddy dependency. The linker then confirmed that the weak boot_progress_poll import needed a provider. A host fixture with a no-op provider linked NativeUfs with zero IOBlockStorageDevice vtable differences and passed the DarwinBoot parser. The two-driver fixture at UbuntuPiano /tmp/piano-verified-collection.68bZb6/host-only.kc hashes to 4cbc76f2427754994f5564a971d1b50fa1b88e82dc295d0db8a4c9754cab295a. It uses fixture keys and the unshimmed SystemOS policy.

The full 26-descriptor host fixture linked and passed the locked DarwinBoot parser and its synthetic tests. Readback confirmed SHA-256 375b54bc4b473dec14e3240531c5b57a75ab5596bea9ebd36a2235e00e4a309d and 26 linked entries in full-host-link.log. UbuntuPiano retains the collection, full-descriptors.txt, HostNativeUfs.json, and archive/inventory.json under /tmp/piano-verified-collection.68bZb6. It uses a fixture signing anchor, unshimmed SystemOS and a no-op boot_progress_poll. The production progress provider still needs identification. No tablet test was performed.

The GitHub API reported the device upstream main at 2438556c77c90c22c7b76e57eb8b40c44b35f1af, matching the deployment baseline. No pins changed. At the locked UniXNU core d830d23b0d19c9322d906c21afeca1d6c54aeffe, both zzz-boot-progress.patch variants declare boot_progress_poll when enabled and use a macro when disabled. Neither provides the function body. NativeUfs calls it after successful block verification. The fixture establishes the link contract only. Check the prepared XNU instrumentation and its build inputs for the production provider.

The locked core's build-xnu-4k-baseline.sh names the optional provider libBootProgress.a. XNU_BOOT_PROGRESS=1 invokes scripts/build-boot-progress.py and force-loads the archive. Neither input was found in the retained UbuntuPiano dependency tree or its /tmp inventory. The independent Mac checkpoint records 0:disabled. This explains the absent export and identifies the expected provider for an instrumented build.

The frozen independent XNU config enables union mounts and already calls the owner preparation hook before launchd. The device NativeRamBlock also supplies the typed Data lookup ABI. See [SystemOS union and Data provider checks](docs/results-2026-11-20-systemos-provider-checks.md) for the paired manifest and APFS request checks still required.

The paired boot manifest publisher compiled on the Mac with the frozen arm64e recipe. Update its caller and definition together when integrating the authenticated SystemOS mount. See [paired manifest compile probe](docs/results-2026-11-20-paired-manifest.md).

The positioned kernel host link passed with a one-time NativeUfs authenticator registration interface. The direct link identified 43 Apple storage imports that require NativeUfs to remain a collection member. See [paired manifest and kernel link results](docs/results-2026-11-20-paired-manifest.md).

The 26-driver host collection now links a NativeUfs module entry that registers the authenticator. The locked DarwinBoot parser passed. See [paired manifest and kernel link results](docs/results-2026-11-20-paired-manifest.md) for the build and remaining runtime gates.

The host collection now includes the early Data mount hook for the retained 24 GiB seed. All 26 descriptors linked and DarwinBoot parser tests passed. The seed callback target still requires relocation verification. See [paired manifest and kernel link results](docs/results-2026-11-20-paired-manifest.md).

The deployment manifest now references owner-dispatch.kc with the early Data mount interface. Host parser success does not establish Data mount runtime or write-path safety.

Correction: the upstream macOS 27.3 owner-data-firmlinks patch supplies the required Data mount point, cross-device firmlinks, and cryptex view. Our prior /private/var host probe omits this required path and must not be treated as a desktop candidate. The patch has not yet been qualified against the prepared XNU source.

The corrected macOS Data mount and firmlink lookup compiled and linked into owner-firmlink.kc. DarwinBoot parser tests passed. See the paired manifest report for the artifact and pending cryptex integration.

The 26-driver owner-cryptex.kc host collection passed DarwinBoot parser tests. Its macOS cryptex helper creates mount directories under Data and requests read-only bind mounts. The bindfs boot exception requires PID 1 and MNT_RDONLY. The Mac source and recipes are under /private/tmp/piano-cryptex-uk1_iur7. GPU add-on integration and runtime mount validation remain pending.

Read-only Mac inspection found no NativeAdrenoMetal or freedreno files in the retained first-install Data seed. The historical r80 bundle's gpu-build.json identifies iOS and a740. The device recipe stages a macOS Adreno830 ABI v2 bundle and its trust entries into a Data seed copy. The retained build trees checked on Mac and UbuntuPiano lacked the pinned bridge archive and Mesa worktree. The upstream add-on path additionally requires a GpuAddon ADT slot and a provider absent from the device NativeRamBlock.

Read-only ADB recheck confirmed HyperOS OS3.0.309.0.WPYCNXM on slot A. Android exposes userdata on sda34 and no linux_rootfs, linux_data or esp partition aliases. Access to /proc/partitions was denied, so this observation does not establish GPT geometry. The UFS SystemOS boot path still needs prepared storage.

The retained 26C5035r launchd binary contains embedded mount commands for the two APFS phases, root remount, and mount-all, plus fsck references. Apple's mount binary points to the APFS filesystem bundle's helper. The locked UniXNU helper passed arm64 host phase checks and arm64e static signing inspection. Integration still needs the boot job predicates, fsck arguments, and visible helper paths in the composed root. See `docs/results-2026-11-20-launchd-boot-dispatch.md`.

The embedded 26C5035r launchd configuration defines four required-success tasks for mounts and filesystem checks. Its fsck arguments are `-fy` and `-qL -R 1`. The affirmative repair path needs write review before a temporary boot. See [embedded boot task definitions](docs/results-2026-11-20-launchd-boot-tasks.md) for the exact flags and remaining runtime checks.

The Mac's `fsck_apfs -n` completed with status 0 against both retained read-only root windows. The Apple OS check reported a newer APFS writer version. See [the APFS check and helper inventory](docs/results-2026-11-20-root-apfs-readonly.md). The boot-time repair path and composed root lookup still need qualification.

The first-install Data seed matches its recorded UUID and lacks fstab entries. Static inspection identified Data paths used by the embedded directory cleaner and two RunAtLoad job definitions. The existing RAM-backed overlay accounts for writes routed through the Data device when `data-persist=0`. See [Data startup paths](docs/results-2026-11-20-data-startup-paths.md) for the write audit and remaining checks.

The stock 27.3 build route selects the development NativeUfs policy. The retained host collection's verified symbols and object digest were checked separately. See [verified UFS build route](docs/results-2026-11-20-verified-ufs-build-route.md) for production inputs and the correction to the compiler guard description.

The host SHNGROOT v2 capacity check passed for the retained OS and unshimmed SystemOS. Mac `/tmp/piano-root-tables.LLOseQ` retains both block hash tables and `layout/manifest.bin` (SHA-256 `de43725482e8d17354e75ab1e2b79d706301dfe81ee6db6bbeaf4d236a124948`). See WRITE-PLAN.md for capacity and revalidation against the final SystemOS.

For the separate 27.3 root partition, set K273_ROOT_PART=linux_rootfs in the image builder. A WSL fixture verified the default and explicit partition arguments and rejection of invalid or duplicate selectors. Full image and tablet validation remain pending.

The 27.3 RAM builder now rejects a root.json write partition that differs from K273_ROOT_PART. The UbuntuPiano fixture passed for matching and absent write records and rejected the historical linux_data record when linux_rootfs was selected. make-root-image.sh still emits historical geometry for the legacy unsigned layout. The SHNGROOT v2 host layout omits write geometry.

The root image assembler accepts K273_ROOT_PART=linux_rootfs with an explicit K273_ROOT_EXTENT in inclusive 4 KiB LUN 0 LBAs. It checks capacity before allocating the image and records the proposed placement under root_extent. The UbuntuPiano fixture accepted an exact fit and rejected an extent one block short before creating root.img. Tablet validation remains pending.

The Mac assembled `root-v2-unshimmed.img` beside the retained SHNGROOT v2 manifest. Readback matched the manifest, both APFS images and both SHA-256 tables. Full image SHA-256 is `529a2c686cc41c8afb86bc3c6c405645a840d30b80125e1136209dc69b4fc14d`. The proposed placement ends at LBA 107421183 with 91392 blocks free. Final SystemOS and signed policies remain pending.

The 27.3 ESP builder accepts --esp-first-lba for the provisional layout and checks that the ESP does not overlap the supplied Data extent. UbuntuPiano built and read back a fixture at LBA 124289792, and rejected an aligned overlapping placement. The tablet installer still requires separately qualified geometry checks.

The 27.3 ESP builder produced identical images from identical inputs with mtools 4.0.50. The complete maintenance fixture suite passed on the Mac. Next, qualify SystemOS mount and fsck in the composed root namespace. See [ESP host regression](docs/results-2026-11-20-esp-regression.md).

For the independent root, the pinned APFS helper overlays only APFS Resources after successful Data preparation. It does not intercept either launchd fsck call. NativeUfs reports verified windows as write-protected and rejects outgoing block requests there. The standard development builder can expose writable windows. A boot candidate must bind the final helper paths and record fsck device selection and exit codes, with root, persistent store, and diagnostic log write channels checked independently. The 2026-11-20 upstream main recheck returned the existing 2438556 baseline.

Windows ADB reconfirmed the connected HyperOS 3.0.309 A-slot baseline and inline-encrypted F2FS mount. The existing rooted shell returned stock userdata geometry and three full boot partition hashes matching the retained capture manifest. Both retained rollback directories also passed `verify-android-rollback.py`. A root-only `snapshotctl dump` returned `Update state: none` on slot A with no merge or rollback indicator. See [connected baseline](docs/results-2026-11-20-connected-baseline.md).

The tablet entered stock ABL Fastboot and returned normally to HyperOS 3.0.309. ABL reconfirmed slot A successful and bootable with six retries and no pending snapshot. See [the ABL recheck](docs/results-2026-11-20-abl-recheck.md). The diagnostic image remains untested on the tablet.

The independent Mac checkpoint has a verified arm64e NativeUfs object with root write registration compiled out. See [the build and symbol comparison](docs/results-2026-11-20-native-ufs-readonly-gate.md). Keep the writable checkpoint as the separate persistent build input.

The read-only NativeUfs object is now relinked into the full 26-driver host KC. DarwinBoot parser, vtable and native-kext checks pass; the collection remains host-only. See [the full relink result](docs/results-2026-11-20-native-ufs-readonly-gate.md).

The UbuntuPiano menu container and its embedded boot DTB match the recorded SHA-256 values. The DTB was extracted to `/tmp/piano-dtb-reuse.wpp3m76n/piano-boot.dtb` on UbuntuPiano. Both embedded copies are identical. The Windows and Mac first-install seeds match the manifest, and the Windows Apple-only trust bundle matches the root assembly record. The historical `/tmp/piano-current.adt` and `/tmp/piano-current.trustcache` lack a build record tying them to the independent read-only KC. The trust file also differs from the verified Apple-only bundle. Generate the ADT from the frozen profile and qualify the firmware handoff and trust inputs before assembling the XNU image. The host staging command failed before invoking the packer. No new image was produced or loaded. Upstream `main` still matches `2438556c77c90c22c7b76e57eb8b40c44b35f1af`.

Correction to the ADT audit: two ADTs generated from the same frozen `d830d23` UniXNU profile and the same board inputs differ only in volatile random-seed bytes. `tooling/adt/adt-dump.py` (which masks `random-seed` and the unique-chip-id seed) produced byte-identical normalized dumps with SHA-256 `a2edf1cbc3e4184298b2b343b884472e82e94f62a5a433f01d51d23957d0798a`. The ADT structure and profile are therefore reproducible; retain the raw ADT hash only as an artifact identity, and use the normalized dump plus input hashes for reproducibility checks. This does not solve the missing native k27 source or device qualification gates.

The 26C5035r-specific ADT profile was regenerated twice from the frozen `d830d23` dependency using `configs/k273-adt.toml`, the retained SystemOS UUID `284548A8-FC87-434E-A0C8-A4038E83704D`, chip identity `47554e5941480001`, data/trust slots and no private Bluetooth address. Normalized dump comparison and the generated hardware DTB comparison both passed. Representative artifacts: normalized dump `c122c12197a8beb73bb087398ba9f9bddbf958081077a98f128f5649d1dd830d`, raw ADT `27b2fcef7493c5b392c1d00859ed03f811f899e29306c48607d488556fd44f07`, hardware DTB `3e8ec34e612242bb93182713c77b8209890407b6497fc71042fac16e5ad5760d`. These are host reproducibility evidence only; they do not establish a valid native k27 kernel or device boot.

A complete 27.3 offline RAM candidate was built with the read-only native KC, Apple-only 26C5035r trust bundle, preserved first-install seed, generated 8 GiB ADT, diagnostic FD and retained DTB. `data-persist=0` was explicit. The candidate image is 185,733,120 bytes with SHA-256 `e0ac9c1eafec6bdb2410144f337cd8860308bf9a57e44f3e22b61a7a7e7eb00e`; the UEFI-only companion is 13,766,656 bytes with SHA-256 `ed5f802afee7ddf9703c5a7b41a9afce7ad59f21e97563b27bbb030dd3af6986`. The Ubuntu reference verifier passed with zero failures. This remains host-only and unsigned; no tablet action was taken. See [27.3 RAM candidate](docs/results-2026-11-20-k273-ram-candidate.md).

The successful RAM candidate includes the PMIC SDAM bootloader restart argument by default and is not approved for a side-effect-free temporary boot. Build and verify a separate `PIANO_RESET_AFTER=0` candidate before considering any device trial; the failed rebuild attempt produced no image.

The reset-safe candidate is now built and verified: `PIANO_RESET_AFTER=0` removes both reset controls, and `verify-ram-images.py` passes. The macOS RAM image SHA-256 is `b99eac50488cd8d24938965987f51be16e41af1257c09a294c8ec1fcccd7704a`; the UEFI companion is `4e8736a9d227e5a476d55bec7a2379532ff6a5d959ecb7c599094801615d1fce`. No device boot was attempted.

The manifest now records both manual-reset containers and the hashes of their build record and verifier log. The verifier read the embedded FAT configuration and reported an empty Android header command line. Both XNU menu entries omit `log=1` and `piano-reset-*` and carry `data-persist=0`. This covers the packaged configuration. The effective arguments at the XNU handoff and the stock Xiaomi ABL persistence paths remain open. The XNU menu defaults to immediate 8-CPU startup, so use the separate UEFI-only image for any qualified menu test.

A fresh read-only ADB check using the retained Windows platform tools confirmed `piano`, HyperOS `OS3.0.309.0.WPYCNXM`, slot `_a`, and `sys.boot_completed=1`. The stock ODM audio preset matched the frozen pin and was staged in ignored `out/firmware/PianoAudio/fs19xx.fsm`; host readback confirmed 4,348 bytes and the same SHA-256. The stock ODM video firmware instead returned SHA-256 `7ab766ffb7bfce6167580d949176004db2f1f5a69aeef88bb29371b6616113be`, which differs from the frozen `95c5882f` pin. No video firmware was staged and the recorded RAM images were left as built. The host ABL and snapshot gate tests passed on UbuntuPiano. No reboot, flash or device backup was performed.

The pinned video firmware was independently fetched from upstream `yzddmr6/xiaomipad-8pro-mainline` blob `2891ccc2b944ed84dd19ed0a6114ae455eaac343`; its 2,409,160 bytes match SHA-256 `95c5882f9b66b93ad2635604b7db563cc310481e017fa6b82e0ce6cbb9eae01b`. It is staged in ignored `out/firmware/PianoVideo/vpu35_4v.mbn`. This is distinct from the tablet OS3.0.309 ODM copy (`7ab766ff...`) and therefore has not been substituted. The existing RAM image has not been rebuilt after staging; its manifest remains an image with zero firmware entries.

The firmware-complete RAM candidate was rebuilt from the preserved kernel, ADT, trust cache, seed and menu FD with `PIANO_FW_ADDON=1` and `PIANO_RESET_AFTER=0`. The addon carries both pinned firmware files, and the verifier passed all checks including both firmware pin matches. macOS image SHA-256 is `aae083633c1ecd75fe8fefa81867e98245a377368c1241fc02d924153fdeb976`; UEFI image SHA-256 is `41cf230acbbd628497236a6021420bb12360ac4f4a67d0e0dfe74eacca2333a9`. The candidate remains unsigned and not device-tested.

The original Apple mount, fsck and fsck_apfs passed strict signature verification across all architectures. Their arm64e CDHashes are present in the frozen Apple trust bundle. See [helper trust evidence](docs/results-2026-11-20-systemos-helper-trust.md). The native shim policy and composed root execution remain open.

The frozen manual-exit maintenance image matched its manifest SHA-256. Its extracted ARM64 BusyBox (SHA-256 `52151e7f322f926b64049cdaa1410dc3ea6485525e0624b05813791c219ae933`) supplied dd, awk, tail, and sha256sum through QEMU user emulation for the host hash tests. Both complete reads matched their expected digests. A missing input and both short reads returned errors without digests. This covers user-space utility behavior on scratch files. ABL write paths and UFS readback remain open on the tablet.

The tablet recovered after the authorized diagnostic RAM boot and now reports HyperOS OS3.0.309.0.WPYCNXM, slot _a, and sys.boot_completed=1 over ADB; see [diagnostic boot recovery](docs/results-2026-11-21-diagnostic-boot-recovery.md). The menu was not observed, so native execution remains unverified.

The maintenance RAM image now boots on the tablet with USB NCM/root shell. Read-only UFS and A-slot boot hashes match the retained recovery assets, and `piano-maint fastboot` followed by `fastboot reboot` returns to HyperOS with unchanged ABL state. See [maintenance RAM boot](docs/results-2026-11-21-maintenance-ram-boot.md).

The owner authorized persistent writes within the deployment scope and requested no tablet backup. The maintainer baseline and retained Mac inputs were rechecked. The SystemOS helper pair and kernel union mount remain open. See [the deployment input check](docs/results-2026-11-21-deployment-inputs.md).

The Mac persistence simulation passed with zero failures on the maintainer fixture geometry. See [host persistence regression](docs/results-2026-11-21-persistence-host.md).

The current HyperOS mount, encryption, and Data capacity reference is in [the recovery acceptance baseline](docs/results-2026-11-21-hyperos-recovery-baseline.md).

The [piano device catalog audit](docs/results-2026-11-21-device-catalog.md) records the repository revisions, a SystemOS media publication timing lead, and the related UEFI AndroidBoot and restore scope. It did not recover the native k27 tree or SystemOS shims.

The [4 KiB host fixture](docs/results-2026-11-21-f2fs-4k-host.md) passed formatting and dry-run fsck at the proposed userdata capacity. Xiaomi Recovery and Android encryption initialization remain to be verified on hardware.

The [GPT rehearsal](docs/results-2026-11-21-gpt-write-rehearsal.md) verified candidate and stock parsing at 4 KiB and restored both original GPT boundaries byte for byte on a host file. The legacy installer still expects the historical layout and a verified backup session.

The Piano catalog audit identified a separate macOS 27.3 XNU pin and a SystemOS retry patch. The public SystemOS host verification hash differs from the retained unshimmed image. ADB confirmed HyperOS booted on slot A. See [catalog audit](docs/results-2026-11-21-catalog-repositories.md).

The retained SystemOS image passed the Mac's read-only APFS check. Its APFS volume UUID matches local metadata and differs from the catalog policy UUID. See [SystemOS APFS check](docs/results-2026-11-21-systemos-apfs.md).

The GPT host rehearsal now also fault-injects a candidate backup table with a stock primary, then restores and byte-compares the original backup tail. This confirms only host parser behavior for that interruption state; device GPT selection and recovery remain unqualified. See [GPT rehearsal](docs/results-2026-11-21-gpt-write-rehearsal.md).

The independent bindfs collection replay matched the retained `owner-cryptex-bindfs.kc` byte for byte in UbuntuPiano. See [the collection replay](docs/results-2026-11-21-owner-collection-replay.md) for the exact input hashes and required working directory. SystemOS helper dispatch and final root authentication remain open before tablet testing.

The latest public SystemOS source check still found no shim sources or downloadable image. The retained Apple SystemOS cryptex DMG matches the comparison tree's hash, but our separately built SystemOS image and UUID do not match its policy. See [latest SystemOS source check](docs/results-2026-11-21-latest-systemos-source.md); do not pair that policy with our image.

The core's pinned VerifiedRoot source is available. Its C parser and OpenSSL provider accepted the signed upstream host fixture and rejected a changed block. See [VerifiedRoot host check](docs/results-2026-11-21-verifiedroot-host.md). Target crypto and final SystemOS policy integration remain pending.

The archived DroidVM Git history contains the legacy mount and fsck shims. Their no-op paths and the hashes of two matching historical SystemOS images are documented in [historical SystemOS provenance](docs/results-2026-11-21-historical-systemos.md). Native filesystem task behavior remains open.

The NativeUfs source audit confirmed separate write paths for development root windows, the Data store and persistent logs. A read-only root build must still account for the latter two paths. The exact launchd filesystem dispatch and target results remain open. [NativeUfs write audit](docs/results-2026-11-21-native-ufs-writes.md)

The pinned DEBUG firmware routes BDS debug output to the Piano_Log RAM ring via MemorySerialPortLib. The storage-isolation patch removes PianoFastbootStart and PianoFastboot.c, which provide the upstream debuglog reader. The BDS error message immediately before CpuDeadLoop therefore has no demonstrated host retrieval path in the frozen diagnostic profile. PianoDwc3 starts in XNU and cannot report pre-XNU stages. Use an independent firmware-stage output for the next diagnostic.

The rebuilt BDS diagnostic adds three console markers and passed host packaging verification. A follow-up build with USB diagnostics reached the built-in UniXNU menu, produced a retrievable log and framebuffer screenshot, and rebooted into HyperOS slot A. XNU was not loaded. See [the hardware result](docs/results-2026-11-21-bds-usb.md).

The retained USB diagnostic image has its FAT boot sector at file offset 4263936, exactly the start of the Android v2 ramdisk section. It declares 512 bytes per sector, ends in `55 aa`, and hashes to `e48dc8825602da737e369d1c603c89a1a708dd52c008501be5693683f95ee320`. The staged RAM Block I/O driver advertises 512-byte blocks and publishes the vendor GUID expected by the configuration filter. The hardware log does not establish the bytes the handle exposes or whether a filesystem driver binds to it. The next diagnostic must record RAM LBA 0, targeted ConnectController status, the Disk I/O and Simple File System protocols, and the TOML read result.

The firmware now reads the diagnostic RAM configuration on the tablet. Its FAT driver rejected the undersized 8 MiB FAT32 volume, and FAT16 restored access with the same FD and menu contents. The tablet returned to HyperOS 3.0.309 with unchanged ABL slot values. See [the FAT16 hardware result](docs/results-2026-11-21-ram-config-fat16.md). The next native test still requires audited payload, root and SystemOS pairing.

The generated FAT16 diagnostic container passed the complete Ubuntu reference verification and tablet test. Android returned on slot A with unchanged ABL state. See [the packaging replay](docs/results-2026-11-21-fat16-packaging-replay.md).

Historical fips4b image hashes, distinct November 15 boot observations, and current Mac reachability are recorded in [the fips4b inventory](docs/results-2026-11-21-fips4b-inventory.md).

The [historical KC loader audit](docs/results-2026-11-21-historical-kc-contract.md) records a host pass for the 79-member fips4b collection and identifies differences from its archived predecessor that the CPU subtype helper cannot reproduce.

The retained 26-driver RAM payloads match their recorded image hashes and carry the 27.3 root identity. The extracted menu starts XNU immediately in UFS root mode, and its FD differs from the tablet-tested isolation FD. See [the retained payload audit](docs/results-2026-11-21-retained-ram-payload.md) before preparing a register probe or a desktop image.

UbuntuPiano `/tmp/piano-probe2.cCVb3GOH/` now holds `boot-piano-macos-ram-probe2.img` (185733120 bytes, SHA-256 `0532e78217c660123b710a87513842a7775b7e85774c3628f23499f571aa0fc6`), its build record and the Ubuntu reference verifier log (`PASS: 0 failure(s)`). The image combines the tablet-tested isolation FD with the retained 27.3 KC. The extracted menu waits for selection and both XNU entries have `native-ufs-probe=2 data-persist=0`, without a UFS root or reset target argument. Probe 2 reads clock and controller registers, then deliberately panics to report the result. Before device testing, qualify the panic output and recovery path and audit panic-time writes. The isolated FD omits UFSDxe, so an inaccessible controller is a possible observation and does not establish root readiness. This image has not been run on the tablet.

The mailbox variant `342c310e43df5d0697d2661c519272bb99dd365f48033fa861bd200816347463` passed the Ubuntu reference verifier. Both XNU entries specify `pstore-console=0x9cb00000 pstore-console-size=0x700000`. The base isolation patch removes UEFI Fastboot, but the retained tracing FD used in this candidate restores it. The matching build source calls `PianoFastbootStart` and contains the OEM dump and upload handlers. Its poll timer ends at ExitBootServices, so the recovery path needs a separate firmware boot to read the retained RAM. ADB confirmed HyperOS 3.0.309, slot `_a`, boot completed. Confirm the mailbox readback range with the menu-only image and resolve panic-time variable persistence before selecting XNU. This candidate has no tablet runtime result.

The packed tracing FD now has direct host evidence for its RAM-backed EFI variable driver. The retained KC contains the mailbox implementation, and the firmware dump limit covers the 7 MiB range. See [the mailbox and EFI variable audit](docs/results-2026-11-21-mailbox-firmware-audit.md). The Linux maintenance panic setting does not establish XNU behavior. The range mapping, reset retention and direct platform panic paths remain open.

The menu-only image verified the full 7 MiB mailbox upload on this tablet and returned to HyperOS with unchanged ABL slot state. The capture contains no expected XNU header. See [the mailbox readback result](docs/results-2026-11-21-mailbox-readback.md). XNU reset retention and direct platform panic paths remain open.

The prepared XNU source dispatches registered panic callbacks before the base platform expert returns from panic sync. The source's `panic-spin` tunable only covers the ordinary debugger restart path. See [the panic path audit](docs/results-2026-11-21-panic-path.md) for the remaining linked-handler checks before the UFS probe.

The revised NativeUfs probe 2 logs and returns before starting the UFS service. The replacement object and KC passed the host link checks. The isolated replay and remaining boot gates are recorded in [the probe 2 result](docs/results-2026-11-21-native-ufs-inspect.md).

The repacked probe 2 mailbox image and its verifier log are in the same scratch directory as the host KC. Before selecting XNU, finish auditing the boot path after the probe returns and take a fresh ABL slot baseline.

The new probe 2 mailbox package audit is in [the NativeUfs probe result](docs/results-2026-11-21-native-ufs-inspect.md). It confirms only the KC changed within the FAT payload, menu timeout waits indefinitely, and the data/log write paths are disabled for this diagnostic. The later [UEFI entry test](docs/results-2026-11-21-probe2-uefi-entry.md) loaded this image and returned to HyperOS without selecting XNU. The running HyperOS A-slot ADB shell has no `/dev/mem` node. Capture the panel at XNU handoff and before reset. If the tablet stops responding, use the physical return to ABL documented in `docs/bringup-debugging.md` and check that ABL answers before considering another RAM boot for mailbox readback. XNU log retention after reset still requires a device test.

A host F2FS rehearsal now uses a 4096-byte loop logical sector and the exact proposed shortened userdata size; mkfs.f2fs and dry-run fsck.f2fs passed. It corrects the earlier 512-byte regular-file trial but does not qualify device recovery or encryption. See [the 4 KiB result](docs/results-2026-11-21-f2fs-4k-host.md).

The frozen mailbox image was rehashed directly in UbuntuPiano WSL at the recorded scratch path and matched the artifact manifest. The upstream default HEAD still matched the planned baseline 2438556c77c90c22c7b76e57eb8b40c44b35f1af. The tablet reported piano, OS3.0.309.0.WPYCNXM, slot _a, and sys.boot_completed=1 over ADB. This check did not load any image. The stock ABL audit remains open, and the owner needs to approve the concrete plan before any GPT or userdata change.

The A-slot recovery partition matches the verified 3.0.309 package image. Both slot hashes and capacities are recorded in [the live recovery result](docs/results-2026-11-21-live-recovery-slots.md).

The explicit candidate GPT fragments generated from the retained stock captures passed `sgdisk -v` on a fresh, read-only 4 KiB-sector host fixture. Their hashes and limitations are recorded in [the candidate fragment result](docs/results-2026-11-21-candidate-gpt-fragments.md). The candidate output and byte diffs remain in ignored `out/rollback-3.0.309-gpt/candidate/` and are not approved for device use until owner approval and maintenance writer qualification.

A read-only ordinary ABL Fastboot session on 2026-11-21 recorded slot A successful and bootable with retries 6, slot B marked unbootable with retries 6, and `snapshot-update-status=none`. Normal Fastboot reboot returned to completed HyperOS 3.0.309 on A. See [the dated ABL state result](docs/results-2026-11-21-abl-read-return.md). This does not qualify `fastboot boot`.

The 3.0.309 GPT pair checker passed against both frozen pairs and rejected the mixed pair expected after an interrupted backup-first update. See [WRITE-PLAN.md](WRITE-PLAN.md) for the readback command and the historical writer's layout mismatch.

The GPT pair checker also recognized both live LUN 0 fragments as stock over root ADB. See [the GPT fragment result](docs/results-2026-11-21-candidate-gpt-fragments.md).

The backup-first GPT routine also completed stock 鈫?candidate 鈫?stock on a 510,165,778,432-byte UbuntuPiano sparse file, with `fsync` and byte-for-byte readback at each offset. The in-memory failure cases and remaining maintenance integration are recorded in [WRITE-PLAN.md](WRITE-PLAN.md).

The final maintenance writer image (SHA-256 `ef9e4d27e921d96b371ac6487814fb758a1af226334ba6fd8d3edddcb8d02722`) passed a write-lock rehearsal on UbuntuPiano. The script extracted from the ramdisk used fixture paths for device, sysfs, proc and runtime files. An existing lock rejected the command and preserved both stock GPT copies and the existing lock. A 512-byte sector preflight failure released the acquired lock and preserved both stock copies. Candidate application and stock restoration each released the lock, and both GPT boundaries matched their expected bytes after each operation. Tablet GPT writes remain unverified.

The direct BDS lookup confirmed DarwinBoot protocol availability. Stock ABL accepted the updated eight-core RAM candidate, but host USB was absent afterward. Recover device access before another boot and verify HyperOS and the Android marker. See [the lookup and probe result](docs/results-2026-11-21-darwin-lookup-auto8.md).

ADB verified HyperOS and its marker after the first lookup-auto8 probe. The prearmed second probe staged a firmware log, but the host watcher stopped before download. See [the capture retest](docs/results-2026-11-21-darwin-lookup-auto8-retest.md).

A third prearmed auto8 run captured a 128-byte log showing DarwinBoot protocol registration, BDS console readiness and menu start; it showed no XNU handoff. HostUSB disappeared afterward. See [the capture retest](docs/results-2026-11-21-darwin-lookup-auto8-retest.md). The Windows watcher remains local and must be corrected and exercised on the next UEFI window before relying on it.

Latest lookup-auto8 follow-up: ADB confirmed completed HyperOS 3.0.309 on A and the unchanged marker. The reset trigger remains unknown. For unattended runs, verify recovery across the ABL-to-UEFI transition and preserve firmware DEBUG ring captures. USB disappearance alone cannot establish XNU entry. See docs/results-2026-11-21-darwin-lookup-auto8-retest.md.

The DEBUG ring capture verified RAM configuration and menu rendering. The last capture precedes selection. The diagnostic returned to HyperOS on A without an operator reboot. See docs/results-2026-11-21-darwin-lookup-auto8-retest.md. Capture the dispatch boundary before counting XNU entry or proceeding to desktop tests.

Prepared lookup-auto2 for a dispatch capture before the previously observed USB loss. Use the artifact digest in artifacts/ARTIFACTS.md. The host launch command stopped at its ADB list check before rebooting the tablet. Use the serial-specific ADB get-state check for the next preflight. Review the ABL and firmware persistence gate in WRITE-PLAN.md before loading the derivative.

The lookup-auto2 RAM boot was accepted by ABL and presented a roughly one-second UEFI USB window. The host captured no ring. Android return and XNU entry remain unverified. See [the result and reset coverage review](docs/results-2026-11-21-darwin-lookup-auto2.md).

The first Gunyah firmware probe is firmware/diagnostic/ram-watchdog-status.patch. It logs the current exception level and queries watchdog status at EL1 before starting diagnostic Fastboot. The patched boot manager compiled with the retained AArch64 DEBUG toolchain and flags. Firmware linking, RAM packaging and tablet execution remain pending.

The Gunyah watchdog status diagnostic now links and packages successfully. The retained WSL build log is UbuntuPiano:/tmp/piano-watchdog-status.zvGZxXmN/build.log. The Windows RAM image and FD hashes are in artifacts/ARTIFACTS.md. Windows PnP, ADB and Fastboot still showed no tablet after packaging. The firmware status query awaits a tablet run.

ADB re-enumerated as serial 7ccdd855 on completed HyperOS 3.0.309 A. Read-only ABL checks showed A successful/bootable, six retries, no snapshot; normal reboot preserved the Android marker. See [device return result](docs/results-2026-11-21-device-return.md). The watchdog status RAM candidate is ready for the next controlled trial.

The watchdog status RAM trial verified UEFI EL1 and STATUS=0 in the DEBUG ring. Host-issued UEFI Fastboot reboot brought back completed HyperOS on A with the original marker. See [the watchdog status result](docs/results-2026-11-21-uefi-watchdog-status.md). Test expiry independently before relying on recovery across handoff.

The Gunyah watchdog expiry RAM trial returned to completed HyperOS on A without a host reboot after capture. The Android marker matched. The reset source and intermediate USB interfaces were not captured. See [expiry result](docs/results-2026-11-21-uefi-watchdog-expiry.md). For the next trial, capture the USB transition continuously and test the gap between ExitBootServices and XNU watchdog ownership separately.

The Windows USB observer captured Android, stock ABL, and completed HyperOS in a host-directed control. See [the observer result](docs/results-2026-11-21-usb-observer.md). Start it before a RAM trial and wait for its first baseline line.

The expiry replay captured UEFI Fastboot loss and later verified completed HyperOS on A with the original marker. See [the replay](docs/results-2026-11-21-watchdog-usb-replay.md). Use a longer observation window for the post-ExitBootServices stall trial.

The [post-ExitBootServices firmware trial](docs/results-2026-11-21-watchdog-post-ebs.md) displayed its green and magenta marker and returned unattended to completed HyperOS on A with the original marker hash. Move the recovery control to DarwinBoot before DarwinEnter next.

The [DarwinBoot post-ExitBootServices stop](docs/results-2026-11-21-darwinboot-watchdog-build.md) builds as a DEBUG module. Rebuild the combined FD with the early firmware stop removed and the watchdog setup retained before packaging the automatic RAM probe.

The combined DarwinBoot watchdog RAM image returned unattended to HyperOS A with the Android marker intact. The loader stop itself lacks a framebuffer or log witness. See [the tablet result](docs/results-2026-11-21-darwinboot-watchdog-return.md).

The [DarwinBoot witness trial](docs/results-2026-11-21-darwinboot-witness.md) captured the loader immediately before the final ExitBootServices sequence with the watchdog armed. HyperOS A returned unattended and the marker matched. Post-exit evidence and XNU watchdog takeover remain pending.

The [DarwinBoot build replay](docs/results-2026-11-21-darwinboot-build-replay.md) linked from the retained cache after restoring the FD macros. The unpacked FVMAIN comparison found only PcdDxe debug metadata and the BdsDxe build time differences among the 174 FFS files. DarwinBoot matches byte for byte. Use the retained tablet-tested FD for the next controlled comparison.

The [DarwinBoot EBS mailbox diagnostic](docs/results-2026-11-21-darwinboot-mailbox.md) built and was packaged from the retained witness. It returned unattended to completed HyperOS A with the marker intact. All eleven checkpoint locations were zero after the separate reader boot. Qualify a known RAM write across this sequence before interpreting the absence.

The archived upstream references passed all 12 length and SHA-256 checks. ADB again confirmed completed HyperOS on A. See [input integrity and device state](docs/results-2026-11-21-resume-input-integrity.md) for the host helper check and outstanding filesystem dispatch qualification.

The [host fsck dispatch check](docs/results-2026-11-21-fsck-dispatch-host.md) found that the stock wrapper rejects a positional APFS volume and can select the host container with no device argument. The explicit APFS helper passed against the isolated SystemOS UUID. Keep target selection and repair policy explicit for the native candidate.

Static symbol inspection of the 26C5035r fsck wrapper confirms fstab enumeration (getfsent) and -R/-L options, but not native target selection. See [fsck symbol audit](docs/results-2026-11-21-fsck-symbol-audit.md).

The CVP dump is not uniformly zero: 1,176,334 bytes are nonzero, but all eleven expected DarwinBoot mailbox pages at offset 0x6F5000 are zero and the magic is absent. See [mailbox offset audit](docs/results-2026-11-21-mailbox-offset-audit.md); this still does not prove whether the records were never written or later cleared.

The DarwinBoot trial dump and the earlier menu-only dump have identical zeroed expected-record windows and the same nonzero tail marker; the reader path is therefore transferring the expected CVP range. See [mailbox baseline comparison](docs/results-2026-11-21-mailbox-baseline-comparison.md).

A WSL artifact recheck found retained 27.3 kernel/boot/payload outputs, but the producing checkout is heavily modified and all related RAM candidates remain explicitly offline and untested. The locked k27 commits are still absent; see [WSL artifact recheck](docs/results-2026-11-21-wsl-native-artifact-recheck.md).

The retained boot image embeds the archived FAT payload byte for byte. Its embedded KC matches the payload manifest and has SHA-256 beginning d36b8047. Trace the loose KC beginning afe7646e separately. See [retained boot payload audit](docs/results-2026-11-21-retained-boot-payload.md).

The retained boot payload's ADT, Apple trust cache and 4 GiB packed APFS scratch image match its manifest. SystemOS and root are external to the payload and still require a verified pairing. See [payload companions](docs/results-2026-11-21-retained-payload-companions.md).

The old task archive identifies d36b8047 as the local 2026-11-14 candidate and records only a first-192-MiB ESP write/readback; its own README says it was not tablet-booted. See [historical d36b8047 source](docs/results-2026-11-21-historical-d36b8047-source.md).

Historical ESP run evidence shows the retained candidate reached the UniXNU menu but DarwinBoot refused with Load Error before XNU handoff. See [historical ESP run](docs/results-2026-11-21-historical-esp-run.md).

The working-copy DarwinBoot parser rejects the original d36b8047 collection and accepts the archived subtype-2 and loose afe7646e collections. See [host parser replay](docs/results-2026-11-21-retained-parser-replay.md).

A byte comparison found the archived subtype-2 correction at offset 0xb. It makes the outer and embedded kernel CPU subtype words equal, satisfying the current DarwinBoot parser. See [retained subtype mismatch](docs/results-2026-11-21-retained-subtype-mismatch.md).

The subtype-2 ESP payload and its padded readback match. Both retained corrected-header RAM images still contain the original payload. See [archived subtype-2 ESP payload and RAM images](docs/results-2026-11-21-subtype2-package-consistency.md).

A host-only v2 RAM image was assembled with the parser-passing subtype-2 payload (d12691e0鈥?, existing kernel field/DTB, and recomputed section ID. Image SHA-256 begins 47125fbd; it has not been signed or sent to the tablet. See [offline subtype-2 RAM image](docs/results-2026-11-21-offline-subtype2-ram-image.md).

The [archived subtype-2 firmware and root audit](docs/results-2026-11-21-subtype2-fd-root.md) recovered the FD by matching its archived build hash and confirmed the ADT's 27.3 root selector. Effective storage defaults and the authenticated root pairing remain open.

The [subtype-2 container verifier](docs/results-2026-11-21-subtype2-container-verifier.md) found two legacy header fields and passed after normalizing those fields in a separate host copy. Root pairing and unattended device recovery remain pending.

The [subtype-2 root pairing audit](docs/results-2026-11-21-subtype2-root-pairing.md) matches the ADT selector to the Apple OS window and confirms that the separate SystemOS window requires its own authentication policy.

The [NativeUfs host tests](docs/results-2026-11-21-native-ufs-host-tests.md) passed all eight plain and ASan/UBSan combinations. They cover the development storage path only; authenticated root and tablet execution remain open.

The [retained SystemOS input check](docs/results-2026-11-21-systemos-stock-policy-input.md) confirms the stock 26C5035r image matches the archived policy digest and size. Full policy and native userland qualification remain open.

The [stock SystemOS table result](docs/results-2026-11-21-systemos-stock-table.md) generated a 22,790,144-byte table for the retained 2.9 GiB window; its SHA-256 matches the checked-in SystemOS policy header. The 50,331,648-byte table in the 6 GiB SHNGROOT assembly is not interchangeable.

The stock-window SHNGROOT v2 candidate passed host readback of both windows and both vroot tables. See [assembly evidence](docs/results-2026-11-21-stock-root-v2-assembly.md). The candidate still requires boot policy registration and native userland verification.

The [RootPolicies ADT comparison](docs/results-2026-11-21-root-policies-determinism.md) confirms fresh random bytes account for the differences between repeated control runs. The policy run adds only the expected 16-byte slot after excluding that randomness.

The Apple OSS IOStorageFamily source archive at e45fbe30a7879e0b16e26dddb78538f1b00e0bff was downloaded and extracted. The manifest records its SHA-256. The clean UniXNU d830d23 checkout on mac contains the header and driver build helpers. Header preparation requires the real upstream Git HEAD. A local archive import failed the revision check, and the Mac shallow fetch ended with early EOF after a transfer timeout. The alternate piano build script requests 7edb88fbae296fb7c8ce2f64e115e116e566d51c; keep that input distinct from the legacy pin.

The IOStorageFamily archive's tree matches GitHub API tree 6e9060023b750feae81727e756a0577f5829baa1. On Mac, the exact upstream commit object was reconstructed from the verified tree and commit metadata; the checkout now reports HEAD e45fbe30a7879e0b16e26dddb78538f1b00e0bff, clean status, and prepare-storage-headers.py passes with 19 public headers. This removes the previous storage-source pin blocker.

Storage ABI audit was attempted on the exact-pinned Mac checkout. The audit script currently fails before compilation because its local kits/XnuPrelink/native_prelink helper is absent from that checkout; this is a tooling-path blocker, not an IOStorageFamily source mismatch. The existing independent build tree contains the older audit assets but has not been substituted into the pinned build.

The retained Mac build tree successfully rebuilt NativeUfs after adding the exact IOStorageFamily header set. Command used the existing pinned SM8750 platform dependency and k27 object recipes; output contains NativeUfs.o and NativeKextGlue.o. This is a development/unverified-root kext build only. The target kernel collection link and device qualification remain pending; no tablet writes were performed.

The stock-window policy replay succeeded: the real OS/SystemOS 544-byte headers and shared 395-byte anchor from piano-stock-root were assembled into an arm64e policy object, then linked with the retained verified NativeUfs, VerifiedRoot and XNU crypto objects. The resulting stock-policy UFS object exports the policy symbols and defines vr_open; only boot_progress_poll remains unresolved for collection integration. See docs/results-2026-11-21-stock-policy-object-replay.md.

A host KC replay now consumes the real stock-root v2 policy object instead of fixture headers. The retained 26-driver owner recipe linked successfully to a 135,806,976-byte KC, SHA-256 a927b53d3ebca0e257d945d89a6ba0594835834a30f2a2bef374c83c1a178fbe. It is unsigned and host-only; packaging and device qualification remain open.

The locked DarwinBoot parser accepted the stock-policy KC replay and passed its synthetic malformed-range and arm64/arm64e tests. This is a structural host check only; signing, root mount and device execution remain open.

The [stock-policy provider audit](docs/results-2026-11-21-stock-policy-provider.md) traced the replay's resolved callback to the host-only object and confirmed the tablet was online in HyperOS on slot A. Historical iPadOS sources contain an empty callback. Establish the frozen macOS provider configuration before promoting the collection.

The [verified NativeUfs host replay](docs/results-2026-11-21-verified-ufs-without-poll.md) removes the host callback object and passes the pinned DarwinBoot parser with the stock policy and VerifiedRoot logic retained. Resume payload pairing from the manifest entry `host_stock_policy_kc_without_poll`.

The [deployment input recheck](docs/results-2026-11-21-deployment-input-recheck.md) confirms live HyperOS on A and a reachable Mac build host. The native SystemOS shim input and unattended recovery after XNU entry remain open.

The SystemOS builder accepts independently supplied mount and fsck shims by SHA-256 (1020daf). The Mac preflight accepted matching fixture digests and rejected a mismatch before image attachment; it did not build an image. The retained Mac fsck log reports a clean read-only inspection of the unshimmed SystemOS UUID 284548A8-FC87-434E-A0C8-A4038E83704D. A separate host options log shows fsck_apfs ignoring -f and rejecting a write-mounted container. These logs do not establish how target launchd selects a device. Qualify target dispatch and write boundaries before wiring any repair mode to the native boot path.

The historical SourceRootBinding resolver has now been inspected in full. It resolves one IOMedia by UUID and requires NativeUfs authentication along the provider chain; the owner patch then checks mounted device identity and read/write flags. The non-RAM Data branch still assumes `/dev/md0`, so it cannot be carried over for persistent UFS Data unchanged. See [the SourceRootBinding review](docs/results-2026-11-21-source-root-binding.md). This old source remains untracked in its workspace and has not been built with the independent XNU tree.

The historical resolver also compiled for the frozen independent iOS 27 arm64e target and linked relocatably with the retained verified NativeUfs object. Its direct NativeUfs authentication import resolves. This is not a final KC integration or runtime test; see [the resolver/NativeUfs link check](docs/results-2026-11-21-source-root-binding-ufs-link.md). A separate retained callback-based resolver requires `native_ufs_install_authenticator`, which this verified NativeUfs object does not export, so those two artifacts must not be paired without an explicit adapter.

The direct resolver replay linked the 26-driver KC. The clean pinned DarwinBoot parser (8544957e) accepted this exact KC and passed its synthetic tests. Symbol inspection found that the selected piano-bindfs-owner kernel already defines all three native_verified_block entry points also exported by the direct resolver. The kernel also exports the callback installer, which the combined direct resolver/UFS object does not import. Do not promote this host artifact until the kernel and UFS use one resolver and its call binding is verified.

Resume with the existing host_stock_policy_kc_without_poll artifact. Its retained start objects (D:/Temp/piano-owner-NativeUfsStart.o and D:/Temp/NativeRamBlockStart.o) contain BR26 relocations to the kernel callback installers. Disassembly shows each start routine signs and passes its corresponding provider function pointer, and reports failure if the installer rejects it. The KC symbol inventory contains both provider functions. Emitted binding and startup order still require verification before device qualification.

The retained no-poll KC now has a byte-level ARM64 BL relocation audit: UFS start's authenticated function pointer is passed to kernel installer 0xfffffff00c268174 (provider 0xfffffff008b51550), and NativeRamBlock start passes its provider 0xfffffff008b620a4 to installer 0xfffffff00c268f5c. This proves the packaged call targets and provider symbols, but not that start runs successfully before root authentication/Data lookup. Manifest records this distinction.

2026-11-22: The no-poll KC prelink metadata lists NativeUfs and NativeRamBlock as Root bundles and declares NativeUfs as a NativeRamBlock dependency. The authenticated kmod start fixups resolve to 0xfffffff008b5d170 and 0xfffffff008b648d8, respectively. Each enters the registration routine whose branch target was checked above. The first root and Data lookup timestamps still require device evidence.

2026-11-22: The [CrossDarwin recovery review](docs/results-2026-11-22-crossdarwin-recovery-review.md) confirms a sheng-specific root-unmount failure distinct from the historical piano platform reset trace. The UbuntuPiano retained payload hash matches the already audited parser-rejected candidate. Continue pairing the independent no-poll KC.

2026-11-22: [Stock KC/root pairing](docs/results-2026-11-22-stock-kc-root-pair.md) verifies exact public anchor and signed header bytes in the no-poll KC against the Mac's existing root policy set. The configured `mac` alias works while `mac-lan` fails DNS. Continue with runtime Data selection and unattended recovery.

The [first-install Data host test](docs/results-2026-11-22-first-install-ramblock-tests.md) passed all nine NativeRamBlock tests on the Mac and independently reproduced the seed identity. The mode-0 profile uses writable volatile Data. Target startup and automatic recovery remain open.

The 2026-11-22 GitHub API recheck resolved CrossDarwin's default test_27 to `4a671773c2ee289735ab526eda7c9628131250df` and the piano upstream main to `2438556c77c90c22c7b76e57eb8b40c44b35f1af`. The reviewed pins still apply. The retained DarwinBoot stop image hashes to `1adc49c0a613beb7775f4276978cfa55528e911cff02211d72e5b085e36a9e4c`. Its automatic return remains tied to the existing tablet record, which lacks a post-EBS witness. The stop patch stalls before DarwinEnter, so XNU reset behavior requires a distinct candidate and device test.

The [packed GOP inventory](docs/results-2026-11-22-packed-gop-inventory.md) confirmed SimpleFbDxe in the retained auto8, witness, and mailbox containers. The menu refusal remains ambiguous. The APRIORI witness provides a confirmed loader-dispatch baseline for the next diagnostic.

The [packed dispatch audit](docs/results-2026-11-22-packed-gop-inventory.md) also confirms that the historical auto8, witness and mailbox images share the exact FAT payload. The witness uses a different FD and reached the final EBS preparation with those inputs. Prioritize the FD and loader runtime differences when diagnosing the auto8 refusal.

The [witness replay identity check](docs/results-2026-11-22-witness-replay-identity.md) found identical APRIORI files and different DarwinBoot driver bytes. Use the archived FD as the witness reference.

The witness replay inventory also found BdsDxe and PcdDxe changes. A host-only decoded section derived from the archive moves DarwinBoot to first in APRIORI while preserving all bytes outside the GUID list. The result and its hash are in the witness replay identity record.

The archive-derived APRIORI order control is packaged and read back. See [packaging and readback](docs/results-2026-11-22-witness-order-packaging.md).

The [watchdog dispatch audit](docs/results-2026-11-22-watchdog-dispatch-gate.md) places all DXE dispatch before watchdog setup. The order control remains host-only.

The diagnostic [watchdog menu gate](firmware/diagnostic/ram-watchdog-menu-gate.patch) built in UbuntuPiano. FD SHA-256 is `453bf2d80b10dd4ed2fcd401141c80f471139a123d095736d49542768033bbd3`. The build log is `/tmp/darwinboot-post-ebs.tzsdzOfC/watchdog-gate.log`. The 174-entry FVMAIN inventory retains the archived order and byte-identical APRIORI and DarwinBoot files. BdsDxe and PcdDxe differ. The fallback cold reset on watchdog setup failure awaits hardware verification.

The watchdog-gate image's packed variable driver uses emulated NV variables with zero flash storage base and size. The reset driver matches the linked output. See [storage audit](docs/results-2026-11-22-watchdog-gate-storage.md).

The [watchdog callback audit](docs/results-2026-11-22-watchdog-reset-callbacks.md) excludes the identified open-source logger, TPM and NVMe callback drivers from the menu-gate FVMAIN. The failure reset runs before diagnostic Fastboot is available. Early reset and proprietary callbacks remain unverified.

The retained [misc captures](docs/results-2026-11-22-stock-misc-baseline.md) contain a bootonce-bootloader command. Use them for historical comparison. Unattended Recovery exit still needs a live readback through the maintenance path.

The [unattended Recovery exit](docs/results-2026-11-22-unattended-recovery-exit.md) passed on stock GPT. The maintenance RAM boot verified and cleared the live Recovery request in the first misc sector, then returned through ABL to completed HyperOS on A. Shortened userdata initialization still needs an unattended procedure.

The [stock Recovery wipe review](docs/results-2026-11-22-stock-recovery-wipe-inputs.md) identified four option names and Xiaomi pre/post wipe hooks in the retained ELF. The bootloader-message option that formats both userdata and metadata remains unverified.

The [stock Recovery UI extension trace](docs/results-2026-11-22-stock-recovery-ui-extension.md) identifies two inert defaults and the Xiaomi extension that supplies wipe handlers. Resolve its dynamic vtable relocations and confirm device factory selection before the stock-layout trial.

The Recovery extension trace now resolves the factory loader and the conditional metadata erase. The stock fstab contains both format targets. The stock-layout trial still needs live evidence that the vendor factory loaded, both volumes formatted, and the encrypted Android mount came back without manual intervention.

2026-11-22: On stock GPT, a no-wipe log-access probe entered Recovery via ADB. Recovery ADB runs as uid 2000; `/tmp/recovery.log` was root-owned mode 0600 and both it and `/cache/recovery/last_log` were inaccessible. Logcat returned Recovery audit events, which did not prove filesystem formatting. The tablet returned via ABL and the existing maintenance image after checking and clearing the exact empty Recovery request in `misc`; direct readback matched the known zero-sector hash. ABL reported no snapshot, and HyperOS `OS3.0.309.0.WPYCNXM` on A booted with encrypted Data. No userdata, metadata, or GPT wipe was issued. Qualify an independent readback or log capture for the format trial.

A stock-layout readback confirmed `sda20` is metadata and has F2FS magic at offset 1024. Raw `sda34` is userdata and does not expose the F2FS magic at that offset in the current encrypted installation. Compare metadata identity in maintenance and collect userdata filesystem evidence from the mapped volume or Recovery formatter. A raw userdata UUID comparison cannot serve as the current baseline. The maintenance shell returned through ABL with snapshot status `none` and HyperOS completed boot with file encryption.

The stock Android transport is currently authorized (ADB enabled, `ro.adb.secure=1`, `ro.debuggable=0`, build type `user`). An Android shell cannot read the raw volume mapping or inspect the persisted ADB trust file, and the prior Recovery return only tested an already provisioned Data volume. Qualify a host-observable boot and a subsequent host-controlled return to ABL across fresh Android setup before using a wipe as an intermediate step of the unattended GPT trial. Keep the verified ABL maintenance entry and existing recovery evidence as separate gates.

2026-11-22: Read the kernel directly from the watchdog-gate boot image FAT at byte offset 4263936. Its SHA-256 matches `readonly-inspect.kc` and differs from the independent no-poll collection. The manifest now records the embedded identity. The watchdog-gate image has no tablet result.

2026-11-22: The [no-poll watchdog control](docs/results-2026-11-22-watchdog-no-poll-control.md) passed boot ID and FAT readback on the host. Its inherited loader stops before DarwinEnter. Tablet testing remains pending.

2026-11-22: Built the watchdog-enter firmware with the successful ExitBootServices marker followed by DarwinEnter. The no-poll image is recorded in the manifest. Readback verified the boot ID, original FAT payload and DTB. The firmware inventory comparison and independent XNU entry witness remain open.

2026-11-22: The [watchdog-enter inventory](docs/results-2026-11-22-watchdog-enter-inventory.md) found two changed modules in the same 174-entry FVMAIN order. DarwinBoot and BdsDxe differ. The baseline and rebuilt watchdog gate require a direct BdsDxe comparison before treating the trial as a loader-only change.

2026-11-22: A fresh [ADB layout check](docs/results-2026-11-22-live-layout-recheck.md) found the candidate partition names and shortened Android capacity. Use this live observation for the next preflight and verify the raw GPT before writing native volumes.

2026-11-22: The first native root write sequence is recorded in [WRITE-PLAN.md](WRITE-PLAN.md#first-native-root-write-on-the-verified-candidate-layout). The existing stock root transfer is still in progress. Writer rehearsal and full target preflight remain pending.
`n
2026-11-22: The [full root preflight](docs/results-2026-11-22-root-blank-preflight.md) verified all 20 GiB of linux_rootfs were zero and returned to completed HyperOS on slot A. Writer rehearsal and complete source digest remain pending.

2026-11-22: The candidate root installer is available as maint-attach piano-309-root with --root-image. The Windows CLI dry run rejected the incomplete transfer. Host fault injection covered mismatched geometry, short writes and corrupted readback. A partial prefix stops for inspection. An already complete prefix is verified without rewriting. The full source digest and first hardware write are pending.

2026-11-22: Windows maintenance attachment uploaded the helper over a host-initiated USB NCM connection and verified its SHA-256 on the tablet. Tablet-initiated HTTP to the Windows listener timed out. The helper confirmed both live candidate GPT hashes and no UFS mounts. The old `map` step rejected the candidate geometry by design; use the candidate step's own mapping checks. The tablet returned via ABL to completed HyperOS. Root image streaming still needs the host-initiated transport; the successful helper upload only qualifies the control path.

2026-11-22: The original SCP session `70626` resumed after a brief stall. Host verifier `19509` waits for all 17,547,919,360 bytes and checks the frozen SHA-256. ADB confirmed completed HyperOS 3.0.309 on slot A. The RAM maintenance image and no-poll KC match their frozen hashes. The full source digest is required before the native root write. Fresh Git remote queries returned CrossDarwin `test_27` at `4a671773` and lenzhang `main` at `2438556c`. The existing source comparison still applies.

2026-11-22: The local unattended root phase is staged in `out/piano-309-root-live-20261122.ps1` (running session `47514`). It waits for the frozen root digest, checks the RAM maintenance image and live Android/ABL identity, runs the bounded root writer, and verifies return to HyperOS. The writer records its state and results in `out/piano-309-root-live-20261122/`. Inspect that session and its logs before starting another writer. The [BdsDxe comparison](docs/results-2026-11-22-watchdog-bds-compare.md) identified only a build timestamp difference in the extracted boot manager.

2026-11-22: The [root installation](docs/results-2026-11-22-native-root-install.md) passed full direct UFS readback, unchanged tail and both GPT checks, followed by unattended return to completed HyperOS on A. Session `47514` exited 0. Reuse the installed root; the next milestone is an observable XNU handoff with unattended recovery.

2026-11-22: The [watchdog-enter tablet control](docs/results-2026-11-22-watchdog-enter-return.md) reached UEFI and returned unattended to completed HyperOS on A. XNU execution remains unproven. Preserve this control and qualify an independent XNU witness before repeating the trial.

2026-11-22: Direct host inspection of the frozen no-poll KC's `LC_FILESET_ENTRY` records found `com.unixnu.driver.PianoDwc3`. The driver is inert without `piano-usb`, which is absent from the recorded watchdog-enter default arguments. Its USB device (`1209:0001` with the default PID) could provide positive evidence after XNU starts the driver. A missing USB device would still leave earlier stages unresolved. Qualify a separate diagnostic profile and host capture before running it. The tablet was present in ADB during this host inspection.

2026-11-22: Staged `out/boot-piano-watchdog-enter-usb.img` (SHA-256 `a711c1885a407d7630b3892e9f871ff6fcb7e3039a08514bd67a1141ce7f6da6`). Compared every file in the original and modified FAT by SHA-256; only `unixnu.toml` changed, appending `piano-usb=2` to the default eight-CPU entry. Verified the v2 boot ID and byte-identical kernel field (shim and FD) and DTB. The KC, ADT, trust cache, scratch image, 90-second reset and `data-persist=0` are unchanged. This image is staged, not tablet tested. Arm a Windows USB VID/PID observer independent of ADB and fastboot before running it.

2026-11-22: The [USB diagnostic tablet trial](docs/results-2026-11-22-watchdog-enter-usb.md) returned unattended to completed HyperOS on A. Independent Windows PnP capture did not sample the expected XNU gadget. Prioritize an earlier checkpoint and verified retrieval before another trial.

2026-11-22: The [CVP retention control](docs/results-2026-11-22-cvp-retention-control.md) recovered a known RAM word at 0x9D1F5000 after maintenance rebooted into ABL and before Android started. The reader then returned to completed HyperOS on A. This qualifies the readback route for that reset path. Gunyah watchdog retention and XNU reset routing remain to be tested.

2026-11-22: The [XNU restart routing trial](docs/results-2026-11-22-xnu-reset-routing.md) returned to HyperOS without observable stock ABL, so the qualified CVP reader did not run. The remaining question is whether DarwinBoot completes the handoff. Move the next observation upstream of the XNU restart hook.

2026-11-22: The [no-demura entry control](docs/results-2026-11-22-no-demura-control.md) passed host parser, fixup, trust and image checks. The bounded tablet run recovered ten successful EBS records through the watchdog and CVP reader, then returned to completed HyperOS A. XNU entry is still unverified. Add separate checkpoints around the MMU transition and at the XNU entry before the next trial.

The [XNU bootstrap table trial](docs/results-2026-11-22-xnu-bootstrap-live.md) reached the branch to `common_start`. Next verify execution before and after the MMU transition.

The [XNU first CPU trial](docs/results-2026-11-22-xnu-first-cpu-live.md) confirmed `start_first_cpu` on the tablet and unattended return to HyperOS A. Next isolate the MMU transition and first C entry.

2026-11-22: The [DarwinBoot checkpoint FD](docs/results-2026-11-22-handoff-cvp-build.md) built with the full replay macro set. FVMAIN has four changed entries. Compare the three other payloads with the frozen no-demura firmware and verify retention of the four CVP offsets before packaging the trial. No device run was issued.

2026-11-22: The [tablet handoff trial](docs/results-2026-11-22-handoff-cvp-live.md) recovered all four CVP checkpoints and returned unattended to completed HyperOS A. DarwinEnter reached the instruction sequence immediately before the branch to XNU. Add an independent XNU entry checkpoint to distinguish branch or entry faults from later console initialization failures.

The [MMU checkpoint mapping audit](docs/results-2026-11-22-xnu-mmu-mapping.md) places the existing post-MMU slot outside the qualified CVP capture window. Add a pre-SCTLR observation and qualify a reader or mapping for the post-SCTLR observation.
2026-11-22: Extended the existing high-address reader capture through the linked `arm_vm_init` checkpoints. Slots 0x13-0x15 are present and 0x16 onward are absent, localizing the stall after physmap setup and before early random completion. Reader returned automatically to HyperOS 3.0.309.0.WPYCNXM A with boot complete. See `docs/results-2026-11-22-xnu-mmu-live.md`.
2026-11-22: entropy-stage readback recovered port_crumb=60, before bootseed_init returns. Split the seed initialization stages before changing behavior. HyperOS A recovered unattended. See docs/results-2026-11-22-xnu-mmu-live.md.
2026-11-22: entropy-split ran on the tablet and recovered stage 166, immediately before the first HMAC. Seed retrieval and length validation passed. The next investigation is linked cchmac/SHA-512 dispatch. HyperOS A recovered unattended. See docs/results-2026-11-22-xnu-mmu-live.md.

The [first HMAC linked pointer audit](docs/results-2026-11-22-hmac-fixup-audit.md) matched on-disk SHA-512 authenticated pointer fields to their call sites. Runtime probes should distinguish HMAC entry, digest setup and compression using phase numbers only.

The [first HMAC compression checkpoint](docs/results-2026-11-22-hmac-compression-live.md) recovered phase 186 on the tablet and returned unattended to HyperOS A. Instrument SHA-512 compression entry and exit to distinguish the indirect branch from execution in the routine.

The SHA-512 entry trial read phase 186 and returned unattended to HyperOS A. Disassembly places the new phase 190 after the stack guard access. Dispatch and the callee prologue remain in the failure interval. See docs/results-2026-11-22-sha-entry-live.md.

The SHA-512 pre-stack trial captured phase 186 and returned unattended to HyperOS A. The new phase 192 precedes stack access. See docs/results-2026-11-22-sha-prologue-live.md.

The pre-stack candidate packed KC was walked from its format-8 page starts. Both SHA-512 fields at `0xfffffff007dd0fa0` and `0xfffffff007dd0fa8` are reachable in `__DATA_CONST`. Their encoded targets match the candidate symbols at `0xfffffff00bbd8c60` and `0xfffffff00bbd90e8`. Both use address-diverse IA with discriminators `0xc7a0` and `0x1bda`. Runtime application remains unverified. CrossDarwin `test_27` still resolves to the reviewed `4a671773` pin.

XNU calls `kernel_collection_slide` in `arm_slide_rebase_and_sign_image` before early random initialization. The caller ignores the return value. Existing trace IDs `0x2212` through `0x2217` record visited and rewritten counts, stop reason and address, return code, and cache level. The packed KC contains 364,815 fixup slots, all at cache level zero. The SHA-512 slots are visits 2,140 and 2,141 in outer chain order. Seven 8-byte words begin four bytes before a page boundary, all later in that order (first at visit 52,213). These boundary cases warrant separate mapping checks; they cannot explain a walker stopping before the SHA slots. Next runtime diagnostic: capture the existing walker counters and dedicated integer witnesses for the SHA slots, without logging pointer or seed values.

The [runtime fixup traversal](docs/results-2026-11-22-sha-walker-live.md) matched the complete offline count and returned zero. Stage 186 still reproduced. Continue at the actual SHA-512 call site.

The [SHA-256 HMAC final trial](docs/results-2026-11-22-sha2final-live.md) advanced past phase 211 and recovered phase 62, proving `cchmac_final()` and `bootseed_init()` returned. The new boundary is `ccdrbg_init()` in early random initialization: phase 63 was not reached. HyperOS A returned unattended with boot complete. Instrument the DRBG factory/init path before another RAM boot.

2026-11-22: The [early-random DRBG barrier](docs/results-2026-11-22-drbg-sha256-live.md) cleared on the tablet. The local `cchmac_init()` patch only redirected SHA-512 compression, so the SHA-256 DRBG still called the authenticated `di->compress` pointer and faulted. Routing `output_size == 32` to `ccsha256_ltc_compress()` for both outer and inner states advanced the RAM boot to stage 13, after `early_random()` returned and immediately before `machine_startup(args)`. The device returned unattended to completed HyperOS `OS3.0.309.0.WPYCNXM` on A. No partition, GPT, boot image, or userdata write was issued. The next diagnostic boundary is `machine_startup()`.

2026-11-23: The [PAC modifier trial](docs/results-2026-11-23-paclr28-address-diversity.md) cleared the fixed `autda` failure: paclr27 signed all 364,813 chained fixups and booted through `timeouts` with no panic, then returned unattended to completed HyperOS A. The next kernel fault was `blraa x16, x17` authenticating `___chkstk_darwin` through its auth_ptr slot; an address-diverse slot with zero diversity needs the full 64-bit slot address as modifier, which paclr27 masked to 48 bits. paclr28 carries that fix and is built (`boot-piano-paclr28.img` SHA-256 `0082bb88...`) but not yet run. The next live step is one paclr28 RAM boot when the tablet is enumerated on USB.

2026-11-23: [paclr39](docs/results-2026-11-23-paclr39-fips-post.md) localized the first corecrypto barrier. Default boots panic in `fipspost_post()` with -2074 (0xfffff7e6); `fips_mode=4` in both boot entries bypasses POST and advances to `0x348`, with `0x349` after `cc_kext_random_init()` missing. Note `piano_dbg_ckptv()` is required above id 0xca. [paclr40](docs/results-2026-11-23-paclr40-prng-checkpoints.md) added `0x350-0x357` in `register_and_init_prng()`: seq 72 records entry then stops, so the current boundary is the function head or `entropy_init()`. The early seq 44-51 block is a ring-collision artifact (torn ids/values), not a proven earlier successful call. Next: instrument `entropy_init()`/`cckprng_init_with_getentropy()` with self-validating records and log `prng_ready`. All runs returned unattended to completed HyperOS A with no persistent write.

2026-11-23: [paclr48/paclr49](docs/results-2026-11-23-paclr48-preexisting-corecrypto.md) found the base collection's 27.0 `com.apple.kec.corecrypto` registering before the linked 27.3 corecrypto. paclr48 recorded `register_crypto_functions() == -1` and zero digest KPI slots. paclr49 changed only the old kext's final `_corecrypto_kext_start` entry to `mov w0,#0; ret`, preserving its code for dependants. Registration then returned 0, both digest KPI slots were non-zero, SHA-1/HMAC/PRNG checkpoints passed, and the log advanced through IONVRAM setup. Trace shows a kext start and module entry returning 0; the machine reset unattended and returned to completed HyperOS A with no persistent write. The next boundary is after the observed kext-start/IONVRAM stage.

2026-11-23: [NativeUfs DMA stall and the KC base](docs/results-2026-11-23-native-ufs-dma-and-kc-base.md). The working base image's console ends its UFS path at `NativeUfs: DMA attempt 0: 2138112 bytes (slots 16, wide 1)` with no attempt 1, while other CPUs keep printing (`CoreAnalyticsHub`, `mbinit`, `NativeGopDisplay`), so the thread is stuck inside the first `IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIOMemoryPhysicallyContiguous|kIOMemoryMapperNone, 2138112, 4096)`. Instrumentation for that call is in `package/NativeUfs/NativeUfs.cpp` and compiles as a kext object on the Mac. The Mac KC builder cannot currently carry it: with the only local base, the already-built `paclr53-unpatched.kc`, four rebuilds (47 entries with NativeUfs, all 81 kept with NativeUfs, identity with nothing re-linked, identity plus `compact-kc-linkedit.py`) all stop right after IONVRAM, before AMFI, security-policy init, `piano-reset-to` arming and `StartIOKitMatching`; the identity build proves re-emitting a built collection is the broken step, not the kext change and not linkedit size. The documented 27.3 recipe needs Apple's `kernelcache.release.mac17j` as `KC_BASE` plus `patches/k27-kc/0001` best-fit placement (the recovered host-run builder lacks it, which is why 26 natives do not fit). Blocked on `out/inputs/k273/kc/kc273-mac17j.bin` and a k27 tree prepared by `tooling/k273/prepare-k27-tree.sh`. Every run in this round returned unattended in 67.6-67.7 s through the Gunyah watchdog and came back to completed HyperOS OS3.0.309.0.WPYCNXM; no partition, GPT, boot-image or userdata write was issued.
2026-11-23: [ufs101-103](docs/results-2026-11-23-ufs101-amfi-initbsd-defer.md) removed the inline AMFI `mpo_policy_initbsd` wait (thread per deferred policy after `mac_policy_release_exclusive()`) and replaced the dead `thread_call` reset tick with a kernel ticker. ufs101 reached `NativeRamBlock` and `bsd_autoconf: IOKitBSDInit rc=0`; ufs102 showed the Gunyah watchdog still bites at 32 s without pets; ufs103 spun on `mach_absolute_time()`, reset itself at 240 s (277.8 s wall), and the decoded console reached `bsd_init: calling setconf`, the registry walk (`visited=6 emitted=3`) and `piano: media wait bounded match 30s`. Root UUID `84581E99-85BE-4957-AC53-2947E887B3E4` was absent and the buzzy ticker starved the deferred AMFI worker, so `waitForMatchingService` never returned. ufs104 proved `thread_block(THREAD_CONTINUE_NULL)` blocks forever here (84.7 s bite, no spin beats). ufs105 uses `thread_block_reason(THREAD_CONTINUE_NULL, NULL, AST_YIELD)` once a second in the ticker; built, to be run. Every run is `fastboot boot` only and returns unattended to completed HyperOS OS3.0.309.0.WPYCNXM A; no partition, GPT, boot image or userdata write.2026-11-24: [ufs164-171](docs/results-2026-11-24-ufs164-171.md) moved the storage boundary: manual IOMedia publication never matched, and asynchronous registerService is not consumed this early, but calling `startMatching(kIOServiceSynchronous)` inside the publish thread let IOBlockStorageDriver match, run every `IOBlockStorageDevice` callback and issue real block reads (OCS=0). The OS window APFS container UUID reads `A7672058-6BF2-44D4-AE1F-CBF9CF814C0D`; SystemOS is `79F45B3D-B2A6-4497-8467-1CFF475F2415`. The booter still waits on volume UUID `84581E99-85BE-4957-AC53-2947E887B3E4`, no IOMedia node appears and `startMatching` never returns, so the driver start path is the current blocker. All runs were `fastboot boot` only and returned unattended to completed HyperOS OS3.0.309.0.WPYCNXM; no desktop, userland, switch or Data persistence yet.







2026-11-25: [UFS-capable FD](docs/results-2026-11-25-ufs-fd.md). The FD in the signed `boot_a` candidate has no UFS stack, which is why UEFI never reaches the ESP. Unpacking the LZMA FVMAIN of every local `*.fd` found `394c5c78…` (piano-macos-latest) with the Qualcomm UFS DXEs, the UniXNU `config: volume` patch and the 13 GiB DarwinBoot window. A trial image built by swapping that 3 MiB FD into the existing boot image was signed and `fastboot boot`ed with the older r9 FD: UEFI enumerated, then stopped enumerating without returning to ABL within six minutes, so its log was lost. No partition was written. The tablet needs one power cycle; the `394c5c78…` trial is prepared.

2026-11-25: [Mac FD build](docs/results-2026-11-25-mac-fd-build.md). The full `tooling/firmware/build-fd.sh` pipeline now
succeeds on the Mac host: FD `3fcc8429…` (3,145,728 B) carries UFSDxe, the patched `UniXNU.efi` (`config: volume %s`) and
the patched `DarwinBoot.efi` (`taken (largest run)`, 13 GiB), and `FV/Ffs` holds all four related modules. Two host fixes
were needed: the Sec sources copied from Windows were CRLF so both directions of `git apply` failed (converted to LF), and
`Mu_Basecore` was removed from the submodule update list on this host. A v6 boot image
(`out/persistent-v6-macfd/boot.img`, `946adfe0…`) was assembled from it and verifies with `avbtool`. Nothing touched the
tablet, which has been off USB since 03:18; the armed watcher still holds the `394c5c78…` trial.

2026-11-25: [switch tool audit](docs/results-2026-11-25-switch-tool-audit.md). The computer-side swap tool is
`tools/piano-switch.py` from the Ubuntu release tree (local copy `out/tmp-upstream-mainline/tools/piano-switch.py`,
`a3d3cb88…`): it checks both triplets' digests and sizes, requires piano/unlocked/slot A/userspace-no, flashes
init_boot_a then vendor_boot_a then boot_a, and restores the rollback triplet on failure. It never sets the active slot.
`maint-session.py slot-success` is the only writer of `boot_a`'s bit 54, with a GPT backup and a read-back limited to that
attribute and the CRCs; `abl-final` requires slot A current and successful with an unchanged retry count 0-7. The live
`getvar` values still have to be read on the tablet. The ESP manifest was also re-checked: `EFI/UniXNU/unixnu.toml` has the
five entries, an 8 GiB `ram_window` paired with the 13 GiB FD, and `data-store-part=linux_data`.

2026-11-25: the Mac build's uncompressed `FVMAIN.Fv` (8,065,024 B) contains `config: volume`, the UTF-16
`taken (largest run)`, `UFSDxe` and `/soc/ufs%d`, so the FD built by `build-fd.sh` carries the UFS stack and both patch
markers through packing.

2026-11-25: [startMatching wait audit](docs/results-2026-11-25-startmatching-wait-audit.md). In the k27 XNU tree at
`E:/Temp/k27-native-candidate-20261119-a`, `IOService::startMatching` waits only when the match leaves the busy count raised;
it then `assert_wait` + `thread_block` (IOService.cpp:1175-1195), which the earlier runs showed never resumes here. The next
image has to log entry/return of `IOBlockStorageDriver::start` and the busy count to tell "blocked inside the driver" from
"blocked in the wait loop".


2026-11-25: [acceptNewMedia audit](docs/results-2026-11-25-acceptnewmedia-audit.md). Apple's `IOBlockStorageDriver::start`
runs `handleStart` -> `checkForMedia` -> `mediaStateHasChanged` -> `recordMediaParameters` -> `acceptNewMedia`; ufs171's
console ends right after `recordMediaParameters`, so the stop is inside `acceptNewMedia` (IOMedia create, `attach`,
`attachToParent` in the driver tree, then asynchronous `registerService`), not proven to be the `startMatching` wait. The
collection `out/inputs/piano-ufs264.kc` keeps `LC_SYMTAB` for IOStorageFamily (1,477 symbols) and the kernel, so probes can
go on named entries: `acceptNewMedia` `0xfffffff00aeb8260`, `instantiateMediaObject` `0xfffffff00aeba1d4`,
`recordMediaParameters` `0xfffffff00aeba5d0`, `IOService::startMatching` `0xfffffff00c1450ec`, `_piano_dbg_kv`
`0xfffffff00bbc7590`, `_piano_stage_slots` `0xfffffff00c912880`. The KC builder is present locally
(`out/k27-independent-20261120/host-run/kc-preflight`), so a kernel-only rebuild or an in-place code patch needs no tree;
compiling a `package/` kext still needs the absent `PIANO_K27_TREE` recipes and generated headers. Nothing touched the
tablet, which stayed off USB.

2026-11-25: [acceptNewMedia probe](docs/results-2026-11-25-acceptnewmedia-probe.md). The probe collection
`out/k27-acceptprobe/piano-ufs265.kc` (`05d8ccb5…`) carries codes 0xA0-0xA6 at `acceptNewMedia` entry, around
`instantiateMediaObject`, `attach`, `attachToParent` and `registerService`; the stubs sit in the 1,657-byte zero hole at
`0xfffffff00bc07988` and preserve x0-x18, NZCV and x30. It is installed in
`out/k27-acceptprobe/boot-piano-ufs265-probe.img` (185,733,120 B, `86b534f2…`, boot id `6d766e67…`) by replacing
`EFI/UniXNU/Payloads/kernel.macho` inside the v2 ramdisk's FAT32 volume (file chain starts at cluster 5, 71,824 clusters,
same length) and recomputing the Android id; `out/fatswap.py` does what the archived scripts did with `mcopy`, since WSL
is unusable here. A byte diff against `boot-piano-ufs264cfg.img` shows 898 changed bytes in 54 runs: the id, the seven
4-byte branches, the 896-byte stub block and the 4-byte 0xA4 trampoline. The embedded collection in the ufs264 image is byte-identical to
`out/inputs/piano-ufs264.kc`, so the swap path is sound.

`tooling/native/run-ufs265-probe.ps1` boots that image, waits for the return to fastboot, boots `reader.img`, dumps one window and reports which of 0xA0-0xA6 were written; the last code is the stop point.
The first window is 1 MiB at `0x8c5000000`, scanned for four equal words in 0xA0-0xA6, because the earlier
8 KiB read at `0x8c5912880` was not the slot table and a slide must not hide the answer. If that window
returns nothing, the runner spends a second reader boot on `0x8c5912880`/`0x2000`, the page that already
read back once, and scans it the same way; one reader boot allows exactly one `oem dump`. It checks both
image hashes first and discovers the serial instead of hard-coding `7ccdd855`. Run: `powershell -NoProfile -File
tooling\native\run-ufs265-probe.ps1`. Without a device it exits 2 with `no adb and no fastboot device`, which is the
current state: the tablet has been off USB since 03:18. Nothing has been flashed; this is a `fastboot boot` probe.

2026-11-25: conditional-branch replay fix in the probe collection. The 0xA4 site replays `cbz w0, 0xfffffff00aeb87d0`,
a copied word whose ±1 MiB offset pointed into the hole from its new location, so the run would have died on the
success path before 0xA5/0xA6. The stub now retargets that word at a helper at `0xfffffff00bc07d08` that branches to
the original destination. New collection `05d8ccb5…`, image `86b534f2…` (boot id `6d766e67…`), 909 changed bytes in
47 runs against the ufs264 image; re-verified by disassembling the `kernel.macho` extracted from the image.
`_piano_dbg_kv` accepts codes `0x0b..0xca`, so 0xA0-0xA6 are stored and cleaned. Still not run: no device on USB.
2026-11-25: the probe now stores return values as well as stop codes. 0xA7 takes `instantiateMediaObject`'s
`x0` and 0xA8 takes `attach`'s `x0`, so a nonzero 0xA8 is the IOReturn to decode instead of an unexplained stop.
Stubs are 192 B (7 x 192 = 1344 B of the 1657-byte hole) with the helper at +1344; sites 0xA2/0xA4 hold two
`_piano_dbg_kv` calls. Collection `05d8ccb5…`, image `86b534f2…`, boot id `6d766e67…`, 1356 changed bytes in 48
runs against the ufs264 image, re-verified by disassembling the `kernel.macho` in the image. Still not run: no
device on USB.
2026-11-25: slot decoder host test committed (`18588056`). `tooling/native/test-decode-slots.py` covers table scan
(b0-b7 eight entries, slid base 0x1200), the default a0-a6 window, the no-hit fallback to the longest agreeing run,
`vote()` bit-error tolerance and `entries()` counting, all nine checks pass on this host. No tablet access; device still
absent from `adb devices` and `fastboot devices`, so ufs265/ufs266 probes remain unarmed.

2026-11-25: [NativeUfs static audit](docs/results-2026-11-25-nativeufs-start-static.md). The kext in
`out/inputs/piano-ufs264.kc` has no symtab, so its text was found by xref from its own cstrings:
`NativeUfsBlock::start` at `0xfffffff008b514f8` only sets `Content`/`Content Hint`, logs and returns
true; `publishMedia` at `0xfffffff008b516c0` calls `registerService` synchronously (`w1=2`); the
publisher thread registers the window asynchronously (`w1=0`) and then calls `publishMedia`. The
collection therefore matches the current source, and ufs171's stop inside Apple's
`acceptNewMedia` happens before any of our media code. Probe images re-hashed and match:
ufs265 `86b534f2…`, ufs266 `a56f132d…`, kc `9f276dff…`. Device still absent from `adb`/`fastboot`.
