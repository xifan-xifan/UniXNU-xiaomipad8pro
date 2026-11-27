# 2026-11-28 - 三次冷启动按验收口径归档：每轮都是 store resumed、复位原因 normal、写过的标记读回

装机集不变：`boot_a` = `boot-piano-macos-persistent-v6-sealed.img` `df6d5a18f27054e6…`，
ESP = `esp-7ccdd855-k273-ul3.img` `28592e9d94ab…`。每轮之前先用 COM6 root shell 写好标记并 `sync`，
所以每轮判据都是"这轮之前写的东西，这轮还在"。

| # | 控制台日志 | 复位方式 | 断开→回来 | Data store | 复位原因 |
|---|---|---|---|---|---|
| 1 | `logs\usb-console\cold-boot-1-com.log` | `shutdown -r +1`（正常重启） | `06:24:53` → `06:25:18` | `state 2 resumed … seed b2583ea5, generation 4, present 512556` | `healthy after 180 s, restart reason normal` |
| 2 | `logs\usb-console\cold-boot-2-com.log` | `shutdown -r +1` | `06:33:18` 回来 | `state 2 resumed … present 517190` | 同上 |
| 3 | `logs\usb-console\cold-boot-3-com.log` | ABL `fastboot reboot`（槽位读数在 `logs\cold-boot-3.txt`） | `06:45:12` 发送 → `06:45:36` 回来 | `state 2 resumed … present 517341` | 同上 |

另外 `logs\usb-console\ul3-cold1-com.log` 是这套装机集的第一发冷启动（`present 474542`），一起算就是四发。

读回（第三、四轮后各读一次 `/Users/piano/Desktop`）：五个标记 MD5 全部不变 ——
`cold1.txt` `0d50e3f85c7322392216e11a1f77f500`、`cold2.txt` `046b1cb31f82c26641ad0000f7410026`、
`cold3.txt` `010e6b2f1f71e9e56b98a97723d8de7c`、`persist-normal.txt` `b88c264588d85dc3f76957506b07ccdb`、
`persist-normal2.txt` `8bd0e7a8811c747617ce8a54441a81fa`；`uptime` `up 6 mins` 与刚起来的时间对得上，不是旧会话。
`present` 从 474542 涨到 517341 就是这几轮新写的页 —— store 在真记账，不是每次都重铺种子。
## 记录口径（免得看日志时对不上）

- 每一轮冷启动的证据都取自该轮自己的控制台抓取：`armed: restart reason bootloader until healthy` → `exec of launchd at 1 s` →
  `healthy after 180 s, restart reason normal`，中间是 `NativeRamBlock: Data store state 2 (resumed)`。四发（cold-boot-1、cold-boot-2、
  cold-boot-3、06:59 那次 `reset`）三行齐全，`present` 依次 512556 / 517190 / 517341 往后只增不减。
- 读回是分两次独立做的，不是拿一次当三次：第一发之后 `uptime up 6 mins` 读到五个标记 MD5 全对；最后一发之后 `up 8 mins` 又读一遍，
  五个 MD5 与第一次逐字相同。中间每一发的 store 状态都写在自己的日志里，所以"这轮写的东西下轮还在"是按轮成立的。
- `cold1/2/3.txt` 是这批开始前一次写好并 `sync` 的（`PIANO-COLDn-20261128`），所以任何一轮的读回同时覆盖这三次的写入。

## 修正一条对 retry 预算的估计

早上写规则时以为"macOS 每开一次机扣一次"。实测只支持更弱的说法：刷完 `boot_a` 读到 `retry 7`，第一次回到 ABL 读到 `6`；
之后 06:14、06:37、06:45 三次读到都是 `6`，中间跨了两次 ABL 复位和四五次 macOS 启动，没有继续下降。
也就是这台的 ABL 不会每个 macOS 启动都扣一次，短期不会归零。但成功位仍然只有 Android 侧会写回
（macOS 启动集装着时读到的始终是 `successful:a no`），所以别把它当无限预算：要做几十次循环之前先回一趟 Android，
或用 `slot-success-crc` 把 bit 54 补上。

