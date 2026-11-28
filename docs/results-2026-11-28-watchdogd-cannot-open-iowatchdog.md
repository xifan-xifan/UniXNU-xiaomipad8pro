# 2026-11-28 watchdogd 为什么每次都 SIGTRAP：它要 `IOWatchdog`，这台板子没有；补一个策略单元把开机风暴的这一头关掉

装机集没换：`boot_a` macOS `df6d5a18…`、ESP ul11 日常 `44de650a…`、Data seed `b2583ea5`。全程无人值守，没有按键、
没有长按、没有新备份。

## 符号化：`+0xc948` 是 `__os_crash` 之后的 `brk`，不是随机崩溃

`/Library/Logs/DiagnosticReports/watchdogd-2026-11-24-220316.ips`：`EXC_BREAKPOINT (SIGTRAP)`，
`codes = 0x1, 0x100dc0948`，触发线程只有四帧 —— `watchdogd+0xc948`、`+0x2bd8`、`+0x5b50`、`dyld+0x31f90 (start+6652)`，
`slice_uuid 3258367c-791c-38d8-89c8-b5de978b556e`。

设备上的 `/usr/libexec/watchdogd` 取出来核对过：sha256 `78bdcbdf831850a49ff9d50859b1f608482f6ad8c57a4c078fad134a99f52565`，
arm64e 切片的 UUID 与崩溃报告一致。在 `mac-lan` 上反汇编（`otool -arch arm64e -tV`）：

- `+0x2af8` 调 `IOServiceMatching`，匹配类名就是字符串表里的 **`IOWatchdog`**；`+0x2b04` `IOServiceGetMatchingService`，
  `+0x2b2c` `IOServiceOpen`。
- 找不到服务时先问一句"是不是虚拟机"：分支条件读 `kern.hv_vmm_present`，是虚拟机就打印
  `detected virtual machine environment and no watchdog KEXT found, exiting...` 然后**正常退出**（`+0x2b98`）。
- 原生机器没有 `hv_vmm_present`，于是走 `bl +0xc930`：那里 `__os_crash("connect_to_kext() unexpectedly failed -
  unable to lookup watchdog KEXT service")` 紧跟 `brk #1` —— `brk` 的返回地址就是 `+0xc948`，也就是崩溃报告里那一帧。

所以这不是"watchdogd 有 bug"，而是它按 Mac 的假设活着：要么有 `IOWatchdog`，要么在虚拟机里。这台板子两样都没有，
它就在 check-in 之前死了；launchd 再拉、ReportCrash 再记，它一件保护都没提供。真正的修法是我们自己发一个
`IOWatchdog` 服务，把 `kIOWatchdogDaemonCheckEnabled`/`CheckUserspaceDefanged`/`Checkin`/`TestCPUHang`/panic 这几个
标量方法接住（选择子编号都能从这份反汇编里读出来），后面接 PianoPower 已经在拨的 Gunyah 看门狗 —— 那是 B 级活，不是今天这一步。

## 今天的这一步：新策略单元 `piano-watchdog-policy`

`tooling/userland/piano-watchdog-policy.sh` + `com.unixnu.piano-watchdog-policy.plist` + `configs/userland.toml`
（`default_on = true`），照 `piano-powerlog-policy` 那套写：动手前把 launchd 原有的 disabled 覆盖记到
`/private/var/db/piano-userland/watchdog-policy/original`，`launchctl disable` + `bootout`，`--remove` 原样交回，
Apple 的文件一个字没改。

- 主机测试 `tooling/userland/test-watchdog-policy.py`（launchctl 桩，临时假 root）WSL 里 PASS：装得上、幂等、
  launchd 拉回来再被 bootout、`--check` 不撒谎、原有状态记下来也还得回去。
- `stage.load_manifest` 认下这个单元（32 个单元，两个安装文件：`/usr/local/libexec/piano-watchdog-policy` 0755、
  `/Library/LaunchDaemons/com.unixnu.piano-watchdog-policy.plist` 0644）。
