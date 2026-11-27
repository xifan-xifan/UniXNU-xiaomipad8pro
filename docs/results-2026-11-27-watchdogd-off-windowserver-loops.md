# 2026-11-27 - watchdogd 关掉以后：mount-phase-1/2 全过、Data store 续上、显示点亮，撞在 WindowServer 与 cryptexd 崩溃循环

镜像 `out/ram-verified-8748-shim2/boot-piano-macos-ram-A-bundled.img`，159,686,656 字节，
SHA-256 `cf4b45901d7e003bf67ef34fab07e1e2d511871e6081cce6d660872c7961aa20`（UEFI 侧 `eb570025…` 未变）。
Mac 构建脚本 `out/build-verified-8748-shim2.sh` = 上一个 shim 脚本加
`panic_on_crash_disabled=1` 与 `PIANO_DENY_EXEC=/usr/libexec/watchdogd,/usr/libexec/PerfPowerServices`，
存储参数仍是 `data-store-part=linux_data data-store-lba=117735999`，**没有** `data-store-replace`。

## 为什么不用 `stage-boot-run.py --disable-job`

`--disable-job` 要改 `private/var/db/com.apple.xpc.launchd/disabled.plist`，而 8748 那颗 seed 里根本没有这个文件，
脚本报"新建的那份不属于 root"就退出。所以改用文档里另一条路（boot-arg + exec 拒绝），在**同一颗 seed、同一个 root** 上试，
不重建 Data、不换 seed。

## 跑的结果

`logs/native-runs/ram-k273-shim2nodog-20261127-124541/`：448 s 后 ABL 回来，mailbox 取回，HyperOS 自己回来了。
`console.txt` 119,236 字节 / 930 行（用修好的 `tooling/native/xnu-log.py` 解出来的）。

watchdogd 那道墙确认是死了：

- 356 起 `panic-on-crash disabled via boot-arg`，没有任何 watchdogd panic；
- 358 `launchd (mount-phase-1) Doing boot task`，380 `launchd (mount-phase-2) Doing boot task`；
- 326..334 七条 firmlink 全部 `: 0`（`/System/Volumes/Hardware` 是 `: 2`，本机没有这个卷，正常）；
- 215/216 `NativeRamBlock: data-store-lba=117735999: window at LBA 117735999, store bound to it`、
  `Data store state 2 (…2 resumed…) … seed 8e1a8…`，也就是续用 `linux_data` 上已有的 store，没有重建；
- 212 `NativeGopDisplay: started, display 3200x2136 at 60 Hz`，530 `NativeGopDisplay: display on`；
- 580/618 `Early boot complete. Continuing system boot.`（21:53:23）；
- 930 `piano-reset-after: 420 s reached, PSCI SYSTEM_RESET` —— 自复位仍然准点。

## 新的墙：WindowServer 崩溃循环 + cryptexd 崩溃循环

- WindowServer：pid 131、221、265、362、388、418、453、473、491、508、528 一路 `Process[N] crashed: WindowServer. Too many corpses being created.`
  它确实连上了显示（526 起反复 `IOMobileFramebufferUserClient::s_map_display_type … display type is 1`），然后就死；
  这颗镜像没有 Metal 设备（`PIANO_GPU=0`，没有 NativeSoftGpu/NativeSoftMetal），和 `docs/drivers/soft-metal.md` 描述的一致。
- cryptexd：同样一路 `crashed: cryptexd`， accompanying `Failed to handle the new encryption key. Error 0xe00002d8`、
  `apfs_keybag_init:2877: failed to initialize volume keybag, err = 2`、
  `AppleImage4: caller not entitled: p = kmutil.319, entitlement = com.apple.private.security.AppleImage4.user-client`；
  内核更早还有 166 `IONVRAMCHRPHandler.cpp:sync:928 - serializeVariables failed, ret=0xe00002d8`。
- 没有 loginwindow / Finder / Dock，所以这一轮不算进桌面。

## 判读工具本身的问题（已修）

这颗 mailbox 用 HEAD 的 `xnu-log.py` 解是垃圾：多数表决阈值从 6/11 掉到 3/11，`capacity 667252`，正文全是 `}`/`o`/`w`。
真正的原因是**步长**：转储是 0x700000，镜像是 `pstore-console-size` 减掉保留的 reset-reason 块（PRR_BYTES = 0x10000），
所以步长是 `size - 0x10000 - base` = 661294；这一轮 copy 0 打印的那行本身有噪声，正则抓不到，候选里就只剩错的 `size - base`。
加上这个候选后：干净的那颗 mailbox 解出来与 HEAD 逐行相同，这一轮解出 930 行可读日志。

顺手试过"按字节取众数"的表决（以为这一轮是整字节噪声而不是位衰减），**已否决删除**：它丢掉整行
（`initUDIDEnforcement: /product is missing udid-version`）并且把 glyph 改坏（`VM bootstrap`→`VM bnotstrap`、
`slot count`→`slgt count`、`0xfffffff9ef031000`→`0xffffdff9ef031000`），干净 mailbox 上也会把 4 行改错。位表决是对的。
