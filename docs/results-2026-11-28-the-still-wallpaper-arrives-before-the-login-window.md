# 2026-11-28 壁纸这条启动路径改完了；顺手量到的负载结论：软件解码的视频壁纸是一大块，但不是全部

装机集不变：`boot_a` macOS `df6d5a18…` / Android `7c444a2d…`，ESP `44de650a…`，Data store seed `b2583ea5`。

## 一次无人值守重启 + 计时：root shell 还是 boot+331 s 才有

- 13:11:19 从 SSH 下 `shutdown -r now`，`logs\v3-boot-test.txt`：监听 26.8 s 后消失，**331.1 s 才回来**；`logs\ul11-v3-boot.log` 里
  `piano-boot-guard: healthy after 180 s`。`/etc/ssh/ssh_host_*` 的 mtime 又落在 boot+~309 s（`kern.boottime` 每次都是同一常数
  `1795585949`，所以设备上的 `date` 减这个常数就是真实开机秒数 —— 这条以后当刻度用）。
- 也就是说 ed25519 那条快路径本身没把时间拉下来：`sshd` 监听晚是**结果**不是原因，整机用户态被拖到几十倍慢，launchd 起一个作业都要几分钟。
  于是这轮把矛头指向负载本身。

## 壁纸：`piano-still-wallpaper` 每次启动都卡在 sips 上，所以苹果默认的视频壁纸一直在放

- 证据：`/private/var/db/Wallpapers/.piano-still-wallpaper.256.png`（11 MB 临时文件）留在那儿没被 trap 删掉，而且 `sips` 还在跑 ——
  这一次启动的转换过了几分钟都没结束。转换没结束 → 登录窗口读到的是空选择 → 放默认 aerial 壁纸（4096x2160 软解）。
- `ps` 实测占用（boot+6 min）：`VTDecoderXPCService 39.0 %`、`WindowServer 24.5 %`、`ReportCrashServi 11.1 %`、
  `WallpaperAerialsExtension`、`sips`、`duetexpertd`、`assessmentagent` 跟着跑；磁盘 6 分钟读 3.5 GB。
- 改法（`tooling/k27/still-wallpaper.sh`）：图片一律取"已经存在的文件" —— 有缓存用缓存，没缓存就直接用打包进去的单色 `still.png`，
  把 `DefaultDesktop.heic` 的转换丢到后台、成功才缓存到 `/private/var/piano/wallpaper-stock.png` 给以后启动用；
  顺手清掉以前留下的 `.piano-*` 临时文件。
- 验证：`tooling/k27/test-still-wallpaper.py` 按新契约重写（先落图片、后台转换并缓存、缓存命中不再调 sips、失败不缓存、临时文件清理），
  **在 mac-lan 上 `python3 test-still-wallpaper.py` 出 PASS**；这个测试要 `plutil`，在 WSL 里跑必然假红（改动之前就是红的）。
- 设备上部署后手跑一次：63 s 内正常返回（以前不会返回），打出 `staged still picture now…` 和 root / _securityagent 的 `replaced ours`。

## 还没解决的那一半：账号自己的选择是苹果默认，我们的规则"不动用户的选择"就守不住

- uid 501（`piano`）那行报的是 `login picture kept the user's, store kept the user's`：`~/Library/Application Support/com.apple.wallpaper/Store/Index.plist`
  里是系统自己写的默认选择，我们按规矩不覆盖，于是 aerial 照放。
- 把我们的 Index.plist 强行装进 store 并 `killall WallpaperAerialsExtension VTDecoderXPCService` + `kickstart gui/501/com.apple.wallpaper.agent` 之后，
  store 里内容确实变成我们的（`imageFile` 指向 `DefaultDesktop.heic`），aerial 扩展和解码进程也没有再起；备份在设备 `/tmp/piano-index-backup.plist`。
- 但整机 `0.0 % idle` 并没有因此消失（杀完之后 `CPU usage: 43 % user / 56 % sys / 0.0 % idle`，`Load Avg` 从 772 掉到 402）。
  所以软件解码壁纸是一大块（约 40 %），底下还有别的东西把 CPU 吃满，下一步用 `spindump` 看内核里谁在转。
- 策略待定：默认那档 aerial 要不要在开机时替换成静态图（这台机器的定位是"不软解视频"，但守规矩就不能碰账号选过的东西）。
  现在的倾向是只认"系统出厂默认那一档"，用户换过的仍然一字不动，并把这条写进 unit 的 about。

## 记账

- 实机新证据：一轮 `shutdown -r`（Data store `state 2 (resumed)`、`healthy after 180 s` 照旧，见 `logs\ul11-v3-boot.log`）。
- 提交：`display: put the still wallpaper in place before the login window reads it`（脚本 + 主机测试）。
- 出网仍未解，`git push` 原文：`ssh: Could not resolve hostname github.com: Name or service not known`。