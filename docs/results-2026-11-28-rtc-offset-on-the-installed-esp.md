# 2026-11-28 把 PMIC RTC 的日历装进日常 ESP：`piano-rtc-offset` 先差 3.4 秒，改成 1776516618 之后开机历元误差 0.1 秒

测量 2026-11-28 17:28~18:40（本地）。装机集这次**动了 ESP**：日常 ESP 从 ul11 换成 **ul14**，
`boot_a` 仍是 `boot-piano-macos-persistent-v6-sealed.img` `df6d5a18f27054e69a394810e2b4d3452818d428cbed8795ea0b11641c5b1f30`
（设备侧回读 `df6d5a18f27054e69a394810e2b4d3452818d428cbed8795ea0b11641c5b1f30`），Data store 仍是
`linux_data` LBA 117735999 上 seed `b2583ea517732505` 那一个（generation 4，`state 2 resumed`）。

## 0. 结论

- 现象原来是这样：每次开机内核历元差三天多，靠 `timed` 联网纠回来。根因不是 RTC 计数器不走，而是缺"纪元偏移"那一格
  （SDAM 偏移格读出来是 0，见下面 `rtc: RTC_RW ... offset cell reads 0`）。
- 现在日常 ESP 的 macos 入口带 `piano-rtc=1 piano-rtc-offset=1776516618`，开机第 0 秒历元就是对的，实测误差 0.1 秒。
  这不再需要 `timed` 参与：ul14 那一轮 `date +%s` 减 `kern.boottime` 等于真实流逝时间（398 秒），
  中途没有被 NTP 拽过一大步。
- 中间那一版（ul13）用的 `1776516615` 是**错的**，开机历元慢 3.4 秒。错法是拿"控制台行 + 当时的 `kern.boottime`"配对，
  而那个 `kern.boottime` 本身就是被上一次 `timed` 纠过的，等于用被纠过的钟去定偏移，自证循环。
  定偏移必须用一个网络时间服务纠过的参考钟（这次是 `mac-lan`），并用带毫秒到达时间戳的串口读法（`tooling/native/rtc-stamp.ps1`）。

## 1. 装机集的两次变化

| ESP | sha256 | 相对 ul11 的差异 | 会话日志 | 结果 |
| --- | --- | --- | --- | --- |
| `esp-7ccdd855-k273-ul13.img` 1,073,741,824 B | `82203354c6c4b613922878ec2cc3896139e3d6ee40e27e2b19e35fa60b701707` | macos 入口加 `piano-rtc=1 piano-rtc-offset=1776516615`（走 ADT 尾巴，入口行 694 字节逐字节不变） | `logs/session-flash-ul13b-20261128-175843.log` | `done rc=0` |
| `esp-7ccdd855-k273-ul14.img` 1,073,741,824 B | `252d25696db55071b07a4787a48874d36fc0df76ca639171ed03612e09d51f4d` | 只把那个数换成 `1776516618` | `logs/session-flash-ul14b-20261128-182737.log` | `done rc=0`，装机后设备侧回读 `sha256=252d25696db55071b07a...` |

构建命令（两条只差那个数）：

```
PIANO_UNIT_PROFILE=/mnt/e/project/UniXNU-xiaomipad8pro/out/unit-profile/piano-7ccdd855-shim.json \
python3 tooling/k273/build-k273-esp.py --base out/persistent/esp-7ccdd855-k273-ul11.json --entry macos \
  --root-part linux_rootfs --arg piano-rtc=1 --arg piano-rtc-offset=1776516618 --name esp-7ccdd855-k273-ul14
```

刷写前的闸门（两次都全绿，值得记下来当口径）：`boot_a` 回读哈希对上；store 头 `SHNGDATA` seed `b2583ea5…`
"IS the one this ESP resumes"；RAM 窗口配对 `boot_a … takes the RAM window of ESP … (13310 MiB): its DarwinBoot's
bound is 13312 MiB`；`linux_rootfs` 块 0 的 `SHNGROOT` 清单哈希 `c16d365e01e5c9d7…` 前后不变；`vendor_boot_a` 不变；
GPT 新表 + `sgdisk` 干净；ESP 逐段回读哈希等于镜像哈希。

## 2. 那个数是怎么定错的、又是怎么定对的

错的一版（ul13）：拿 17:15 那次开机的两条 `PianoPower: … rtc: <count>` 控制台行配上当时认为正确的
`kern.boottime` 1795857412/1795857472，得 `19340797 ↔ 1795857412` → `1776516615`。问题是这个配对的"真实时刻"来源
不可靠：控制台行没有到达时间戳（`capture-com.ps1` 只给开合事件打时间戳），而 `kern.boottime` 在那个窗口里已经被
`timed` 纠过，两者一配就带了系统性的几秒。

对的一版（ul14）：新工具 `tooling/native/rtc-stamp.ps1` 直接在 COM5 上给每条 `rtc:` 读数打毫秒到达时间，
并且能在复位期间端口消失后自己重开；同时把主机时钟和网络时间源对表（本轮三次测都是主机慢 2.0 秒：
`host_utc=1795861377/maclan=1795861379`、`1795861473/1795861475`、`1795862208/1795862210`）。
ul13 那一轮三条稳定读数（相邻间隔 30 秒，抖动 ±0.13 秒）算出来的偏移都是 1776516618.4：

```
GOT 18:23:06.205 count=19344769 cal=2026-11-28 10:23:04 UTC   ->  真实 1795861387.4 - 19344769 = 1776516618.4
GOT 18:23:36.212 count=19344799 cal=2026-11-28 10:23:34 UTC   ->  1776516618.4
GOT 18:24:06.213 count=19344829 cal=2026-11-28 10:24:04 UTC   ->  1776516618.4
```

