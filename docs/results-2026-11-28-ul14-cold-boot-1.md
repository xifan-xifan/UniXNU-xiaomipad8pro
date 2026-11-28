# 2026-11-28 ul14 装机集第 1 发冷启动：三行判据齐全、九个标记读回不变；时钟那格按 §6 修正另记

装机集：`boot_a` `df6d5a18f27054e6…`（未重写），日常 ESP ul14 `252d25696db55071b07a…`，Data store `linux_data` LBA 117735999
seed `b2583ea517732505` generation 4。复位方式：macOS 侧 `/sbin/shutdown -r +1`（19:00:22 到点，属于正常重启那一类，不是断电硬复位）。
控制台抓取 `logs\usb-console\ul14-cold1-com.log`（`capture-com.ps1` 19:00:19 起，900 秒；注意开头有上一次会话的重放，见文末口径）。

## 判据（都取自这一发自己的日志）

| 项 | 日志里的原文 | 行号 |
| --- | --- | --- |
| 记账开始 | `piano-boot-guard: armed: restart reason bootloader until healthy, Gunyah watchdog armed (32 s bite) …` | 5740 |
| Data store | `NativeRamBlock: Data store state 2 (… 2 resumed …) … seed b2583ea5, store seed b2583ea5, flags 2, generation 4, present 867337` | 5898 |
| 里程碑 | `piano-boot-guard: exec of launchd at 2 s (the milestone)` | 7061 |
| 健康判定 | `piano-boot-guard: healthy after 180 s, restart reason normal` + `health at 180 s (decided at 180 s): exec of launchd at 2 s; watchdog on (32 s bite) … 18 pings, longest gap 10.0 s` | 13790、13791 |

`present` 867337 比早上归档的 517341 只增不减，store 在真记账。开机时刻用 RTC 行定：`PianoPower: [0s] rtc: 19347060 (2026-11-28 11:01:18 UTC, offset 1776516618 from piano-rtc-offset)` → 19:01:18 本地；
`healthy` 落在 19:04:18。

## 读回（boot+12 min，root SSH）

`date` `2026-11-25 14:04:44`、`uptime` `up 12 mins`，九个标记 MD5 全部与写入时一致：

```
ul11-rt1.txt      a3f8428da2559d1e5a8bb76d6f9f745e
ul11-rt2.txt      843081c6cfb64ce2a59b7f7635f24d81
ul11-hard.txt     ef00c0de656a58302a26add9695a78a7
ul11-cold1.txt    0480425f279a5066c014f09f31e3d370
ul11-cold2.txt    4b90aae32365f01ead873f6f4284bfe2
ul11-cold3.txt    80e33cf18d7109fea64ba766dc9b3405
ul12-c1.txt       fe817565f59968e70501cd642eac001a
ul14-cold1.txt    dec18f5fff8585263bbda492bac274b2   (本轮之前写的 PIANO-UL14-COLD1-20261128)
ul12-dirty2.log   dcf0b77723c5e2e97ec167fb496322b2
```

## 这发暴露的两件事（不在验收判据里，但影响判读）

1. **内核历元没走 RTC**：`kern.boottime` = `1795585949`（Nov 25 13:52:29），boot+13 min `date` 仍是 Nov 25 14:05。
   分析、作废的两条推断和下一步（把 `piano-rtc` 那对放进命令行再看 60 秒内的 `kern.boottime`）写在
   `docs/results-2026-11-28-rtc-offset-on-the-installed-esp.md` §6。这轮 Wi-Fi 一直在
   `deauthentication, reason 15 (the four-way handshake timed out)`，没网就没 NTP 兜底。
2. **重启后第一次 SSH 前要先 `arp -d 169.254.80.2`**：Windows 留着旧 MAC 的 ARP 项时表现为 TCP `Connection timed out`；
   清掉之后 ping 21 ms、TCP 通。另外设备上 `sysctl -a` 会挂住，只查具体 oid。

## 日志口径（免得对不上）

`capture-com.ps1` 会同时开 COM5/6/7，而且内核控制台在**新会话接上时会重放缓冲**（`session 2: the log resumes where the last
session stopped (4659085 bytes; a few lines may repeat)`），所以本文件里 `armed:`、`exec of launchd` 这类行会出现两次；
判据取第二次出现的那一组（行号 12270 之后），它是 19:01 这一发自己的。开头的 `[1440s]…[1620s]` 那批 `PianoPower` 行是
18:33 那一发的尾巴重放，别按成本轮读数。