# 2026-11-26 - 装机集第一次带计时的从盘启动：72 秒回到 ABL，CDC-ACM 从未出现

`tooling/native/run-disk-boot-experiment.ps1`（本轮新增，无人值守）：写 macOS 启动集 → 从盘启动 → 每 5 秒记一次状态迁移
（fastboot / adb / `1209:0001` 串口）→ 无论结果都恢复 HyperOS 三元组并证明起来。日志
`logs/native-runs/disk-boot-20261126-123345/run.log`。

## 写了什么（都有哈希门槛）

| 分区 | 源 | sha256 前缀 | 结果 |
| --- | --- | --- | --- |
| `init_boot_a` | Ubuntu v1.1.0 `init_boot.img` | `96057eb263fb` | OKAY（8 MiB，0.4 s） |
| `vendor_boot_a` | Ubuntu v1.1.0 `vendor_boot.img` | `7cbb06b9eddb` | OKAY（2.7 s） |
| `boot_a` | `boot-piano-macos-persistent-v4-avb.img` | `0d52dd5e2c63` | OKAY（2.7 s） |

写入顺序按切换工具的顺序（init → vendor → boot），三个源在写之前都先与记录哈希相符；`--set-active=a` 之后 `reboot`。
恢复用的 HyperOS 三元组同样先验哈希（`7c444a2d…`/`7f766f1d…`/`56197797…`，11-25 的验证备份）。

## 计时结果

```
12:36:35.564 reboot issued at t0
12:37:47.282 t+72s state=abl
12:38:13.663 hyperos=up build=OS3.0.309.0.WPYCNXM
```

- **t+72 s 回到 ABL**。这不在两个已知窗口里：预看门狗挂死是 30–45 s（RAM 那批 37.2/38.6/44.9/51.9 s），
  `piano-boot-guard=180` 的"没等到 launchd"判定是 180–210 s。72 s 与两条都对得上：
  (a) guard 从 EARLY_BOOT 起每 10 s 喂狗，核挂死后 32 s 咬 → 核大约死在 40 s；
  (b) guard 的"某颗核两拍没中断"检查（20 s 起）→ PSCI 复位随时可发生。两者都表示**死得比 RAM 那批晚、但远早于 launchd**。
- **`1209:0001` 的 CDC-ACM 串口在 72 秒里一次都没出现**（轮询每 5 s 查一次 `serial.tools.list_ports`）。装机参数是
  `piano-usb=2`，而 `PianoDwc3` 是第 12 个原生 kext —— 与 `docs/results-2026-11-25-usbc-console-try.md` 同一结论：
  这一次仍然没走到存储发布之后的世界。
- 恢复分支按设计工作：写回三元组、`--set-active=a`、`reboot`，`hyperos=up`。**全程没有任何一次需要人按键**。

## 与既有记录的关系

装机 ESP + 盘上的根 + `data-persist=2` 的空白 store，这套组合此前只在 11-25 被冷启动过几次且**没有计时记录**
（`docs/results-2026-11-25-slot-a-bootset-revert.md` 只记了"A 槽是 macOS 集、反复落 ABL、retry 被烧光"）。
本轮把"反复落 ABL"量化成 72 s，并且证明：装机集的失败启动**自带回到 ABL 的能力**，主机一条 `fastboot reboot`/一次刷回即可走回
HyperOS —— 无人值守的 Android→macOS→Android 循环在机制上已经成立，缺的只是让 macOS 活过那 40–70 秒。

## 下一步（同轮接续）

`tooling/native/run-pstore-after-disk-boot.ps1`：同样的从盘启动，回到 ABL 后**不经过 Android**直接
`fastboot boot reader.img`，会话第一条命令 `oem dump 9cb00000 2000` —— 装机参数把 XNU 控制台镜像到
`pstore-console=0x9cb00000`（`pstore-console-size=0x700000`），复位不清 DRAM，这是唯一不需要 XNU 侧仪器配合就能拿到
XNU 侧文本的通道（11-25 对 `0x9cb00000` 的失败读发生在旧规则的热会话里，规则已被本轮推翻）。