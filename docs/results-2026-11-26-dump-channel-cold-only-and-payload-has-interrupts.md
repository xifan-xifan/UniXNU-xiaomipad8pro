# 2026-11-26 - 读回通道的真相是"冷会话能读、热会话必坏"；带中断路径的载荷就是我们手里那份 kernel.macho

本轮设备侧只有只读动作：`fastboot boot` 探针镜像一次、reader RAM 镜像三次，每次跑完自己回到 HyperOS
（`hyperos=up build=OS3.0.309.0.WPYCNXM`，`slot-retry-count:a` 全程 6）。没有任何分区、GPT、启动镜像或 userdata 写入。

## 一、`oem dump` 的可用条件被重新测出（推翻 results-2026-11-25-dump-channel.md 的两条规则）

`tooling/native/sweep-dump-addresses.ps1`（本轮新增）：每个地址一个全新 reader 会话、会话第一条命令就是一次
`oem dump`、每次之间用 `ensure-hyperos.ps1` 走回 Android。实测：

| 会话 | 地址 | 结果 |
| --- | --- | --- |
| 冷（Android → ABL → reader），11:49 与 11:52 两次 | `8c0004000 0x100` | rc=0，256 字节，全零 |
| 冷，12:00:40 | `8c5912880 0x2000` | **rc=0，8192 字节，4811 个非零** |
| 热（探针跑完后 `reboot bootloader` → reader），11:23–11:25 多次；本轮 11:22 又复现一次 | `8c5912880 0x2000`、`8c5000000 0x1000000` | 一律 `FAILED (Status read failed (Too many links))`，随后 reader 掉线 |

所以 11-25 记的两条都不成立：`8c5912880` **不是**"reader 的 stage-2 没映射"，`oem dump` 也**不是**"必须在同一个热会话里读"——
恰好相反：热会话必坏，冷会话可读。11-25 之所以得出相反结论，是因为当时所有成功的读都发生在冷会话（那一节自己的表格就是
"cold, from Android → OKAY / warm, after ufs264 → Too many links"），结论却写反了。

两个必须记下来的副作用：

1. **成功的 dump 也会把主机的 fastboot 客户端卡死**：`get_staged` 之后第一次 `fastboot devices` 永久挂住
   （本轮两次：11:52:19 与 12:00:41 各留下一个挂死的 `fastboot.exe`，父脚本卡在 `WaitFast` 里 150 s 与 6 min）。
   无人值守脚本在 dump 之后不得再调用 `fastboot devices`；要么直接结束本轮，要么用带超时的进程包装。
2. `adb reboot bootloader` 会间歇性不枚举（11:57:49 那次 150 s 内 ABL 没出现，重试一次后 18 s 成功）。
   脚本已加"失败后重发一次 reboot 并记录两处 devices 输出"。

代价：冷会话读到的这份页面**不是探针表**。8192 字节的形状是"每个 8 字节字里两个接近相长的 u32、整体单调递增"，
`decode-slots.py --scan --codes b0 b7` 报 `no probe entry found in the window`。`piano_dbg_kv` 的表形是同一个 u64 写满 4 个字，
形状完全不符，所以它不是 `_piano_stage_slots`（或者那份表已被后续启动覆盖）。

## 二、探针仪器的下一次设计（结论，不是待办）

要在 XNU 侧拿到证据，只有三条路，前两条被本轮结果限定：

1. **冷会话 dump 一个大窗口**：窗口内让解码器自己找表形（`decode-slots.py --scan` 已支持），不再猜 VA。
   前提是探针产物在中间那次 Android 启动里没被覆盖 —— 必须先做一次对照实验（探针 → Android → reader → 读同一页），
   否则读到的是别的东西而看起来像"探针没跑"。
2. **让产物自己走出去**：`piano-usb` 的 CDC-ACM 监视器有 `peek PA`，但它要求启动真的走到第 12 个原生 kext
   （`docs/results-2026-11-25-usbc-console-try.md`：那次停在 `IOBlockStorageDriver::start`，DWC3 没接管，主机侧一个串口都没有），
   且代价是一次断电重启 —— 与本目标"不要让用户按键"冲突。
