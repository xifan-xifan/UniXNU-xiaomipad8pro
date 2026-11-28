# 2026-11-28 停掉 PowerLog 之后重启一次：它确实没再起来，但开机那几分钟还是满的

装机集不变（`boot_a` `df6d5a18…`、ESP `44de650a…`、Data store seed `b2583ea5`）。这一篇给 `results-2026-11-28-powerlog-holds-the-machine.md`
补上"重启之后"的那一半，把那篇里没复测的因果收口。

## 重启计时（13:45:48 从 SSH 下 `shutdown -r now`，`logs\ppsoff-boot-test.txt`）

- 监听 19.6 s 后消失，**289.0 s 才回来**（上一次同口径 331.1 s）。`logs\ppsoff-boot.log`：`healthy after 180 s` 照旧，`Data store state 2 (resumed)`。
- `com.apple.PerfPowerServices` 这次启动**没有作业**（`launchctl print` 无 state/runs），`ps` 里只剩那个 0 % 的注册 XPC ——
  开机作业 `com.unixnu.piano-powerlog-policy` 起了 1 次，`launchctl disable` 的 override 也确实跨了一次启动。
  也就是说：停用是持久的，但 root shell 并没有因此提前几分钟。
- 一个之前读错的刻度要更正：`/etc/ssh/ssh_host_*` 的 mtime 三次启动都读 `13:57:38`，看着像"每次开机现做到 boot+309 s"，
  其实 `kern.boottime` 每次都是同一常数 `1795585949`（设备时钟每次从同一个值起算），**同样的 mtime 恰恰说明主机密钥是留在盘上的、没重做**。
  所以 sshd 晚不是密钥生成的锅（那把 ed25519 本来就在），而是作业本身起得晚 / 起得早但排不上队。

## 同一台机器 boot+271 s 的读数（PPS 已停）

- `CPU usage: 43.70 % user, 56.29 % sys, 0.0 % idle`，`Load Avg 399 / 230 / 100` —— **开机风暴这一段跟停没停 PPS 无关**。
- 上一篇里 58 % idle 那组数字是 boot+1530 s（开机风暴过去之后）测的，把两件事记成一件是我先前收得太快。
  现在的读法：PPS 是"开机后一直按住不放"的那个持有者（它被拉起后稳定吃 45–68 %），而 boot+0…300 s 这段另有来源，
  大概率是 `watchdogd` 崩溃循环 + `ReportCrash`/`coresymbolicationd`/`sips` 这一串开机代办挤在一起。这条单独立案，下一轮拿 boot 窗口的分段时间线来定。

## 顺手修的一条可调试性

`piano-sshd` 的 `say()` 第二条 `printf` 走的是 stdout，launchd 的 `StandardErrorPath` 收不到，日志文件才会是 0 字节；
改成 `>&2`（`tooling/native/piano-sshd/piano-sshd.sh`）。`sh -n` 过，设备上 `PIANO_SSHD_NOW=1` 报
`key present, sshd present, host key … present`。下一次启动就能从 `/private/var/log/piano-sshd.log` 里读它自己的时间线，
不用再靠控制台猜。

## 记账

- 实机新增：又一轮 `shutdown -r` 重启 + Data 读回（`state 2 (resumed)`），停机作业跨启动有效。
- 标记读回的位置记一下（以前没写进文档）：`/Users/piano/Desktop/`。这次启动七个文件 MD5 与记录逐字节一致 ——
  `ul11-rt1.txt a3f8428d…`、`ul11-rt2.txt 843081c6…`、`ul11-hard.txt ef00c0de…`、`ul11-cold1.txt 0480425f…`、
  `ul11-cold2.txt 4b90aae3…`、`ul10-cold-1.txt 6c71c71d…`、`cold1.txt 0d50e3f8…`。
- 出网仍未解，`git push` 原文：`ssh: Could not resolve hostname github.com: Name or service not known`。