[English](README.md) | 简体中文

# UniXNU-piano

在小米平板 8 Pro（`piano`，骁龙 8 至尊版 SM8750）上原生运行 macOS 27。本仓库是 [UniXNU](https://github.com/UniXNU/UniXNU) 的设备仓库：
https://github.com/lenzhang/UniXNU-xiaomipad8pro（原名 lenzhang/UniXNU-piano，本地检出仍沿用 UniXNU-piano 这个名字）。
27.3 线的 k273 XNU（k27 源码树加上 `patches/k27` 0053-0074 和 `patches/k273`）作为 HLOS 运行在 EL1，Gunyah 仍在 EL2。
启动链：ABL → BootShim → Mu-Silicium UEFI（FD，即 piano-uefi 的 `pianoPkg` 加上本仓库的改动）→ UniXNU 载荷与菜单 → DarwinBoot → XNU。
UniXNU 核心、Mu-Silicium、piano-uefi、DarwinBoot 和 SoC 模块都是固定版本的依赖（`unixnu.lock`、`feeds.toml`），不是分叉。

相关公开仓库：[UniXNU-sm8750-common](https://github.com/lenzhang/UniXNU-sm8750-common) 是今后共享的 SM8750 部分的占位仓库
（代码目前仍在本仓库，构建固定使用另一个 SM8750 模块）；[veil](https://github.com/lenzhang/veil) 是早期 Linux + Gunyah 客户机路线的冻结存档。
仓库、本地目录和规则的总览见 [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)。

## 当前状态（2026-11-26）

原生 macOS **27.3（26C5035r）** 既能开机从磁盘启动，也能以 RAM 镜像启动。当前日常安装版本是 **v13.1y12m**（2026-11-26，在 v13.1y12l 上只更新 ESP；其 RAM 资格测试 429 项通过，安装轮次 67 项检查通过，3 项失败都是检查器漏判，证据显示功能本身正常）。
系统识别为 Mac17,11，显示为 "Xiaomi Pad 8 Pro"，芯片为 "Snapdragon 8 Elite"。根卷在 `linux_rootfs`，机主的 Data 存储在 `linux_data`。
日常能耗模式为“高功率”。与 Android 对齐的 CPU/GPU 频率曲线和 PD 降压充电（9 V）默认开启，电荷泵保持关闭。

已安装系统上目前可用的功能：从磁盘启动并保留机主数据、60 Hz 显示、原生触摸屏、键盘保护套及其触控板、Wi-Fi（开机自动连接、开关、
断线后自动重连）、扬声器、电池电量百分比、5 V 和 9 V PD 充电及 80 % 充电上限、自动亮度、Adreno 830 上的 Metal 及分页 GPU 内存、
4 GiB GPU 窗口（4032 像素照片上的 Core Image）、Photo Booth 与摄像头录像、QuickTime 和 Safari 中的硬件视频解码（H.264 最高 1080p）、
Safari 的 WebGL 2 和 WebGPU，开启 GPU 沙盒的 Chrome GPU 加速（WebGL、GPU 合成、流畅滚动和视频），以及自 v13.1y12m 起无需启动参数的
Chrome WebGPU（内核补丁 0075，47 位用户地址空间）。自 v13.1y12m 起日常版本还包括：保护套和电源键睡眠策略（接着充电器时合上保护套只锁屏不睡眠；
按电源键睡眠和唤醒）、由显示驱动截取的原生截图，以及低功耗蓝牙事件掩码修复。已安装系统使用 13 GiB 内存窗口。
尚不支持：蓝牙配对（需要实机测试）、快速磁盘写入、电荷泵快充、日常 120 Hz、USB 主机设备和外接显示器。进行中：原生面板休眠、夜览和刷新率切换
（已构建，默认关闭，资格测试中）、蓝牙键盘和鼠标、用电池时合上保护套（现场检查）、磁盘速度、自动旋转，以及面向其他机器的安装指南。

### 硬件支持

所有状态都来自平板上的实际运行。“可用（已安装）”指在安装到平板磁盘的系统上验证过（版本见该行）；
“仅 RAM”指只在 RAM 启动镜像上验证过。

✅ 完整支持 · 🟡 部分支持 / 进行中 · ❌ 尚不支持

| 功能 | 状态 | 验证（版本，日期） | 说明 |
| --- | --- | --- | --- |
| 从磁盘启动、持久安装 | ✅ 可用（已安装） | v13.1y12m，2026-11-26 | 从磁盘启动，ESP 配置完全一致，机主的 Data 存储正常恢复，启动保护正常，无 panic。v13.1y12m 是在 v13.1y12l 上只更新 ESP 的版本：安装轮次 67 项通过；其 3 项失败都是检查器漏判（控制台行被截断、匹配模式过宽、在错误的时间窗口读日志），证据显示功能本身正常。安装只写改动部分，先备份，写后回读校验，并可回滚到上一个版本。 |
| 显示 | ✅ 可用（已安装），60 Hz | v13.1y12m，2026-11-26 | 画面通过 PianoDpu 在启动帧缓冲的图层管线上翻页送到面板；CPU 仍要做逐帧工作（缓存清理，分 4 个线程）。整个会话中没有欠载、回退或 SMMU 错误。由显示驱动在设备上直接截取的原生截图已在日常版本中开启。原生面板休眠、夜览和刷新率切换已构建，但默认关闭（资格测试中）。 |
| 120 Hz | 🟡 仅 RAM | RAM gpu-hz120-2，2026-11-21 | 系统能提供 120 Hz 模式，且在显示关闭再打开后保持；实际每秒呈现 73-78 个新帧。日常配置仍为 60 Hz。 |
| 触摸屏（原生） | ✅ 可用（已安装） | v13.1y12l，2026-11-26；手势在 RAM touch-native3，2026-11-23 | 绑定到显示器的原生内置触摸屏：点按、长按（右键）、在滚动视图中拖动与滚动、双指缩放、旋转、智能缩放、防误触、90 度坐标（RAM 上 44 项通过 / 0 项失败）。已安装系统：开机即发布并被系统接管。原生路径的长期手动使用仍待验证。 |
| 键盘保护套与触控板 | ✅ 可用（已安装） | 机主实测，v12，2026-11-19；v13.1y12l，2026-11-26 | 打字和触控板可用；登录窗口能识别保护套键盘。 |
| Wi-Fi | 🟡 可用（已安装），部分 | v13.1y12l，2026-11-26；v13.1x，2026-11-21 | 开机自动连接（WPA2、DHCP、DNS）；开关关闭再打开后重新连接；强制断线后几秒内重连；国家码已设置。待验证：手动点击网络列表中的某一行加入、热点消失又恢复后的自动重连、多接入点的办公室网络。 |
| 蓝牙 | ❌ 尚未支持（进行中） | 机主在已安装的 v13.1 版本上测试，2026-11-20；v13.1y12m，2026-11-26 | 能发现设备；低功耗蓝牙连接曾在链路就绪前超时（HCI 0x08），因此键盘和鼠标无法配对。低功耗蓝牙事件掩码的修复现已进入日常版本（v13.1y12m：控制器就绪，蓝牙守护进程启动时设置了掩码）；键盘和鼠标配对仍需实机测试。与 Wi-Fi 的射频共存（共享天线）正在分析。蓝牙音频尚未开始。 |
| 扬声器 | ✅ 可用（已安装） | v13.1u，2026-11-21（有声测试音） | 播放、音量和静音；扬声器保护（增益 243）和原厂杜比 “Movie” 音效在 DSP 上运行。自 2026-11-23 起应要求静音安装，之后的安装只检查输出设备而不播放测试音。 |
| 麦克风 | 🟡 部分可用 | v13.1y12l，2026-11-26；降噪在 RAM mic-ns4b，2026-11-23 | 已安装系统的音频系统中有原始 “Mic” 设备（默认输入）和 “Mic (noise reduced)” 设备（RNNoise）。边播放边录音的卡顿已在已安装系统上修复（2026-11-21）。用应用（QuickTime）录音并手动回放仍待验证。 |
| 摄像头：Photo Booth | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 前置摄像头实时预览，无授权弹窗；特效网格可以打开。 |
| 摄像头：QuickTime | 🟡 部分可用 | v13.1y12l，2026-11-26 | 前置和后置摄像头都能录 10 秒 H.264 视频（AVFoundation，QuickTime 高质量预设）。手动“新建影片录制”并保存回放仍待验证。编码为软件编码。 |
| 摄像头：FaceTime | 🟡 尚未验证 | 权限已预先授予，v13.1y12l，2026-11-26 | 两个摄像头都已列出，FaceTime 已有摄像头和麦克风权限；尚未检查 FaceTime 自拍预览。 |
| 充电：5 V | ✅ 可用（已安装） | v13.1y12d，2026-11-24 | 3 A 电源下最高 3 A（输入限流 3000 mA；在电脑接口上，总线电压降到约 4.72 V 时输入稳定在约 1.8 A）；电池净充电电流为正。 |
| 充电：PD 9 V（降压） | ✅ 可用（已安装） | v13.1y12d，2026-11-24 | 默认开启。原装充电器上的固定 9 V PD：9 V / 1.6 A（约 14 W，即充电器固件请求的电流，与 Ubuntu 相同）。PPS 9 V（最高 2.5 A）已构建但未运行。 |
| 充电：80 % 上限 | ✅ 可用 | RAM chg-buck6，2026-11-17；此后每个已安装配置都包含 | 充到 80 % 停止，降到 75 % 恢复。 |
| 充电：电荷泵 / MiPPS | ❌ 尚未支持 | 首次有人值守尝试，2026-11-24 | 默认关闭。首次尝试时输入限流停在 100 mA（一个未映射的厂商电源对象）；已在版本中修复，下一次有人值守测试待进行。 |
| 电池电量百分比 | ✅ 可用（已安装） | v13.1y12k，2026-11-25 | 百分比、充电状态和电量计读数正确。循环次数、健康度和充满时间等字段尚未填写。 |
| 睡眠与唤醒 | 🟡 部分可用 | v13.1y12m，2026-11-26；RAM v13.1y12m 资格测试，2026-11-26 | 挂起到空闲并定时唤醒可用；Wi-Fi 和 USB 链路能恢复；锁屏界面正常出现。保护套和电源键策略已在已安装系统上启用：接着充电器时合上保护套只锁屏不睡眠（继续充电），按电源键睡眠和唤醒（两者都在 RAM 资格测试中验证）。用电池时合上保护套仍需现场检查。 |
| 亮度：手动 | 🟡 部分可用 | 按键在已安装 v12，2026-11-19；滑块路径在 v13.1i | 亮度键可用。通过系统亮度控制的滑块路径可用；在控制中心手动拖动仍待验证。 |
| 亮度：自动 | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 光线传感器驱动自动亮度；手动调整不会退出自动模式。真实光线变化范围的测试仍待进行。 |
| GPU：Metal（Adreno 830） | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 我们的 Metal 插件驱动 WindowServer、桌面和应用；ImageIO、基于 Metal 的 Core Image、MetalKit 纹理加载和快速查看缩略图全部通过：30 条评分路径 30 条通过，包括此前内存不足的 4032 像素 Core Image 和 HEIC 纹理路径；预览能打开 4032 像素的照片。 |
| GPU：分页内存 | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 4 KiB 分页 GPU 内存（上限 7168 MiB，无固定池），无拒绝、无缺页错误；默认开启重放式挂起恢复。 |
| GPU：Android 级频率曲线 | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | Android 的 14 级频率表，最高 1100 MHz，含其表面温度和热控级别以及 KGSL 式调频器；Android 的 CPU 频率表。持续 3D 基准约 50-54 fps，734 MHz（RAM，2026-11-23）。 |
| GPU：4 GiB 窗口 | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 256 GiB 地址空间中 64 个 4 GiB 客户端窗口，单个对象 1 GiB，没有被拒绝的 GPU 打开请求。修复了 4032 像素 Core Image 内存不足的路径（Metal 图像检查 30/30）。 |
| Safari：WebGL 2 和 WebGPU | ✅ 可用（已安装） | v13.1y12m，2026-11-26 | WebGL 2 开启，WebGPU 适配器为 “adreno”（WebGPU 页面约 52 fps，回读正确）；Safari 的 GPU 进程能打开加速器。 |
| Safari：硬件视频解码 | ✅ 可用（已安装） | v13.1y12m，2026-11-26；v13.1y12i，2026-11-25 | 1920x1088 以内的 H.264 在 Iris 视频核心上解码：1080p 600 帧，丢帧 0。 |
| Safari：HEVC 和 4K | ✅ 可用（已安装），软件解码 | v13.1y12j，2026-11-25；v13.1y12m 上再次播放 | HEVC 2160p 600 帧 / 丢 0 帧，29.9 fps；H.264 2160p 解码 591 / 丢 5，均由 Apple 的软件解码器完成（按设计）。不支持 VP9 和 AV1。 |
| Chrome：GPU 加速 | ✅ 可用（已安装） | v13.1y12m，2026-11-26；视频在 v13.1y12l | 在开启 Chrome GPU 沙盒、不加任何启动参数的情况下：WebGL 经 ANGLE 运行在 Metal 上（Adreno 830，v13.1y12m 上 WebGL 测试约 38 fps）、GPU 合成、2D canvas 56 fps、滚动 60 fps 且没有超过 34 ms 的帧。所有测试视频都能播放，丢帧 0-1：1080p 和 2160p 的 H.264 与 HEVC，1080p 的 VP9 与 AV1（HEVC 经 VideoToolbox，其余由 Chrome 的软件解码器完成）。没有崩溃报告。RAM 启动时不使用 Chrome（它可能耗尽基于内存的 Data 盘）。 |
| Chrome：WebGPU | ✅ 可用（已安装） | v13.1y12m，2026-11-26 | 无需任何启动参数：适配器为 Qualcomm “adreno-8xx”，WebGPU 上下文 6/6，计算结果正确，WebGPU 画面绘制并回读成功。内核补丁 0075 为进程提供 47 位用户地址空间，使 Chrome 的 V8 沙盒能预留完整区域（1152 GB）。未决细节：Chrome 的 17 个进程中有 1 个报告非安全的 V8 沙盒模式（调查中）。 |
| 视频播放（QuickTime） | ✅ 可用（已安装） | v13.1x，2026-11-21 | AVC High 和 HEVC Main10 硬件解码；各播放约 100 秒，摄像头记录中没有闪烁或黑帧。 |
| 能耗模式 | 🟡 部分可用 | v13.1y12l，2026-11-26 | 用 `pmset` 设置的低功耗、自动和高功率（日常设置）都能传到电源和热控策略。系统设置中的选择器尚未手动验证。 |
| 登录窗口空闲 CPU | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 已修复：一个音频守护进程在登录窗口反复循环（约占 1.3 个核心）；现在它会被暂停到登录之后。 |
| 应用启动 | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | 计算器、文本编辑和备忘录的第一个窗口都在 2.5 秒内打开。 |
| USB 调试（控制台、root shell、ssh） | ✅ 可用（已安装） | v13.1y12l，2026-11-26 | USB-C 调试控制台（其监视器可让平板回到引导程序）、root shell 和数据线 ssh；整个启动期间端点无卡死。 |
| 系统切换（Android / macOS） | 🟡 仅 RAM，只读 | RAM v13.1y12e，2026-11-24 | 启动标志的 sysctl 能读取标志；写入按设计被拒绝。尚未做过完整切换。 |
| USB 主机 / OTG | ❌ 尚未支持 | RAM usb-a2d，2026-11-18（空端口） | 主机模式能在空端口上启动；VBUS 供电和真实设备尚未完成。 |
| 外接显示器（USB-C DP） | ❌ 尚未支持 | 无 | 仅有设计。 |
| 自动旋转及其他传感器 | ❌ 尚未支持 | RAM，2026-11-19（加速度计采样） | 光线传感器可用；日常配置中自动旋转关闭，正在分析中。 |
| 硬件视频编码 | ❌ 尚未支持 | 摄像头录像，2026-11-25 | 录像使用软件编码器。 |
| 红外发射、手写笔、手电筒 | ❌ 尚未支持 | 无 | 驱动已构建或已设计，未运行。 |
| 内存（16 GB） | 🟡 部分可用 | v13.1y12l，2026-11-26 | 已安装系统使用 13 GiB 窗口（13310 MiB；此前为 8 GiB）。空闲时：47 % 空闲，无压缩，无交换，无内存压力。进行中：GPU 内存分层和驱动内存池。 |
| 内置存储（UFS） | 🟡 部分可用 | v13.1y12l，2026-11-26（首次测量） | 在 Data 卷上与同一台机器的 Android 对比：队列深度 1 的 4K 随机读与 Android 持平（15580 对 14967 IOPS），但读取不随队列深度提升（8 个并发：15696 对 88663）；写入为 Android 的 0.18-0.24 倍（顺序 519 对 2137 MB/s，4K 随机 6494 对 36693 IOPS）；有写入进行时读取从 14490 降到 1300 IOPS。WriteBooster 已由启动固件开启。修复正在进行中。 |
| 指纹、NPU | ❌ 暂缓 | 无 | 没有安全隔区（无法使用触控 ID）；此 SoC 上没有 Core ML 后端。 |

详细说明（状态定义、已知缺口、效果展示）：[docs/STATUS.zh-CN.md](docs/STATUS.zh-CN.md)。
今天的平板测试结果：[docs/results-2026-11-26.md](docs/results-2026-11-26.md)（英文；昨天的：
[docs/results-2026-11-25.md](docs/results-2026-11-25.md)）。

**效果展示：** 各项功能的截图和短片（在平板上直接截取）收录在 [docs/STATUS.zh-CN.md#效果展示](docs/STATUS.zh-CN.md#效果展示)
（第一批原生截图来自已安装的 v13.1y12m：关于本机、显示器、电池、访达、活动监视器、Metal 3D、Safari 视频）。

USB-C 调试控制台（`package/PianoDwc3`、`tooling/native/usb-console.py`）和通过数据线的 ssh 都可用；控制台监视器的
`reset fastboot` 命令无需按键即可让平板回到 ABL。日常开发使用 `fastboot boot` 启动 RAM 镜像，它优先于已安装的系统。

各驱动的说明、启动参数和测试级别：[docs/drivers/README.md](docs/drivers/README.md) 和 [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)。
已安装系统的使用、更新和恢复：[docs/USING.md](docs/USING.md)。更早的公开记录包括
[docs/results-2026-11-19.md](docs/results-2026-11-19.md) 和 [docs/results-2026-11-17.md](docs/results-2026-11-17.md)。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `patches/k27-tree/` | 我们在 k27 源码树固定上游版本（macOS-DroidVM）之上的改动：构建脚本、KC 构建器、内核补丁 0050-0054 |
| `patches/k27/` | k27 内核补丁 0053-0074，基于 k27 源码树（xnu-12377 + 基线补丁 0001-0052） |
| `patches/k273/` | 27.3 线在 `patches/k27` 之上的内核补丁 |
| `patches/k27-kc/` | KC 构建器的原生布局（最佳适配、`__TEXT` 中无重定位常量），需手动应用到 k27 树（[docs/kc-space.md](docs/kc-space.md)） |
| `patches/xnu/` | UniXNU macOS 27.3 线的设备补丁（目前为空） |
| `package/` | 原生 kext；没有对应启动参数时都不生效 |
| `configs/` | 原生 kext 列表（`k27-native-kexts.txt`）、ADT 标识与显示节点（`k27-adt.toml`、`k27-gop-display.toml`）、27.3 线的内核配置 |
| `dts/piano.dts` | 用于 ADT 的 piano 硬件描述 |
| `.github/`、`scripts/` | CI（检查、每月依赖检查、构建、手动发布：[docs/ci.md](docs/ci.md)）与 `fetch-pins.py` |
| `tooling/` | `k273/build-k273-image.sh`（27.3 kext → 内核集合 → ADT → RAM 镜像，`K273_KERNEL`）、`k273/build-k273-esp.py`（已安装系统的 27.3 ESP）、`kc/`（KC 构建）、`adt/`、`bootimg/`（RAM 镜像及其校验）、`native/`（测试循环、USB 控制台、输入）、`persist/`（安装到磁盘）、`firmware/`（FD） |
| `firmware/` | 对 piano-uefi 的 `pianoPkg`、Mu-Silicium 和 UniXNU 的改动 |
| `docs/` | 移植笔记、驱动说明（`drivers/`）、计划和证据 |
| `skills/`、`tooling/repro/` | 面向其他设备的代理指南及工具：Apple 输入文件的固定版本、自行下载、根卷重建校验、主机检查 |
| `logs/`、`artifacts/ARTIFACTS.md` | 已提交的验证日志；构建产物和输入的 SHA-256 |
| `private/`、`out/` | 被忽略：密钥、单机校准数据；构建输出 |

## 构建

新设备的完整流程（要求、输入、构建、RAM 启动验证、受保护的安装、回滚、检查）见安装指南
[skills/install-piano-macos/SKILL.zh-CN.md](skills/install-piano-macos/SKILL.zh-CN.md)（中文版；英文原版
[SKILL.md](skills/install-piano-macos/SKILL.md) 面向编程代理）。本节是摘要，以该指南为准。

### 主机要求

- **必须是 Apple 芯片的 Mac（M1 或更新）。不支持 Intel Mac。** Xcode 27 的编译器和链接器以及 macOS 27 自带的工具只有 arm64 版本，
  而且有两个构建步骤会运行自己编译出的 arm64e 程序。证据见安装指南 §1.1。
- 已测试：macOS 27.2.1 加 **Xcode 27.2**（需要完整的 Xcode，仅有命令行工具不够：内核需要 iphoneos SDK 和 `Kernel.framework`，
  并用 Xcode 的 `ld-classic` 链接）。Homebrew 装在 `/opt/homebrew`，并安装安装指南 §1.2 列出的软件包、`fastboot`、Python 3.11 或更新版本；
  约 90 GiB 可用磁盘空间；根卷步骤需要在前台终端里使用 `sudo`。
- `bash tooling/repro/check-host.sh` 只读检查主机，每项输出 `ok` / `WARN` / `FAIL` / `GAP`；只有 `0 FAIL` 时才继续。
  请在原生 arm64 shell 中运行，不要在 Rosetta 下运行。

### 源码与固定版本

- 本仓库，加上 `unixnu.lock` / `feeds.toml` 中固定版本的依赖（UniXNU 核心、Mu-Silicium、piano-uefi、DarwinBoot、SoC 模块），
  以及 k27 内核源码树（`PIANO_K27_TREE`，写在环境变量或 `~/.config/piano/env` 中；版本固定在 `unixnu.lock`）。
- **拉取固定版本：** `python3 scripts/fetch-pins.py` 按提交把 `unixnu.lock` 里的每个依赖拉到 `$UNIXNU_RESEARCH`
  （`--verify` 只做校验；由 SorasakiHinaHusband 贡献）。k27 内核源码树是固定版本的上游
  [macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) 加上我们的 [patches/k27-tree](patches/k27-tree/README.md)：
  `tooling/k27/fetch-k27-tree.sh DEST`，然后设 `PIANO_K27_TREE=DEST`。
- 本项目从不分发 Apple 的文件。`tooling/repro/fetch-ipsw.py` 从 Apple 的 CDN 下载固定版本 27.3 IPSW 的组成文件，从 Apple 获取 AEA 密钥，
  并按 `tooling/repro/pins.json` 校验每个文件。
- 有些输入目前仍只存在于原始环境（k27 内核源码树、基础信任包、Data 种子、Turnip 源码树、写入工具中的单机固定值）：
  指南的 "Known gaps" 一节逐项列出并给出可选方案。

### 构建步骤

```sh
bash tooling/repro/fetch-and-build.sh --dry-run   # 列出将要执行的每条命令；不下载、不构建、不写入
bash tooling/repro/fetch-and-build.sh             # host、inputs、systemos、root、kernel、fd、image（也可按名字只跑一步）
bash tooling/repro/fetch-and-build.sh --check     # 按固定值检查每一步的输出（不联网）
```

它执行的步骤，也可以逐个运行：

```sh
bash tooling/k273/prepare-k27-tree.sh          # 27.3 内核源码树及重新链接的内核；打印 K273_KERNEL 的路径
bash tooling/firmware/build-fd.sh               # 从 unixnu.lock 的检出构建 UEFI FD
K273_KERNEL=<上一步打印的路径> bash tooling/k273/build-k273-image.sh   # kext → 内核集合 → ADT → RAM 镜像
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

- **kext 与内核集合：** KC 只链接本仓库的 kext（`configs/k27-native-kexts.txt`）；`tooling/k27/check-native-kexts.py` 在链接前后都会强制检查。
  KC 日志中的通过标志：`native kext rule: ... all this repository's`。
- **根卷镜像在本地生成，从不分发。** 20.9 GB 的根卷由 IPSW 的 OS 卷（逐字节一致，版本固定）加上由 `tooling/k273/make-systemos.sh`
  和 `make-root-image.sh` 重建的 SystemOS 窗口组成，并由 `tooling/repro/verify-root.py` 按固定的文件级清单校验（指南 §3.1）。
- 只构建 27.3 线；已退役的 27.0.1 路线（`tooling/k27/build-native-image.sh`）不再使用。根卷位置：新设备先把根卷放在 `linux_data`
  （`K273_ROOT_PART=linux_data`，指南 §3.2）；参考设备的已安装 ESP 使用 `build-k273-esp.py --root-part linux_rootfs`
  （[docs/persistent-boot.md](docs/persistent-boot.md) §13.8）。

### CI 与发布

FD、k27 原生内核和我们的 kext 也可以在 GitHub 托管的 runner 上构建（[docs/ci.md](docs/ci.md)）：拉取请求会运行依赖拉取、
自测和隐私扫描；发布版本（在平板验证通过后手动打标签）只包含我们自己的二进制、构建配方、SHA-256 清单和安装 skill。
根卷镜像和内核集合始终在用户本地生成。

### RAM 镜像的大小上限

RAM 启动镜像**最多 250,000,000 字节**。ABL 会把整个镜像载入 UEFI 的 256 MiB 内核区域；一个 294,952,960 字节的镜像在下载后一直停在
fastboot，而 247,767,040 字节的镜像可以启动（[docs/k273-line.md](docs/k273-line.md) §4）。该上限是 `tooling/bootimg/build-ram-images.py`
中的 `IMAGE_MAX_BYTES`（镜像将要超限时它会压缩载荷的空闲空间），`tooling/bootimg/verify-ram-images.py` 会再检查一次
（两者都读取 `PIANO_IMAGE_MAX_BYTES`；不要调大）。

### 安装前先做 RAM 启动

每个镜像都要先用 `fastboot boot` 启动（RAM 启动不写任何分区），在那里通过后才能写入磁盘：控制台能走到 XNU，USB 控制台有响应，
启动能通过复位保护回到 ABL；桌面镜像还要能正确显示登录窗口，并且触摸、键盘保护套、ssh、Wi-Fi 和电池读数都正常（指南 §4）。
安装只能通过 `tooling/persist/maint-session.py` 进行，每一步都有备份、回读校验和回滚
（[docs/persistent-boot.md](docs/persistent-boot.md)、[docs/install-esp-v3.md](docs/install-esp-v3.md)）。

更多：启动参数与无屏调试见 [docs/bringup-debugging.md](docs/bringup-debugging.md)（§4 说明从 k27 树读取哪些内容）；
当前可用功能见 [docs/STATUS.zh-CN.md](docs/STATUS.zh-CN.md)。

## 启动与测试

日常开发使用 RAM 启动：`fastboot boot` 一个 RAM 镜像（它优先于已安装的系统），由无人值守的循环启动测试、复位并读回测试日志
（`tooling/native/native-loop.sh`，[docs/bringup-debugging.md](docs/bringup-debugging.md) §3）。安装到磁盘
（[docs/persistent-boot.md](docs/persistent-boot.md)）只写该文档列出的区域，每个区域先做经过校验的备份，每次写入后回读校验，
每一步都有回滚；从不写固件分区。请勿手动刷写 boot 或固件分区。

## 原生 kext 规则

原生内核集合只链接本仓库的 kext（`configs/k27-native-kexts.txt`）。`tooling/k27/check-native-kexts.py` 在每次构建的链接前后都强制检查：
不允许来自别处的描述文件，也不允许虚拟机时代的半虚拟化 kext。

## 延伸阅读

- [docs/STATUS.zh-CN.md](docs/STATUS.zh-CN.md)：逐项功能状态，以及每项验证时的版本和日期。
- [docs/USING.md](docs/USING.md)：已安装系统的使用、更新和恢复。
- [skills/install-piano-macos/SKILL.zh-CN.md](skills/install-piano-macos/SKILL.zh-CN.md)：在其他设备上构建和安装（安装指南，中文版）。
- [docs/drivers/README.md](docs/drivers/README.md)：所有驱动、其级别和状态。
- [docs/PROJECT-MAP.md](docs/PROJECT-MAP.md)：仓库、本地布局、硬性规则。
- [docs/bringup-debugging.md](docs/bringup-debugging.md)：启动链、无屏调试、测试循环、重新构建。
- [docs/BACKLOG-hardware.md](docs/BACKLOG-hardware.md)：尚待适配的硬件。
- [docs/kc-space.md](docs/kc-space.md)：内核集合中留给原生 kext 的空间、运行时固件加载器。

（以上文档除注明外均为英文。）

## 许可与内容

除非某个软件包的 `LICENSE`、文件的 SPDX 头或 `UPSTREAM` 文件另有说明，本仓库代码采用 BSD-2-Clause-Patent，
Copyright (c) 2026 Leo Zhang（第三方部分保留其原有版权人和许可）。
本仓库从不包含厂商固件、分区镜像和 Apple 载荷（内核集合、信任缓存、由 IPSW 派生的文件），也不包含机密信息、密钥、单机校准数据、
本地网络或存储细节以及机器名。

`scripts/fetch-pins.py` 最初由 SorasakiHinaHusband 编写（拉取请求 #2）。