## 3. 装机后的实机结果

ul13 那一轮（18:04:38 放行，两轮重启）：

- 控制台 `PianoPower: [0s] rtc: 19343686 (2026-11-28 10:05:01 UTC, offset 1776516615 from piano-rtc-offset), enabled 1`，
  `[30s]`/`[60s]` 各 +30 计数；`kern.boottime` = 1795860302（18:05:02）。
- 18:12:12 主机下 `shutdown -r +1`：设备自己回到 macOS（不是 ABL），`kern.boottime` = 1795860845（18:14:05），
  `boot-guard` 在 launchd 那一刻（`exec of launchd at 2 s`）判健康。
- 但这一轮的开机历元仍慢 3.4 秒，`timed` 之后才对表（18:19:26 设备 vs 18:19:46 `mac-lan`）。

ul14 那一轮（18:33:12 放行）：

- 到达时间戳读数（同一台主机、同一对表口径）：
  `GOT 18:36:36.901 count=19345580 cal=2026-11-28 10:36:38 UTC` → 真实 1795862198.1，RTC 给 1795862198，**误差 0.1 秒**。
- SSH 落地后 `date +%s` = 1795862414，同一时刻 `mac-lan` = 1795862415、主机 = 1795862413；
  `date - kern.boottime` = 1795862414 - 1795862016 = 398 秒，与真实流逝一致 —— 没有 NTP 大步纠正的痕迹。
- Data 读回：`ul11-rt1.txt a3f8428da2559d1e5a8bb76d6f9f745e`、`ul11-rt2.txt 843081c6cfb64ce2a59b7f7635f24d81`、
  `ul11-hard.txt ef00c0de656a58302a26add9695a78a7`、`ul11-cold1.txt 0480425f279a5066c014f09f31e3d370`、
  `ul11-cold2.txt 4b90aae32365f01ead873f6f4284bfe2`、`ul11-cold3.txt 80e33cf18d7109fea64ba766dc9b3405`、
  `ul12-c1.txt fe817565f59968e70501cd642eac001a`、`ul12-dirty2.log` 242,094 字节
  `dcf0b77723c5e2e97ec167fb496322b2` —— 八件在 ul13 刷写、一次正常重启、ul14 刷写之后逐个逐字读回。

顺带钉死两件流程上的事：

1. **进 ABL 不需要按键也不需要诊断 ESP**：macOS 侧 `tooling/native/piano-console.ps1 -Port COM5 -Line 'reset fastboot'`
   之后 19 秒到 `7ccdd855 fastboot`（本轮两次都这样）。上一份交接里"刷 ESP 前必须先做一个带 `piano-reset-to` 的
   诊断 ESP"这个前提是多余的。
2. `patches/k27/0057-…piano-reset-to` 的解析点在 `piano_reset_init()` 里 `piano-reset-after` 缺失时的提前返回**之后**，
   所以只给 `piano-reset-to` 而不给 `piano-reset-after` 是不会生效的；而日常 ESP 不能带它
   （`docs/boot-guard.md:70,105`：健康检查会把复位原因永久留在 `bootloader`）。

## 4. 还没证明的、以及风险

- **跨 Android 往返之后这个偏移还成不成立没验**。RTC 计数器由 PMIC 自己走，HyperOS 起来会不会改写它（或者写那格 SDAM
  偏移）我们没测。下一轮 Android→macOS 往返要在 macOS 起来的第一时间读一条 `rtc:` 并和 `mac-lan` 对表；
  如果偏移变了，说明必须让某处把内核时钟写回 RTC（`docs/drivers/rtc.md` 的 `settime.sh` 那条路），
  而不能只靠一个静态偏移。
- 这台板子的 RTC 走时误差本身没测（要在无网络、跨若干小时里读计数器增量和对端增量）。目前的 0.1 秒只覆盖
  "开机那一刻"，ul14 那一轮跑到 +7 分钟没有漂移迹象（30 秒间隔抖动 ±0.13 秒，历元每秒推进）。
- 用户态日志的时间戳仍是另一套：启动捕获里 `com.apple.xpc.launchd|2026-11-25 13:53:05` 这类行在两次开机里都出现
  （`logs/rtc-ul13-boot-1804.log` 里这样的行有 114 条，最早在第 4242 行），而同一时刻 PianoPower 的历元是对的。
  嫌疑是入口里那条 `droidvm-utc=1795478400`（那是 VM 线的时钟锚点）。也就是说**内核时钟修好了，
  某些用户态日志的打印基准没跟着修**；要不要去掉 `droidvm-utc` 得单独试，它同时也是 VM 时间基的锚。
- `AMFI: Denying core dump … Process[…] crashed: AppSSODaemon` 在 ul13/ul14 两轮里照旧（各 4 条起），
  这条跟时钟无关，记在这儿只因为同一份捕获里能看到。
- 开机 load average 仍然虚高（ul14 那一轮 `up 6 mins` 时 581.88/349.37/162.35，且随时间衰减；
  前一天有一次在 +29 分钟仍是 730/723/608）。这条没并到本结论里，另案。

## 5. 复现

```
# 1. 参考钟与主机钟的差
ssh mac-lan date +%s            # 与 [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() 同时刻取
# 2. 抓 RTC 读数的到达时刻（复位期间会自己重开口）
tooling/native/rtc-stamp.ps1 -Ports COM5 -Seconds 330 -OutFile logs\rtc-stamp-live.txt
# 3. 偏移 = 真实 UTC - count，取整即 piano-rtc-offset；本轮 1795862198.1 - 19345580 -> 1776516618
# 4. 重打日常 ESP 并刷（见 §1 的命令），进 ABL 用 COM5 的 'reset fastboot'
```