3. `oem log` / `oem debuglog` 在任何会话都可用（11-25 那份 983 行固件 debug log 就是这么取回的），但它只有固件侧，
   没有 XNU 侧。

## 三、"带中断路径的载荷"就是手里这份文件（决定路线 B 不必存在）

`docs/results-2026-11-25-interrupt-kernel-provenance.md` §5 说发布镜像扫不到 `piano_gicv3_init` 但载荷是压缩的，
所以那只是"证据缺失"。本轮用 `tooling/persist/build-boot.py` 的 v2/v4 分区解析 + `common.fat32_read` 把
`out/k27-smprobe/boot-piano-ufs266-smprobe.img` 的 ramdisk FAT 真解开：

| 文件 | 字节 | sha256 前缀 | 中断路径符号 |
| --- | --- | --- | --- |
| `EFI/UniXNU/Payloads/kernel.macho` | 147,095,552 | `9f276dff4a35ef6e` | `piano_gicv3_init`×10、`piano_gic_irq_handler`×2、`piano_delay_timer_service`×6、`piano-gic`×2、`piano_dbg_kv`×4、`_piano_stage_slots`×4 |
| `EFI/UniXNU/Payloads/scratch-data.img` | 22,200,480 | `48dbc08d5955a202` | 无 |
| `EFI/UniXNU/Payloads/kernel.macho.trustcache` | 451,264 | `002db2e6ee9000c8` | 无 |
| `EFI/UniXNU/Payloads/kernel.macho.adt` | 15,736 | `4e904d90e92e880c` | 无 |

也就是说 `kernel.macho` 就是那份集合（`9f276dff…` = `piano-ufs266.kc`），它**明文带着** GICv3/架构定时器代码，
不需要"从内核缓存里提取内核"这种路线 B。发布镜像（`boot.img`/`vendor_boot.img`/`init_boot.img`/`boot-ram-check.img`/
持久 v4）里扫不到这些符号也一并确认：它们的 kernel/ramdisk/dtb 分区没有 gzip/lz4/zstd/xz 成员（脚本会解并再扫），
所以那份"作者出厂内核已带中断路径"的猜想可以放下了。装机 ESP 用的 `piano-ufs264.kc` 属同一族（11-25 已记它带这些符号），
"基座内核没有中断路径"只对 `docs/results-2026-11-25-ufs269-base-kernel.md` 里 `blendfix.macho` 那条链成立。

## 四、顺带纠正一条会影响无人值守设计的读法

装机 ESP 是 `--profile daily`，按 `docs/boot-guard.md` 它带 `piano-boot-guard=180`：180 s 内没看到 `launchd` 执行就
PSCI 复位**进 ABL fastboot**（看到则把重启原因改回 normal，之后的挂死会重启 macOS 而不是停在 fastboot）。
这条对本目标是好消息，也是验收设计的地基：不成功的 macOS 启动会自己回到 ABL，而 `fastboot reboot` 从 ABL 会启动装机系统
（boot-guard.md 表格第一行，verified），也就是"主机一条命令把设备送回 Android"，不需要人按键。前提是那次复位后 USB
连接还在（11-26 早上 v5 RAM 启动后整机掉总线是唯一已知的反例）。

## 五、下一步（顺序已按本轮结果改）

1. 先做那个**对照实验**：跑一次探针，然后走回 Android，再冷启动 reader，读同一页并与本轮这份 8192 字节比对。
   比对相同 → 中间启动没覆盖，冷会话窗口扫描可用；比对不同 → 仪器必须换（第 2/3 条路）。
2. dump 之后的 `fastboot devices` 挂死要在 `run-ufs266-smprobe.ps1` 与普查脚本里一起堵掉，否则无人值守循环会卡住。
3. 存储发布那一步的产物按 11-25 的更正清单做（带 `piano_gic*` 的谱系 + 上游 `NativeUfs` 对象 + PPI 27 到 Group 0 +
   解 `DAIF.F`），不再依赖只能产热会话转储的探针。