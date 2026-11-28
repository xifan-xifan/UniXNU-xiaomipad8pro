# 2026-11-28 整机被一个进程按住：PowerLog 的 SMC 探针在没有 SMC 的板子上空转

装机集不变：`boot_a` macOS `df6d5a18…` / Android `7c444a2d…`，ESP `44de650a…`，Data store seed `b2583ea5`。
这一篇解释为什么以前每次 `top` 都是 `0.0 % idle`、`Load Avg 600+`，以及把它停掉之后拿到了什么。

## 怎么找到的

1. 杀掉软解壁纸之后 `Load Avg` 掉到 35，但 `PerfPowerServices` 立刻被 launchd（`spawn type = adaptive`，keepalive）重新拉起，又是 52–68 % CPU。
2. `sample` 它的调用栈：`-[PLSMCMetricsAgent getAllKeys]_block_invoke`（PowerlogLiteOperators）+ `SMCGetKeyFromIndex`（libSMC），
   外加 `objc_msgSend` / `objc_autoreleasePoolPop` 的海量对象翻腾 —— 它在按 Mac 机型那套 SMC 键表逐个索引查询。
3. 这台板子默认**根本没有 AppleSMC 服务**：`piano-smc` 不带就是 0，`package/PianoPolicy/AppleSmc.cpp` 那个只认 `MSHP` 的替身也不会发布
   （`sysctl machdep.piano_smc` 不存在即为证）。也就是说 powerlog 在对着"不存在"反复问，而且没有退避。
4. `launchctl disable` + `bootout` 之后它整个启动周期都没再起来（`disable` 单独用不管用：那次它照样被拉起来，必须两个都做）。

## 数字（同一台、同一次启动，前后各测一次）

| 量 | 之前 | 之后 |
| --- | --- | --- |
| `CPU usage` | `10 % user / 89 % sys / 0.0 % idle`（多轮都是 0.0） | `3.6 % user / 38.7 % sys / 58.3 % idle` |
| `Load Avg`（1 分钟） | 660–800 | 16.8 |
| `networksetup -listallhardwareports` | 几十秒 | 0.07 / 0.08 / 0.08 s |
| 前 6 名 CPU | PPS 45–68 %、ReportCrash 27 %、VTDecoder 39 % | `top` 自己 20 %、`kernel_task` 8.4 %、其余 ~1 % |
| `ps -Ao state` 普查 | 进程里只有 ~10 个 R/U，其余 S | 一样 —— 见下面"指标本身要打折" |

**指标打折**：`Load Avg` 几百的时候 `ps` 状态普查里真正 runnable/uninterruptible 的进程只有 10 个上下，所以这台机器上的 `Load Avg`
不能照 Mac 的口径读；`0.0 % idle` 同样可疑（可能是空闲 tick 在我们平台上就记不准）。真正能定案的是墙钟：同一个 shell 循环
10 万次 3.18 s、`networksetup` 0.07 s、`top` 自己占 20 % —— 机器该多快就是多快。这条要记进测量手册。

## 停机是不是砍功能

不是。`PerfPowerServices` 是电源**遥测**（powerlog 采集/导出），不是电源管理：断言和睡眠在 `powerd`，电量读数来自我们自己的
`IOPMPowerSource`，"能耗模式"三档开关来自 `MSHP` 替身（`package/PianoPolicy/AppleSmc.cpp`）。被它带崩的下游才是真问题：
`watchdogd` 崩溃循环喂给 `ReportCrash` 的那一大块 CPU、sshd 开机等主机密钥等到 boot+300 s、802.1X 客户端第一条日志 boot+471 s、
`WindowServer` 六帧每秒，全是在一个忙死的核后面排队的表现（因果还没逐个复测，见下）。

## 落地

- 新 unit `piano-powerlog-policy`（`configs/userland.toml`，`default_on = true`）：
  `tooling/userland/piano-powerlog-policy.sh` + `com.unixnu.piano-powerlog-policy.plist`（`RunAtLoad` + `LaunchOnlyOnce`），
  走 `launchctl disable` + `bootout`，把动手之前的 override 记在 `/private/var/db/piano-userland/powerlog-policy/original`，
  `--remove` 原样交回去。和 `piano-passd-policy` 一个套路，不改苹果的文件、不碰 launchd 数据库。
- 主机测试 `tooling/userland/test-powerlog-policy.py`（launchctl 桩）：装/幂等/被重新拉起再 bootout/`--check` 诚实/记录与还原，WSL 里 PASS。
  `stage.load_manifest` 认下这个 unit（`kind = file` + `kind = daemon`，root:wheel，755/644）。
- 设备实机：`--apply` 与 `--check` 都 `PASS disabled=true loaded=false policy=powerlog1`；`plutil -lint`、`sh -n` 过。
  踩到一个自己写的坑并已修：`--apply` 之前没有 `mkdir -p` 状态目录，锁目录建不出来就报"state is locked"（开机作业正是直接 `--apply`）。
  收据里先记到的 `disabled` 是我这次手工 `launchctl disable` 留下的，已经改回 `default`（真实原始状态）。

## 还欠的账

- 停 PPS 之后：`watchdogd` 崩溃循环、`shutdown_stall`、`swap_wait` 失败数、帧率、Wi-Fi 首关联那 5–8 分钟，都要按新条件重测一遍才有资格说话。
- `0.0 % idle` / `Load Avg` 这两个指标在本平台是否天生不准，需要单独定案（不影响本次停用的决定）。
- 出网/DNS 仍未解，`git push` 原文：`ssh: Could not resolve hostname github.com: Name or service not known`。