- 设备 `--install` / `--check` 都报 `PASS disabled=true loaded=false policy=watchdog1`；
  `launchctl print system/com.apple.watchdogd` 从此是 `Could not find service`，
  `launchctl print-disabled system` 里 `"com.apple.watchdogd" => disabled`。
## 无人值守重启实测（`shutdown -r` 17:15:01，控制台抓 900 s：`logs/watchdog-policy-boot-1714.log`）

- 单元**自己跑了**：`/private/var/db/piano-userland/watchdog-policy/status` 的时间戳就是这次开机，内容
  `PASS disabled=true loaded=false policy=watchdog1`；开机后 `launchctl print system/com.apple.watchdogd` 找不到服务。
- 控制台整段 900 s 里 `Process[…] crashed` 只有 **2** 条，全是 `AppSSODaemon`，**watchdogd 一条都没有**。
  对照 12:24 那次开机风暴的量化记录（launchd 六分钟窗口里 throttle 6144 次、crashed 944 次），这一头的循环是断了。
- `piano-boot-guard: healthy after 180 s, restart reason normal`；`NativeRamBlock: Data store state 2 (resumed) …
  seed b2583ea5`；`piano-boot-args … boot-args 1124 bytes (line 694)` —— 还是 ul11 那条日常行。
- root shell（TCP 2222 + ssh 都能用）17:20:38，内核 `kern.boottime` 反推开机 17:16:22，也就是 **boot+256 s**
  （今天早些时候三次量到 230 / 271 / 377 s，这次落在同一区间）。
- 重启后读回：`ul11-rt1 a3f8428d…`、`ul11-rt2 843081c6…`、`ul11-hard ef00c0de…`、`ul11-cold1 0480425f…`、
  `ul11-cold2 4b90aae3…`、`ul11-cold3 80e33cf1…`、`ul12-c1 fe817565…` 七个 MD5 全部不变，
  `ul12-dirty2.log` 242,094 字节还在。这是又一次"写文件→重启→读回"的凭据。

## 顺带把"时钟不持久"这件事钉到了具体那一位上

- 这次开机后 `date` 是 `Nov 25 13:57`（差三天），`kern.boottime` 退回常数 `1795585949`；到 17:21:44 又被 `timed`
  纠正成 17:21:44（`kern.boottime` 跟着变成 1795857382 = 开机 17:16:22）。也就是说现在"时间是对的"靠的是联网校准，
  离线开机就会带着错三天多的时钟跑。
- 原因在控制台上写得很明白：`PianoPower: [0s] rtc: 19340766 (+ offset 0 from the SDAM cell = 19340766, year 1970:
  not a date, measure piano-rtc-offset), enabled 1`。PMIC RTC 计数器本身在走（`[0s]` 19,340,766 → `[30s]` 19,340,797，
  30 秒走 31 跳，跟得上），缺的是**纪元偏移**：SDAM 那一格现在是 0，所以算出来的年份是 1970，内核就不采纳。
- 下一步很具体：把 `piano-rtc-offset` 量出来（同一时刻的墙上时间减 RTC 计数）写进那一格，之后 RTC 就能自己跨掉电带时间，
  不再依赖 `timed`。这一格是要写盘的，写完要用"改时间→重启→看 `date`"来验。

## 这一篇证明了什么，没证明什么

证明了的：watchdogd 崩溃的确切原因（缺 `IOWatchdog`，且它的安静退出口只在 `hv_vmm_present` 为真时开着）；策略单元在
主机测试、manifest、设备 `--apply`/`--check` 和"开机自动跑"四个层面上都成立；这次重启里 watchdogd 崩溃归零、
Data 读回不变、启动仍然 180 s 判健康。

没证明的：`AppSSODaemon` 还在崩（这次 2 次，也是 `Too many corpses`），`cryptexd` 那两条 `Preboot/Cryptexes/{ExclaveOS,Rosetta}`
还补不了 —— `/System/Volumes/Preboot` 与 `/` 同一个 `dev`（16777221），在那儿 `mkdir`/`touch` 都是 `ENOENT`，
只能回到 `tooling/k273/make-systemos.sh:70` 那次组装里补，运行时改不动；`IOWatchdog` 原生服务还没写；
`piano-rtc-offset` 还没量。