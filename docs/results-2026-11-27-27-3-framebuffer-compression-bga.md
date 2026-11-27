# Results — 2026-11-27 `&BGA` 是从哪来的：27.3 的 framebuffer 压缩，不是 VM 分支，也不是解码噪声

## 先把那个数字钉死

`641877825 = 0x26424741 = ''&BGA''`。三件独立的事说明它不是 pstore 位衰减造出来的：

- 那颗信箱（`logs/native-runs/ram-k273-sm-20261127-144201/mailbox-9cb00000-700000.bin`，11 副本、容量 661294）里
  `Unimplemented pixel format of` 出现 **14 次，在 14 个不同的 ring 偏移**（102924、135206、145631、159036、177032、190937、
  203997、215703、228253、243278、260327、271858、293202、328036），是 14 次独立写入的日志。
- 把逐位表决的阈值从 `need=6`（脚本按直方图自适应选的那档）扫到 4、5、7，这 14 处**每一位都读出同一个数字**；
  同一份日志别处确实有衰减（`dyld+0x#1f90` 应为 `0x31f90`，`need=7` 时还会把某个 `pid 430` 读成 `.30`），
  所以这不是"整份日志都可靠"，而是"这一行的数字稳到能扛住阈值变化"。
- 我们自己已经给这个值起过名字：`package/NativeGopDisplay/GopShot.h` 里
  `#define GOP_SHOT_BGA 0x26424741u  // ''&BGA'', linear plane 0 with BGRA8 pixels`；
  `docs/k273-line.md:134` 记的维护者 27.3 首启也是 `a frame surface 3200x2136 format 0x26424741 …`。
  上游那句解释是"Apple 的无损压缩 BGRA 写法，单 plane"。

## 27.3 的实现读在哪、读出什么

读的是 pin 住的 27.3 输入，不是宿主：`out/inputs/k273/26C5035r/141-22145-021.dmg` 造出的那份 SystemOS 卷挂在
Mac `/private/tmp/soscheck/mnt`，它的 `System/Library/dyld/dyld_shared_cache_arm64e` 是
`Magic = dyld_v1 arm64e`、`OS Version = 27.3`、4142 个 image。宿主那台 MacBookPro（macOS 27.2.1）上没有共享缓存，
OS 盘 `141-22233-021.dmg` 里也只有骨架没有 Mach-O，所以能读的只有这份。用 `/opt/homebrew/bin/ipsw dyld` 读：

- `CA::WindowServer::IOMFBDisplay::framebuffer_pixel_format()` 在 `0x18e1e1048`。它先备好候选
  `''RGhA''`（`mov w19,#0x6841 ; movk w19,#0x5247,lsl 16`），按 EDR/能力位可能换成 `''b3a8''`（`0x62336138`）或
  `''BGRA''`（`0x42475241`），**然后把这个候选交给一条 vtable 调用**（`blraa`，参数 `(this, fmt, 0, 0)`，
  符号 `CA::WindowServer::AppleDisplay::compressed_pixel_format(unsigned, unsigned long, SurfaceUsage)` 在 `0x18defb0ec`），
  返回的是那条调用的结果。函数尾部还有 `''&b38''`（`0x26623338`）到 `''&w4a''`（`0x26773461`）、`''b3a8''` 到 `''w40a''`
  （`0x77343061`）两条换算 —— 也就是 `''&xxx''` 一族就是同一批像素的压缩写法。
- 决定压不压的两个门都是"读一次就缓存"的布尔：
  `CADeviceUseFramebufferCompression` 在 `0x18e12b26c`（两道 once-init，各带一个名字串），
  `CADeviceSupportsUniversalCompression` 在 `0x18e12b564`（能力字节 `& ~` 一个标志）。
  QuartzCore 的字符串表里 `0x18e285b1f` 是 `CA_DISABLE_FRAMEBUFFER_COMPRESSION`，
  另有 `CA_PRINT_UNCOMPRESSED_IOSURFACE`、`CA_DISABLE_COMPRESSED_CG_IMAGES` 和 IOMFB 侧的
  `IOMFBCompressionSupport`、`IOMFBAsyncRGBCompressionSupport`、`IOMFBMaxCompressedSizeInBytes` 等显示属性。

