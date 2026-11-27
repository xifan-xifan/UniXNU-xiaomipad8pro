# 2026-11-28 - 正常重启这一关在本机过了：`shutdown -r +1` 真的复位、回到 macOS、复位原因 normal，Data 两个文件重启后逐字读回

装机集：`boot_a` = `boot-piano-macos-persistent-v6-sealed.img`（`df6d5a18f27054e6...`，DarwinBoot 绑定 13312 MiB），
ESP = `out/persistent/esp-7ccdd855-k273-ul3.img`（sha256 `28592e9d94abe57e78ed999fc4c07f214f8b1624a2835f20050d6fdef3b6723a`），
会话 `out\persistent\session-k273-ul1-20261128`，日志 `logs\session-flash-ul3-20261128-050915.log`。
启动行关键项：`data-persist=2 data-store-part=linux_data data-store-lba=117735999 piano-boot-guard=180 piano-usb-shell=1 piano-usb-hid=1`，
**没有** `piano-reset-to`，**没有** `data-store-replace`（这两条正是上一轮把正常重启带回 ABL、把 Data 打成易失的元凶，见下）。

## 判据

- 冷启动（`logs\usb-console\ul3-cold1-com.log`）：`NativeRamBlock: Data store state 2 (resumed) ... seed b2583ea5, generation 4, present 474542`，
  随后 `piano-boot-guard: healthy after 180 s, restart reason normal`。
  对比 `docs/boot-guard.md:70,105`：带 `piano-reset-to=bootloader` 且没有 `piano-reset-after` 时，健康检查会把复位原因永远留在 `bootloader`，
  于是每次重启都落回 ABL fastboot —— ul1 那一版就是这样（`healthy after 180 s, restart reason stays bootloader (piano-reset-to)`）。ul3 去掉这条之后判据翻正。
- 正常重启（`logs\usb-console\ul3-normal-reboot-com.log`、发起记录 `logs\usb-console\ul3-normal-reboot-sd.txt`）：
  `05:42:57` 通过 COM6 root shell 发 `/sbin/shutdown -r +1 unattended-reboot-test`，
  `shutdown: [pid 3717]`、`SD_EXIT=0`（`shutdown: can't detach from console` 是无控制台时的正常噪音，不是失败）；
  控制台 `05:44:32 CLOSED COM5/6/7` → `05:44:59 OPEN COM5/6/7`，即真的复位了，而且回的是 macOS（不是 ABL）；
  新一轮又是 `piano-psci-halt: on` + `exec of launchd at 1 s` + `healthy after 180 s, restart reason normal`。
  这一条同时结掉 ul2 那轮的疑点：`shutdown -r now` 在 ~170 s 内没有任何复位动作（PianoPower 的 uptime 不动、pstore 里没有 halt 行），
  原因仍未查明；但 `+1` 这条延时路径是可用路径，验收按它记。
- 重启读回：重启前 Desktop 上只有 ul2 那一轮写的 `persist-normal.txt`（`PIANO-UL2-NORMAL-20261128`），
  重启后 `ls`/`md5`/`cat` 两个文件都在且内容逐字一致：
  `persist-normal.txt` = `PIANO-UL2-NORMAL-20261128`（MD5 `b88c264588d85dc3f76957506b07ccdb`），
  `persist-normal2.txt` = `PIANO-UL3-NORMAL-20261128`（MD5 `8bd0e7a8811c747617ce8a54441a81fa`）。
  所以**正常重启的 Data 读回**成立（硬复位那条读回早先已经成立），两条恢复路径分开记。
  核对时 `uptime` 显示 `up 5 mins`，与这次复位的时间吻合，能排除"读的是重启前的旧会话"。

## 本机读法（无人值守，别再去按按键）

- COM5 内核监视器、COM6 root shell（`uid=0(root)`）、COM7 HID。`tooling\native\piano-shell.ps1` 发一条命令读回执；
  `tooling\native\capture-com.ps1` 会占住**所有** COM 口，用 shell 之前必须先停掉它。
- 进 ABL：`piano-console.ps1 -Port COM5 -Line 'reset fastboot'`（约 10 s）；回正常启动：`-Line 'reset'`。
- 机器时钟被 `droidvm-utc=1795478400` 带偏（显示 2026-11-24 晚 10 点前后），打标记一律用 `uptime`，别用 `date`。

## 还没做完的验收

两轮 Android↔macOS 往返（含 `slot-successful`/`slot-retry-count` 前后值）、单帧与连续视频、Wi-Fi、音频、亮度、电池、GPU 池
（`BoAlloc refused 0xe00002bd`；`NativeGopDisplay: swap_wait ... 0xe00002d5` 累计 23.1 万次失败、GPU 硬顶 160 MHz）。
冷启动原始日志已有 ul1-cold1、ul2-cold1、ul2-cold2、ul3-cold1，尚未按验收口径重命名归档。

