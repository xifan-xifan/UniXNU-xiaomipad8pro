# 2026-11-28 ul11：第三轮复位、第二轮往返，以及"开机要过几分钟才有 root shell"的真正原因

装机集不变：`boot_a` macOS `df6d5a18…`（`out\persistent\boot-piano-macos-persistent-v6-sealed.img`，本机 SHA-256 复核一致）/
Android `7c444a2d…`（`E:\piano-backup\persistent-boot-20261128-010938-fe5vsrzx\boot_a.img`），ESP `esp-7ccdd855-k273-ul11.img` `44de650a…`。

## 第三轮复位（控制台 `reset`）与第二轮往返都过了

- 第三轮复位：`logs\ul11-restart-2.log`（12:32:08 起抓），`piano-boot-args … 1124 bytes`、`Data store state 2 (resumed) … seed b2583ea5`、
  `exec of launchd at 2 s`、`healthy after 180 s, restart reason normal`；这一轮之后读回，`ul11-hard.txt ef00c0de…`、
  `ul11-cold1.txt 0480425f…`、`ul11-cold2.txt 4b90aae3…` 都不变。
- 第二轮往返：`logs\roundtrip-ul11-to-android-2.txt`、`logs\roundtrip-ul11-back-2.txt`。去程前槽位是 `successful:no / unbootable:no / retry-count 5`
  （这一位确实会被 macOS 侧的启动消耗，之前"短期不降"的观察只适用于那几发），刷 Android 后回到 `no/no/7`；
  HyperOS 再次起来（`OS3.0.309.0.WPYCNXM`、`sys.boot_completed=1`、焦点窗口 `com.android.systemui`）；回程 `adb reboot bootloader` →
  刷回 macOS `boot_a` → `healthy after 180 s, restart reason normal`。
- 往返前后的读回：`ul11-rt2.txt 843081c6cfb64ce2a59b7f7635f24d81`（去程前写的）与 `ul11-rt1.txt a3f8428d…`、`ul11-hard.txt ef00c0de…`、
  `ul11-cold1.txt 0480425f…`、`ul11-cold2.txt 4b90aae3…`、`ul10-cold-1.txt 6c71c71d…` 一起读回，全部逐字节一致。两轮往返成立。

## 开机之后要等 4–8 分钟才有 root shell：卡在主机密钥，不是网络也不是 sshd 配置

- 现象：`Test-NetConnection -Port 2222` 早就是 `True`（监听在），但第一次能用 `ssh` 往往是开机后好几分钟；控制台里 `sshd` 反复起十几次，
  还夹着 Apple 的 `sshd-keygen-wrap`。
- 实机证据（12:47 那一轮）：`/etc/ssh/ssh_host_*` 的时间戳是 `Nov 25 13:57`，也就是 boot+~280 s —— 主机密钥是那次启动现做的；
  里面还有 `ssh_host_mldsa44_ed25519_key`（macOS 27 的 `ssh-keygen -A` 会连后量子密钥一起生成，在这台的随机源上要跑很久）。
  密钥没做完，`sshd` 就没法监听，launchd 每 10 s 重试，于是那十几条 `sshd` 行。
- 改法（已装到设备并同步进仓库）：`piano-sshd` 只等 `ed25519` 这一把快密钥（缺了就 `-t ed25519` 现做，秒级），把它用 `-h` 显式交给 sshd；
  整套 `ssh-keygen -A` 丢到后台补，不再挡监听。同时保留"等 `/var/root/.ssh/authorized_keys` 最多 240 s"。
  UbuntuPiano 里用桩跑过：有密钥时直接过、没密钥时先生成再报，`sh -n` 过；设备上 `PIANO_SSHD_NOW=1` 报
  `key present, sshd present, host key /etc/ssh/ssh_host_ed25519_key present`。
- 顺带记两条坑（都是今天踩的）：`scp` 的端口是大写 `-P`、`ssh` 是小写 `-p`，用 `C:\Windows\System32\OpenSSH\ssh.exe` 还会嫌
  `E:\piano-backup\usb-dev-key\dev2` 的 ACL 太开；`piano-sshd` 写到 `/dev/console` 的行不保证出现（`/private/var/log/piano-sshd.log`
  这次是 0 字节），判"有没有 shell"要看 `launchctl print system/com.unixnu.piano-sshd`（`runs`、`pid`）或 TCP 2222。

## 这台机器现在的负载状态是异常量级，别当成背景噪声

- 连着几轮 `top -l 1` 都是 `309 running`、`2056 threads`、`CPU usage: 46% user, 54% sys, 0.0% idle`、`Load Avg 660+`，
  而 CPU 大头只有 kernel_task、VTDecoderXPCService、WindowServer；`NativeGopDisplay: swap_wait(...) = 0xe00002d5 (69000 failures)`
  和 `at most 6 frames in a second` 是同一片土壤上的症状（帧率那条见 `results-2026-11-28-swap-wait-rate-and-fps.md`）。
- 抓到的具体异常：`watchdogd` 在开机 ~150 s 处崩溃循环（`Process[348] crashed: watchdogd. Too many corpses being created.`，
  七次 `AMFI: Denying core dump`）；`cryptexd` 也有 `.ips`；`/Library/Logs/DiagnosticReports` 里躺着 1.4–1.8 MB 的
  `shutdown_stall_*.shutdownStall` —— 我们每次 `shutdown`/`reset` 都被记成"关机卡住"。这几条都还没查。
- 触控也有量在掉：`PianoTouch: frames at 566 s: 17 reads/s … checksum ok 4971, bad 4462`（近半读取校验不过），
  这解释了为什么手指动的时候更卡，得单独查。

## 现在的验收状态（老实记账）

- 已经实机过：可诊断启动/桌面/视频（30 s 1080p 零重启）/Wi-Fi 关联+DHCP+局域网双向/音频推到 DSP（能不能听见要人耳）/GPU 解除限频/
  三轮复位 + 两轮 macOS↔Android 往返 + 每次的 Data 读回。
- 没通过或没做：首次 Wi-Fi 关联要 5–8 分钟（客户端晚到，见 `results-2026-11-28-ul11-two-restarts-a-boot-time-sshd-and-the-late-client.md`）；
  出网/DNS 至今无法验证（整个局域网 WAN 断）；`watchdogd` 崩溃循环、关机 stall、`0.0% idle`、触控校验；掉电式硬复位仍需人做一次长按；
  严格意义的"断电冷启动"三轮同样受此限制（现在做的是 `shutdown -r` 与控制台 `reset` 两种复位）。