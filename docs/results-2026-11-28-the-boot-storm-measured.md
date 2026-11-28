# 2026-11-28 开机风暴量完了：不是某一个进程，是几百个苹果代办一起上，外加三个永远起不来的在循环重启

装机集不变（`boot_a` `df6d5a18…`、ESP `44de650a…`、Data store seed `b2583ea5`）。承接
`results-2026-11-28-powerlog-holds-the-machine.md` 与 `results-2026-11-28-the-boot-storm-is-not-perfpowerservices.md`。

## 量到了什么（launchd 自己的日志，开机 6 分钟窗口，402934 行）

- **三个代办在无限循环重启**，每次都被 throttle 推迟约 10 s 再试：
  `com.apple.security.cryptexd` 起来 25–130 ms 就带 `OS_REASON_LIBSYSTEM | unexpected failure: Failed to copy grafted system cryptexes:
  [2: No such file or directory]` 退出；`com.apple.security.xprotectd` 34 ms–1.2 s 就 `exit(1)`；`com.apple.watchdogd` SIGTRAP
  （`EXC_BREAKPOINT`，`watchdogd+0xc948`），`Too many corpses being created` + `AMFI: Denying core dump`。
  整窗口里 `throttle` 出现 6144 次、`crashed` 944 次；`created` 最多的名字是 `user` 2812、`fskitd` 1814、`xprotectd` 1785、
  `ReportCrashServi` 571、`loginwindow` 480、`watchdogd` 325。
  cryptexd 的怀疑对象也定位到了：`/System/Cryptexes/{ExclaveOS,Rosetta}` 是指向 `/System/Volumes/Preboot/Cryptexes/…` 的软链，
  而 Preboot 那边只有 `App` 和 `OS -> /`，另两个不存在（`/System/Volumes/Update/mnt1` 也不存在）。
- 但**把它们停掉并不能把开机变快**：`launchctl disable` + `bootout` 掉 xprotectd 和 cryptexd 之后重启，root shell 反而更晚
  （同一支探针同口径 470.4 s；对照：只停 powerlog 那轮 289.0 s、更早 331.1 s、带 5 s 探针那轮 436.4 s）。
  实验做完已把这两个作业 `enable` 回来 —— 它们是安全代办，没有收益就不该动它们。
- **探针自己就是负载**：5 s 一轮 + `top -l 1`（首样本永远 0.0 %CPU）实测每条记录 12 s，把 root shell 推到 436 s。
  已改成 15 s 一轮、`top -l 2` 取第二个样本，并作为 `default_on = false` 的 unit 收进
  `tooling/native/piano-boot-probe/`（日常启动不开）。
- **launchd 排到我们的开机作业要 400 s**：`piano-boot-probe` 与 `piano-sshd` 都是 `RunAtLoad` 的系统作业，
  这三轮分别在 boot+374 s / +404 s / 约 +440 s 才真正被执行（`/private/var/log/piano-sshd.log` 里那行 `exec /usr/sbin/sshd …`
  出现的时刻就是监听起来的时刻）。到 +404 s 时 `top` 的前几名是 `dasd 16.4 %`、`photolibraryd 14.7 %`、`imagent 12.6 %`，
  +460 s 是 `linkd`、`mobileassetd`、`remindd` —— **没有一个大户，是一大片代办同时在跑启动工作**（`0.0 % idle`、load 400–570）。

## 结论与下一步

1. 之前"整机被一个进程按住"只对了一半：PowerLog 是稳态运行期的大户（它一停就有 58 % idle），
   开机那几分钟则是苹果代办群发风暴 + 三个起不来的代办循环重启，两者要分开记账。
2. 真正能改动机的两件事：把没意义的代办从开机集合里裁掉（这是策略决定，要写清楚裁哪些、为什么、怎么还原，并和上游对齐），
   以及让 cryptexd 的假设成立（补 `Preboot/Cryptexes/ExclaveOS`、`Rosetta`）而不是禁掉它 —— 后者还没试，是下一个具体实验。
3. `watchdogd` 的 SIGTRAP 还没定案（`.ips` 里没符号，`asi` 为空），要符号化设备上那份 `/usr/libexec/watchdogd`（Mac 上有 `nm`/`otool`）。
4. 磁盘读带宽没量过：6 分钟启动读 4.3 GB 这个数值不值得怀疑，得用 `dd if=/dev/rdiskN bs=1m count=400` 定一次。

## 顺手记的运维事实

- `piano-sshd` 的 `say()` 修好之后，`/private/var/log/piano-sshd.log` 真的会落一行 `exec /usr/sbin/sshd -D -p 2222 -h …`；
  判"root shell 什么时候能用"以后看这个文件的时间戳，别再猜控制台。
- 出网今天恢复了，`git push` 通到 `origin/main`；平板侧 `dig github.com` 出地址、`ping 223.5.5.5` 0 % 丢包。