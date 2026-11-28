# 2026-11-28 视频收口：avc1 连续播放跑在自研 VideoToolbox 解码器上，VPU 记账，CPU 侧只有十几个百分点

装机集没换：`boot_a` macOS `df6d5a18…`、ESP ul11 日常 `44de650a…`、Data seed `b2583ea5`；本次是冷启动 3 那次开机
（`kern.boottime` = 16:14:53）上做的测量，全程无人值守。

## 用的东西

- 探针 `tooling/native/piano-play-probe.m`（A 部分：`AVAssetReader` 解一帧并把它认出来；B 部分：`AVPlayer` 播 30 s，
  每 2 s 记一次播放钟，统计 stall 与倍率）。
- 在 `mac-lan` 上编：`xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 -fobjc-arc
  -framework AppKit -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework IOSurface`，
  `codesign -s -` 临时签名。产物 57,120 字节 `0b1fd14e92611b9dca74bec669dcf20714fe497661fc2238e3d53397b03196d1`，
  设备 `/tmp/probe2` 与它逐字节相同（`shasum -a 256` 一致；`scp` 在这台 Windows 上会静默不干活，所以走
  `mac-lan:8123` 的 http + 设备 `curl`）。
- 片子 `/tmp/clip.mp4` 1,019,562 字节 `4e7107b03189ad90086302db8b91c086fab4407e286fac97297a179e0ad70001`，
  轨道 `1920x1080 codec=1635148593`（`avc1`），时长 30 s。

## A：一帧真解出来了，而且有内容哈希

    probe: frame0 pts=0/1 1920x1080 fourcc=BGRA planes=0 iosurface=yes bytes=8294400 sha256=afc32c16e3bd549ff99be1615d34808d

属性里 `MetalCompatibility = 1`、`IOSurfacePurgeWhenNotInUse = 1`，色彩三元组 `ITU_R_709_2` 且
`ColorInfoGuessedBy = VideoToolbox`。同一时刻控制台两次 `IOServiceMatching failed for: AppleM2ScalerCSCDriver`：
这台板子没有 Apple 的 M2 色彩转换 HAL，所以色彩信息是 VideoToolbox 自己猜的 —— 这不是故障，但说明色彩管理这条
只能靠我们自己给元信息。

顺手修了探针一个会骗人的地方：IOSurface 后端的 `CVPixelBuffer` 不 lock 就没有基地址，
`CVPixelBufferGetBaseAddress` 返回 NULL，于是 `sha256=` 打印空串 —— 之前那次"帧内容哈希"其实什么都没证明。
现在先 `CVPixelBufferLockBaseAddress(kCVPixelBufferLock_ReadOnly)` 再取地址、算完解锁，才有上面这串哈希。
## B：连续播放两轮都不掉速，`stalls=0`

    probe: end played=30.00 s of 30.00 s wall=30.37 s stalls=0 rate=0.99   （16:32 那次，探针还是旧的二进制）
    probe: end played=28.00 s of 30.00 s wall=30.01 s stalls=0 rate=0.93   （16:52 这轮）
    probe: end played=28.00 s of 30.00 s wall=30.04 s stalls=0 rate=0.93   （16:53 复测）

播放钟每 2 s 才回报一次，所以 `played=28.00` 是最后一次 tick 的读数，不是尾巴掉了 2 s：墙钟 30.01/30.04 s、
`stalls=0`，中间 14 条 `probe: play` 一路 0.04 → 28.00 s 没有断档。屏幕上确实有 `AVPlayer` 的窗口（`probe: window shown, playing`）。

## 解码到底在 VPU 还是 CPU：三条独立证据

1. **插件是选中的我们自己的**。控制台（`logs/video-verdict-1652.log`，16:52:19 起 160 s 抓取）：
   `PianoVideoVT V9: +dec selected com.unixnu.pianovideo.decoder.avc codec 61766331 pid 7114`，
   而 pid 7114 就是探针进程 —— 是这条播放会话挑中了 `com.unixnu.pianovideo.decoder.avc`。
2. **VPU 侧有对应数量的记账**。同一次抓取里 `decision accept` 共 **957** 条（30 s × 30 fps = 900 帧，加上 A 部分那次会话），
   kext 侧 `dfrm` 累计计数从 **3247 涨到 6168**，`dinv` 到 3498。作为对照，播放结束后另抓 45 s 空闲
   （`logs/video-idle-1654.log`）：`PianoVideoVT` 相关一行都没有 —— 这些记账只属于这次播放。
3. **CPU 侧撑不起软解**。播放期间 `ps -Aceo pid,pcpu,comm` 六次采样：探针进程 9.1–16.0 %，
   `WindowServer` 0.8–1.1 %，三个 `VTDecoderXPCService` 全 0.0 %（Apple 自己的 VPU XPC 解码服务在睡觉）。
   1080p30 的 avc1 软解至少要吃掉一个大核，探针这点占用只可能是解码头寸之外的事。

顺带记一条已经知道的毛病，播放不受影响：窗口里 7 条否定回复 ——
`PianoVideoVT V5a: reply dset request 101 status -12900`、`reply dset request 102 status -12900`、
`reply dcop request 107 status -12900`。这些是没实现的 VT 消息类型被拒，解码与播放照常。## 这一轮证明了什么，没证明什么

证明了的（本机实机）：avc1 1080p30 的连续播放在日常装机集上不掉速、不 stall；解码由我们自己的
`com.unixnu.pianovideo.decoder.avc` 承担并且有 VPU 侧记账；单帧解码给出可复核的 BGRA 内容哈希。
这三条合起来就是 v2 日用清单里"单帧硬解 + 连续应用播放"那两项的实测凭据。

没证明的，别当成已过：

- **画面本身**没人眼看过。这一轮只测了时间轴与记账，没有截屏（这台系统 `screencapture` 起不来，
  `osascript` 的 Apple Events 被 TCC 挡住），所以"解出来的图是对的画面"还欠一次人眼确认。
- **HEVC 这一轮没重测**。开机日志里 `+dec selected com.unixnu.pianovideo.decoder.hevc`（pid 372、539）有，
  但那两个 pid 不是探针；本轮片子是 avc1。HEVC 需要拿 `/private/var/tmp/piano-1080p-30s.mp4` 再走一遍同样的三条证据。
- **内容哈希只跑了一次**，还不足以说明"可复现"；同一 clip 同一帧再解两次哈希一致才算。
- `dfrm` 计数一次请求对应几条消息的换算关系还没吃透（30 s 里 957 条 accept 对 2921 条 `dfrm` 消息），
  记在这里免得下次把它当成帧数用。

下一步顺序不变：先让 A/B 的 bit 54 长期置位（不然每 7 次 macOS 冷启动要被 ABL 按在 fastboot 里一次），
然后回到 v2 剩下的音频可闻、触摸 Y 轴方向、时钟持久化这几项。