# 2026-11-28 - ul11 的两轮重启、开机即起的 sshd，以及 802.1X 客户为什么这么晚才到

装机集：`boot_a` = `boot-piano-macos-persistent-v6-sealed.img` `df6d5a18…`，ESP = `esp-7ccdd855-k273-ul11.img`
`44de650a17c6de8e8e784a0db005dc25db72662183239cfb86986b3105147b38`（11:22–11:26 刷入，`logs\session-flash-ul11-20261128-112216.log` `done`）。
Data store 全程没动：seed `b2583ea5`、绑定 `linux_data` LBA 117735999–124027647、generation 4。ul11 相对 ul10 的 payload/ADT/trust cache
逐字节不动，macos 条目只多了 `piano-wifi-clientwait=120000`；控制台 `piano-boot-args: /chosen/piano-boot-args-macos (430 bytes): 429 bytes
appended, boot-args 1124 bytes (line 694)`（上限 4095）。

## `piano-wifi-clientwait` 确实等了，但等不到：客户是 5–8 分钟量级才到

- ul11 第一次启动（`logs\ul11-boot.log`）：`+5607 ms` 开始等（"waiting for the 802.1X client … up to 120 s"），`+125615 ms` 等满退出，
  `+126618 ms` 关联但没有客户 → 四次握手超时 → `deauthentication, reason 15`，退避 2/5/10/20/30 s 连撞 5 次；
  客户在 `+285557 ms` 出现（`the port: a client (1 connect(s)…)`），`+290556 ms` `keys: 2 sent, 2 installed`。
- ul11 第二次启动（`logs\ul11-restart-1.log`，正常重启）：同样的 120 s 等待、同样的撞墙，客户 `+285497 ms`、密钥 `+290497 ms`。
- ul11 第三次启动（`logs\ul11-hardreset-1.log`，控制台 `reset`）：撞在 `+224653/+290051/+355487 ms`，客户 `+476747 ms` 才到，链路随后起来。
- 所以 120 s 的等待不是修法：客户的到达时间在 285 s 与 477 s 之间摆动，加大等待只是把首次关联推迟到更晚。

## 客户晚到的直接证据在 `piano-wifi-supplicant` 自己身上，不在驱动

- 驱动的 802.1X 端口很早就在：`portRegister()`（`package/PianoWifi/PianoWifi.cpp:4322`）在 `start()` 的最开头被调用
  （`if (gArgs.level >= 8) portRegister()`，:6140），控制台里 `the 802.1X port … registered` 就在 `stage 1: GCC, TCSR and TLMM` 之前。
- `piano-wifi-supplicant` 进程在 boot+61 s 就有了（`ps -axo pid,lstart,comm`），但它 `run()` 的第一件事是 `load_pmk()` 读
  `/private/var/db/piano-wifi/profile`，而这一轮它的第一条日志（`profile read: a network name of 11 byte(s) …`）出现在
  `2026-11-25 14:00:20.045`，也就是 boot+471 s；紧接着 4 ms 后就 `connected to the 802.1X port`，`+7 s` 完成四次握手并 `authorize sent: 0`。
  端口从来不难找（连上就成功），难的是这个进程在第一条日志之前不动。
- 上一轮（11:51 那次启动）里它甚至先打过 `the 802.1X port is not there yet (No such file or directory): waiting`
  （`log show` 里 pid 262 每 60 s 一条），后来 `the port is open (generation 7)`；`launchctl print` 显示 `runs = 1`、从不退出。
- 背景原因待查，但今天有硬数据：`top -l 1` 在这一轮报 `398 total, 315 running, 2149 threads`、`CPU usage: 50.95% user, 49.4% sys, 0.0% idle`、
  `Load Avg: 765`，而 CPU 大户只有 kernel_task 27%、VTDecoderXPCService 25.6%、WindowServer 16.2%、WallpaperAerials 2.5%。
  也就是说整机用户态确实被拖慢了，`networksetup` 之类的调用也要几十秒到几分钟才回（本机实测）。这一条列在待查里，别当已解决。

## 新 unit `piano-sshd`：root shell 不再等 COM6

- 加 `tooling/native/piano-sshd/{piano-sshd.sh,com.unixnu.piano-sshd.plist}` 与 `configs/userland.toml` 的 `[[unit]] piano-sshd`
  （`default_on = true`，装成 `/usr/local/libexec/piano-sshd` 和 `/Library/LaunchDaemons/com.unixnu.piano-sshd.plist`）。
  `stage.load_manifest(configs/userland.toml)` 现在 29 个 unit，加载与检查通过（Linux 侧能跑的这部分；`tooling/userland/test-stage.py` 要 macOS 的
  `xattr`，本机跑不了）。
- 门槛：`/var/root/.ssh/authorized_keys` 为空就不启动；主机密钥缺失先 `ssh-keygen -A`；端口 2222 且 `-o UsePAM=no`
  （Apple 的 `ssh.plist` 在 Remote Login 关着时 `Bootstrap failed: 5` 起不来，22 端口会死在 `pam_restriction`，见 `results-2026-11-28-usb-net-ssh-and-the-arg-budget.md`）。
