# 2026-11-28 ul11 的 macOS→Android→macOS 往返：全程无人值守，往返前后 Data 里的标记逐字节不变

装机集不变：`boot_a` macOS `df6d5a18…` ↔ Android `7c444a2d…`（`E:\piano-backup\persistent-boot-20261128-010938-fe5vsrzx\boot_a.img`，
100663296 字节，SHA-256 已核），ESP 一直是 `esp-7ccdd855-k273-ul11.img` `44de650a…` 未动。脚本 `tmp-rt-mac-to-android.ps1`（去）和
`tmp-rt-back.ps1`（回），日志 `logs\roundtrip-ul11-to-android.txt`、`logs\roundtrip-ul11-back.txt`。

## 去程（macOS → Android）

- 从 macOS 里不碰按键：COM5 监视口 `reset fastboot` → 14 s 后 `fastboot devices` 看到 `7ccdd855 fastboot`。
- 刷之前槽位 `slot-successful:a yes / unbootable no / retry-count 6`；刷完 `no / no / 7`（Android 的镜像把重试预算又填回 7，和之前记的一致）。
- `--set-active=a` + `reboot` → 300 s 窗口里 adb 认出设备：`ro.build.version.incremental = OS3.0.309.0.WPYCNXM`、
  `ro.product.model = 25091RP04C`、`ro.build.version.release_or_codename = 16`、`ro.miui.ui.version.name = V816`、
  `ro.boot.verifiedbootstate = orange`，随后 `sys.boot_completed = 1`、`settings get global device_name = Xiaomi Pad 8 Pro`、
  焦点窗口 `NotificationShade`。HyperOS 3.0.309 原样在。

## 回程（Android → macOS）

- `adb reboot bootloader` → 进 ABL 时读到 `slot-successful:a yes`（这一位只有 Android 侧会写回，这里正好证明是 Android 起来后写的）。
- 刷回 macOS `boot_a`（发送+写入 2.7 s），槽位变 `no / no / 7`，`reboot` → 控制台 COM5/COM6/COM7 回来（`logs\ul11-rt-back-boot.log`）。
- 内核侧照旧：`piano-boot-args … boot-args 1124 bytes`、`Data store state 2 (resumed) … seed b2583ea5`、`exec of launchd at 2 s`、
  `healthy after 180 s, restart reason normal`。

## 往返前后的读回（这一条才是"Data 穿过了一次换系统"）

去程之前写 `ul11-rt1.txt`（`PIANO-UL11-RT1-ROUNDTRIP`，MD5 `a3f8428da2559d1e5a8bb76d6f9f745e`），回程之后连同更早的标记一起读：
`ul11-rt1.txt a3f8428d…`、`ul11-hard.txt ef00c0de…`、`ul11-cold1.txt 0480425f…`、`ul11-cold2.txt 4b90aae3…`、
`ul10-cold-1.txt 6c71c71d…`、`cold1.txt 0d50e3f8…` —— 六个 MD5 全部与写时一致。

## 这一轮里补的两个装机件

- `piano-sshd` 在设备上换成会等密钥的版本（`PIANO_SSHD_WAIT`，默认 240 s），plist 的 `StandardErrorPath` 从 `/var/log/…` 改到
  `/private/var/log/piano-sshd.log`（和能正常开机的 `piano-wifi-join` 用同一种路径写法；`/var` 要靠 Data 的 firmlink 才成立）。
  装完 `plutil -lint` OK、`sh -n` OK。
- 时钟再拨一次：这台机器每次启动都落回同一个常数（`kern.boottime sec = 1795585949`），所以 `date -u` 只在本次开机有效；
  时区 `Asia/Shanghai` 是持久的。

## 还没做 / 需要记住

- 这次往返是**第一轮**。目标要求两轮，第二轮走同一套脚本。
- `launchctl print system/com.unixnu.piano-sshd` 在这轮 `runs = 1`、pid 264、etime 说明它在 boot+60 s 就位；但它的控制台行这两轮没出现，
  `/private/var/log/piano-sshd.log` 是 0 字节 —— 也就是说 `/dev/console` 在 boot+60 s 那一刻不一定写得进去，别把"控制台有行"当成判据。
- 从 Windows 用 `C:\Windows\System32\OpenSSH\ssh.exe` 连的时候，`E:\piano-backup\usb-dev-key\dev2` 的 ACL 会被它嫌太开
  （`Bad permissions … Authenticated Users`），要嘛修 ACL 要嘛用 `tmp-ssh.ps1` 里那份 ssh；别把这个当成平板拒绝连接。