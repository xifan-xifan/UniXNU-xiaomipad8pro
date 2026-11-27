# Results — 2026-11-27（本机）带垫片根第一次跑满整个窗口：`Early boot complete` + `loginwindow`，WindowServer 死在 framebuffer 像素格式

## 哪一发、怎么判读

镜像 `b63ee621b5c3c024…`（`fastboot boot` 的 RAM 判读，不落盘），目录 `logs/native-runs/ram-k273-sm-20261127-144201/`。
14:42:22 起跳，447 s 后 ABL 回来 —— 镜像自带的 `piano-reset-after=420` 准点生效，也就是内核活着跑完了整个窗口，没有被人提前 panic 复位；
reader 一次取回 7,340,032 B pstore 信箱（非零 4,187,731 B），`tooling/native/xnu-log.py decode` 解出 4,565 行；
14:51:15 HyperOS 自己起来（`adb` 见 `7ccdd855 device`）。全程没人按键。

## 这一发第一次做到的事（同一颗 seed `8e1a87e1…`、同一份已写进盘里的 `9e4ef505…` 根）

- 整轮**一次内核 panic 都没有**：前一发的 `panic(watchdogd (pid 182) exited …)` 消失了（`watchdogd`、`mobileactivationd`、
  `xprotectd`、`PerfPowerServices` 四个标签灌进 seed 的 launchd override 库）。
- 四个启动任务全部完成：`mount-phase-1` 21:53:14.355、`data-protection` .805、`mount-phase-2` 15.128、`init-with-data-volume` .225。
- 九条 firmlink 全 `: 0`，只有 `/System/Volumes/Hardware` 是 `: 2`（本机没这个卷，正常）。
- Data 续用盘上已有的 store，没有新建：`data-store-lba=117735999: window at LBA 117735999, store bound to it`、
  `Data store state 7 (1 blank, 2 resumed, 3 fresh, 4 failed, 7 kept), header 3, first LBA 117735999, 6291456 pages`。
- `NativeGopDisplay: started, display 3200x2136 at 60 Hz, 239x163 mm`（L187），L1075 `display on`。
- L1455 / L1599 `Early boot complete. Continuing system boot.`（21:53:41），`loginwindow` 真的跑起来（pid 102，随后 297…）。
- 这发的 seed 里灌了 GPU 替身包，WindowServer 起得来并拿到设备：`NativeSoftMetal … abort reporter installed`、
  `64 selectors stubbed, 0 left to forwarding`、`device made, registry ID 0x100000157, port 0, no queue: software compositing`。

## 新的墙：软件合成器的 framebuffer 像素格式

420 s 里 `exit: pid N WindowServer status` 共 **15 次**（只有 pid 113 是 `status 0`，其余全是 `0x6` 即 SIGABRT），每次都停在同一处：

```text
crash message (SkyLight): Unimplemented pixel format of 641877825 used in WSCompositeDestinationCr
crash message (SkyLight): eateWithIOSurface.
#3 libsystem_c.dylib+0x7b4ac abort
#4 SkyLight+0x409e0 WSCompositeDestinationCreateWithIOSurfac
#5 SkyLight+0x422fb8 WS::Displays::SLCADisplay::render_upd…
```

这正是 `docs/drivers/soft-metal.md` §8 那条路径，但**报出来的数字对不上那张表**：`641877825 = 0x26424741`（`'&BGA'`），
既不是它只认的八个（`'BGRA' 1111970369`、`'RGBA' 1380401729`、`'RGfA' 1380410945`、`'R10k' 1378955371`、`'b64a' 1647719521`、
`'l10r' 1815162994`），也不是会 abort 的三个（`'RGhA' 1380411457`、`'b3a8' 1647534392`、`'w40a' 1999908961`）。
这颗信箱本身有字符衰减的旁证（同一份日志里 `dyld+0x#1f90`，应为 `0x31f90`），所以**这个数字必须先从原始转储的多副本里定死，再谈改代码**。

CA 调试选项侧的读数：只有 `CA option 206 CA_HDR10_SET_EDR_SCALE is on`，`CAEnableDeepFramebuffer set false for this process`，
并且**没有**任何 `CA option … cleared` 行 —— §8 里那两个强开候选（124 `CA_FORCE_FP16_FRAMEBUFFER`、125 `CA_FORCE_CIF10_FRAMEBUFFER`）
本来就是关的。按 §8 的判读表，这落进"格式不是选项强开决定的"那一格，也就是 EDR／能力位那条分支，能动的地方是
NativeGopDisplay 对外发布的显示属性（它现在发的是 8-bit SDR sRGB、无亮度、无 EDR、`SupportsPQEOTF` false）。

## 还没做到的

没有出现 `first swap submitted` / `first frame`，屏上仍是启动控制台 —— 这轮不算进桌面，验收一条都没过。

## 下一步（按顺序，都在 RAM 判读上跑，不落盘）

1. 从 `mailbox-9cb00000-700000.bin` 的原始副本把 `Unimplemented pixel format of N` 的 N 定死（逐副本表决，别信单次解码），
   必要时让 abort reporter 的那句话走 os_log 或串行口再取一次。
2. N 定死之后再选杠杆：若是表里那三种浮点／10-bit 格式，就查 NativeGopDisplay 里让 `edr_headroom()` 或能力位为非零的那处；
   若是别的数字，回到 §5 的表。