- 本机验证（ul11 第二次启动，`logs\ul11-restart-1.log`）：控制台出现 `piano-sshd: exec /usr/sbin/sshd -D -p 2222 …`，
  `ps` 里监听进程 boot+41 s 就位；这一次 PC 侧 `ssh -p 2222 root@…` 直接可用，不再需要 COM6。`/var/log/piano-sshd.log` 记它自己的行。

## USB 网口有了持久地址：不等 Wi-Fi 也能进平板

- 在平板上一次 `networksetup -setmanual "Ethernet Adapter (en1)" 169.254.80.2 255.255.0.0`（服务本来就存在，en1 是 MAC
  `02:50:49:41:4e:02` 的 gadget），设置落在 Data 上的 SystemConfiguration 里，configd 自己会再贴上去。
- PC 侧不用改：`以太网 4`（MAC `02-50-49-41-4E-03`，"UniXNU piano network"）拿到的 APIPA 是 `169.254.142.124/16`，与 `169.254.80.2/16` 同段。
  本机验证：`ssh -p 2222 root@169.254.80.2` 通，`USB-SSH-OK`。这样 Wi-Fi 关联要多久都不影响装机关怀。

## 两轮重启的验收证据（都按轮记账）

| 轮 | 控制台 | 复位方式 | Data store | 复位判定 | 写在前面的标记 |
|---|---|---|---|---|---|
| 1 | `logs\ul11-restart-1.log` | SSH 里 `shutdown -r +1`（正常重启；`shutdown: can't detach from console` 只是它不能从 ssh 会话脱身，计划仍然生效） | `state 2 resumed … seed b2583ea5` | `armed: restart reason bootloader until healthy` → `exec of launchd at 2 s` → `healthy after 180 s, restart reason normal` | `ul11-cold1.txt` `0480425f279a5066c014f09f31e3d370`、`ul11-cold2.txt` `4b90aae32365f01ead873f6f4284bfe2` 读回不变；`ul10-cold-1.txt` `6c71c71d…`、`cold1.txt` `0d50e3f8…` 也不变 |
| 2 | `logs\ul11-hardreset-1.log` | COM5 控制台 `reset` | `state 2 resumed … seed b2583ea5` | 同上（`healthy after 180 s, restart reason normal`） | 写于复位之前的 `ul11-hard.txt` `ef00c0de656a58302a26add9695a78a7`（`PIANO-UL11-HARDRESET-20261128T035800Z`）读回不变 |

COM5 那句 `reset` 不是掉电意义上的硬复位：控制台里能看到 `ASP: System is shutting down`、PianoWifi 的 `the ladder's end`、
PianoPower 的 `shutdown handover done`，也就是说它走的是内核协同关机。掉电/长按那一档仍然只能由人来做，目前无人值守能做到的最硬
一档就是这条；报告里按"控制台发起的重启"记账，不冒充断电。

## 时钟：每次启动都落在同一个常数上，改时间不保留，改时区保留

- 两轮启动的 `kern.boottime` 都是 `sec = 1795585949`（`ESP` 里 `droidvm-utc=1795478400` 加上一段固定偏移），比真实 UTC 晚约 2 天 22 小时；
  用 `date -u` 改成真实时间后，下一次启动又回到同一个常数 —— RTC 没有留住时间。
- 时区改了以后留着（`/etc/localtime` 在 Data 上）：`ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime` 之后 `date` 显示 CST。
  这一条要在验收口径里记着：镜像里没有可用的对时路径，装完系统的头一次联网之前，日期是错的。

## 这两轮里没做到的

- 802.1X 客户晚到没修，Wi-Fi 首次关联仍要 5–8 分钟（三次启动分别 +285.5 s、+285.5 s、+476.7 s）。
- COM6 用户管道这两轮都没有起来（`piano-console.ps1 -Port COM5` 的监视端口正常，`/dev/pianousb` 那条管道发命令没有任何回应），
  现在靠 `piano-sshd` + USB 网口绕过，但没修。
- `piano-still-wallpaper` 装着，`WallpaperAerialsExtension` 仍在跑；VTDecoderXPCService 也一直在解码（上一次播放测试的后遗症）。
- `Load Avg` 600–770、`0.0% idle` 的根因没查。
- 出网仍然不可能：路由器对平板的 DNS 回 `REFUSED`，`223.5.5.5` 从平板、Mac、PC 都不通，PC 上 `127.0.0.1:10808` 对任何 CONNECT 都只回
  `200 Connection established` 然后一字节不转；把 PC 上所有 loopback 监听端口都试了一遍，能"200"的其实都是本机自带的 web 服务
  （拿假域名一试就露馅），不是代理。所以 `git push` 继续失败，原文：`ssh: Could not resolve hostname github.com: Name or service not known`。