# 切换工具审计 — 2026-11-25

审计对象是 `yzddmr6/xiaomipad-8pro-mainline` 的电脑端切换脚本与设备内切换组件。结论先行：电脑端 `tools/piano-switch.py` 不适用于保留 HyperOS 3.0.309 的主线；设备内 `piano-boot-switch`、`piano-slot-success` 与 KernelSU 模块的门控值得复用，但成功位必须走我们自己的 fail-closed 单比特流程。

## 上游版本

此前 manifest 记录的是 v1.6.1（2026-11-18）。本轮复核时最新 release 已是 v1.6.3，发布时间 `2026-11-23T04:46:40Z`。

| 项目 | v1.6.1 | v1.6.3 |
| --- | --- | --- |
| `piano-ubuntu-*-tools.zip` | 35,336,074 B，sha256 `1e6b309b…` | 35,336,434 B，sha256 `680377bf2eeef574b02639bb6229cea3aab4ea0431ebf74110bb5a194cf42938` |
| `tools/piano-switch.py` blob | `c504b603ba1cb3629057afeb501afe5740819766`，3,111 B | 同左，逐字节相同 |
| 安装基线 | OS3.0.303.0.WPYCNXM，槽 A，512 GB | 未变 |

`SHA256SUMS.assets` 也随 release 更新（sha256 前缀 `e6d3652d…`，完整值以 release 页面为准）。v1.6.1 到 v1.6.3 的 32 个提交改动了 `device/boot/switch-common.sh`、`device/android/piano-switch-ksu/module/*`、`tools/build-device-debs.py`、`tools/build-kernel.sh`、`tools/test-desktop-integration.py`，没有改动电脑端切换脚本。

`docs/INSTALL.en.md`（v1.6.3）仍明确只支持 `OS3.0.303.0.WPYCNXM` 槽 A、512 GB，并且**会擦除全部 Android 数据**（重建 userdata、清 metadata）。本机是 `OS3.0.309.0.WPYCNXM`，保留 Android 是硬要求，因此电脑端安装器不能使用。

## 电脑端脚本的行为

读完全部 3,111 B 源码后确认，`piano-switch.py` 只做四件事：

1. `getvar product/unlocked/current-slot/is-userspace` 四项预检。
2. 按 `SHA256SUMS` 校验镜像大小与哈希。
3. 顺序执行 `fastboot flash init_boot_a`、`vendor_boot_a`、`boot_a`。
4. 任一步失败时回灌 rollback 三件套。

它没有槽成功位校验，没有重试次数门控，也没有写后整分区读回。所有写入都依赖 fastboot 直接操作分区。这套流程与“可诊断、可回灌、读回验证”的主线要求不匹配，且会把安装器绑定的 303 基线带进来。

## 设备内组件的能力

同一 release 的下列文件已取到 `out/switch-audit-163/`（`out/` 被忽略，不入库）：

- `device/boot/piano-boot-switch`（25,104 B，Python 3）：只操作槽 A 的 `init_boot_a`、`vendor_boot_a`、`boot_a`；store 位于 `/var/lib/piano/switch/{ubuntu,ubuntu.prev,android}`；用整分区 SHA-256 判定占用者，GPT 属性位变化不影响判断；写失败自动回灌原占用者且不重启；不修改 vbmeta、槽 B、GPT 或其它分区。子命令为 `status/verify/to-android/to-ubuntu/import-android/import-ubuntu`，支持 `--dry-run/--reboot/--force/--desktop/--only-if-ubuntu`。
- `device/boot/piano_bootslot.py`（25,607 B）：解析 GPT 槽位，b48–49 priority、b50 active、b51–53 retry、b54 successful、b55 unbootable；`slot_state()` 要求 A 是唯一 active、priority 3、非 unbootable。
- `device/boot/piano-slot-success`（8,358 B）：fail-closed 置 b54。先校验两份 GPT 一致，先写 backup（entries + header，重算 CRC）再写 primary，逐字节读回比对，只允许 boot_a 条目字节 54 与两个 CRC 变化；要求 `retry != 0`，且槽内容必须是已登记 set。支持 `--check/--mark/--dry-run`。
- `device/android/piano-switch-ksu/module/piano-switch.sh`（7,910 B）：Android 侧 KernelSU 模块，只从 `linux_rootfs` 以 `ro,noload` 只读挂载取镜像（journal 脏则拒绝），dd 到槽 A，每个分区 SHA-256 读回，失败回灌 android set；`--check/--no-reboot`；退出码 0/1/2/3。模块版本 v0.2.2。

这套组件的方向与主线一致：切换只碰槽 A 三件套、按内容而非属性判断占用者、失败回灌、成功位单独 fail-closed 写入。可直接借鉴其判定与回灌顺序。

## 冷启动与验收门控

`tools/piano-cold-boot-accept.py`（16,744 B）只读保存的采集或经开发者 SSH 采集两段数据，缺失证据一律 `UNAVAILABLE`，绝不当作 PASS。窗口由采集里的 `UPTIME` 决定，第二次采集要求 ≥ 200 s，且两次必须同一 `BOOT_ID`、同一内核版本、root 身份。判据包括：simpledrm 首帧 < 1 s、`switch_root` ≤ 10 s、gdm 20 s 内 active、GMU 固件 ≤ 20 s、GPU 错误计数全 0、时钟/DSI 警告全 0、256 MiB 拷贝 ≥ 2000 MB/s、HF-AXI 全部保持、INTF1 与 INTF2 各 +72±2 帧/0.5 s、失败单元为 0、Wayland 会话恰为 1、声卡存在、THP 首帧 > 0 且错误为 0。任一 FAIL 覆盖 UNAVAILABLE，verdict 为 FAIL/UNAVAILABLE/PASS 三态。

`tools/piano_boot_acceptance.py`（49,852 B）提供同一套三态判据与解析器，覆盖启动模式、simpledrm、HF-AXI、switch_root、gdm、GMU、GPU 错误、拷贝带宽、scanout、THP、HCI、失败单元、电源同步、内核 release、模块归属、配置与 getvar 槽位。它对“journal 接收时间”和“内核时间戳”做了区分，时间预算只认内核时间。

这两个工具可以直接作为我们三次冷启动与日用验收的判定器；采集脚本依赖设备内开发者 SSH，等 macOS 用户层可用后再接入。

## 与主线现有工具的对齐

主线已有：

- `tooling/persist/maint-session.py` 的 `slot-success` 子命令（约第 2216 行）：只改 boot_a GPT 条目 bit 54。
- `piano_309_gpt_write.py`：先 backup 后 primary，写入后读回。

两者与上游 `piano-slot-success` 的 fail-closed 思路一致。主线继续使用自己的实现，吸收上游的三点约束：两份 GPT 一致才写、只允许 bit 54 与 CRC 变化、写完逐字节读回。

## 决定

1. 不采用电脑端 `piano-switch.py`，不引入 303 基线安装器，不执行任何 fastboot 分区写入。
2. 切换主线采用设备内方案：只碰槽 A 三件套，内容哈希判定占用者，失败回灌，成功位单比特 fail-closed。
3. 冷启动与日用验收采用上游三态判据（PASS/FAIL/UNAVAILABLE），并保留我们自己的采集与日志。
4. 本审计仅完成源码与文档核对，未在平板执行任何切换操作。