顺带排除 `docs/drivers/soft-metal.md` §9 那条 VM 分支：本机 27.3 用的 ADT 早就是 `vmm-present = 0`
（`configs/k273-adt.toml:86`，`tooling/k273/build-k273-image.sh:39` 也默认 0），
这发的 `out/adt/piano-k27.adt`（`8ebbcdb6…`，就是镜像里那份）里读得到 `vmm-present` 且 k273 身份是 Mac17,11 / chip 0x6050。
所以 26.6 上那台"半浮点 framebuffer"的病根和这台 27.3 上的病根不是同一个：**27.3 是 framebuffer 压缩开着**，
`''BGRA''` 在交给软件合成器之前被改写成了 `''&BGA''`，而 SkyLight 的软件合成器只会写不压缩的格式。
（上游那条线不会撞这堵墙：他们的 WindowServer 有真 GPU，走的是 `WSCompositeDestinationCreateWithMetalTexture`。）

## 这一轮改了什么

两处，都带判读用的日志，`soft-metal-fb8=0` 一律关掉：

- `package/NativeSoftMetal/NativeSoftMetalPlugin.mm`：`coreAnimationOptions()` 原来只看"当前为 on 的选项"，
  所以永远碰不到一个默认 off 的选项。现在按名字先看表里有谁，再把 `CA_DISABLE_FRAMEBUFFER_COMPRESSION`
  **置为 on**（`CASetDebugOption`），并保留原来清 `CA_FORCE_FP16_FRAMEBUFFER` / `CA_FORCE_CIF10_FRAMEBUFFER` 的行为；
  如果这张表里根本没有这个名字，第一次扫完就说一次，别什么都不印。
- `tooling/bootimg/build-ram-images.py`：`exec-env` 增加第二组
  `WindowServer:CA_DISABLE_FRAMEBUFFER_COMPRESSION=1`（k27 0022/0025：内核按可执行文件基名匹配后往环境里追加变量）。
  这条是为了抢在缓存之前 —— `CADeviceUseFramebufferCompression` 读一次就存，插件是在 WindowServer 里才加载的，
  显示 framebuffer 可能在那之前已经定完格式；96 字节的值仍在那 256 字节缓冲里。

**判读（还没跑，构建在跑）**：镜像 `out/ram-verified-8748-sm/boot-piano-macos-ram-A-bundled.img`（下一发的 sha 记在
`artifacts/ARTIFACTS.md`），命令
`powershell -NoProfile -File tooling/native/run-console-ram-k273.ps1 -ImgRel out\k273-ram-8748-sm\boot-piano-macos-ram-A-bundled.img -Tag fbc -ImageSha <sha>`。

- 控制台出现 `CA option N CA_DISABLE_FRAMEBUFFER_COMPRESSION set`，并且**不再有** `crash message (SkyLight)`，
  接着 `NativeGopDisplay: first swap submitted` / `first frame` —— 这一堵墙拆了。
- `no CA debug option named CA_DISABLE_FRAMEBUFFER_COMPRESSION` —— 27.3 的名字不在这张表里（这张表是
  `CAGetDebugOptionEnvVariableName` 给的），那就回到环境变量那条路单独验，或者去查显示属性
  `IOMFBCompressionSupport`（那要落在 NativeGopDisplay 发布什么上）。
- 照旧 abort 且数字仍是 641877825：说明格式在插件加载前就定了，环境变量那一路也没生效，
  下一步就把 `CADeviceUseFramebufferCompression` 的缓存时机钉到 WindowServer 的启动序列上（`exec-log=1` 已经开着）。

`display on` 之后屏上仍可能是启动控制台：`first frame` 才算第一帧。