[English](SKILL.md) | 简体中文

<!-- 本文件是 SKILL.md 的中文版，内容和顺序与英文版一致；命令保持原样。代理加载的技能文件仍是 SKILL.md。 -->

# 在小米平板 8 Pro（piano）上安装原生 macOS 27.3

你是操作者。平板的机主坐在平板旁边负责按键，你在 Mac 上运行命令。请按顺序执行各节。每一步都写明了要运行的命令、它会写入什么，
以及哪一行输出表示通过。**遇到第一个失败就停下，并报告给机主。** 不要重试写入操作，不要绕过任何拒绝。

所有路径都相对于本仓库的检出目录。`tooling/repro/` 是本指南调用的工具包。Apple IPSW 中的任何内容都不会被提交或分发：每个用户都从
Apple 获取它，并在自己的 Mac 上重新生成派生文件。

## 0. 状态：第二台设备目前能做到什么（2026-11-26）

参考平板运行 **v13.1y12o**，包含内核补丁 **0076**。见[安装结果（英文）](../../docs/results-2026-11-26.md#v131y12o-installed)。

| 章节 | 对第二台设备的状态 |
|---|---|
| 1 要求 | 就绪：`tooling/repro/check-host.sh` |
| 2 来自 Apple 的输入 | 就绪：`tooling/repro/fetch-ipsw.py`（每个组成文件都有固定值，AEA 密钥来自 Apple）和 `boot-dtb.py` |
| 2 单机数据 | 部分就绪：通过 Ubuntu 安装工具从本机提取（G5） |
| 3 根卷镜像重建 | 如果你有 k27 树的两个启动垫片（G1b）则就绪：`make-systemos.sh`、`verify-root.py`、固定的文件清单 |
| 3 内核、KC、RAM 镜像 | **缺少 k27 树（G1）、基础信任包（G2）、Data 种子（G3）、Turnip 树（G4）时无法进行** |
| 4 RAM 启动 | 第 3 节构建完成即可（`fastboot boot` 路径不需要任何单机数据） |
| 5 安装 | **受阻：写入工具固定为参考设备（G6）**。`maint-session.py` 会在 `map` 步骤拒绝其他设备。绝不要强行绕过。 |
| 6 安装后检查 | 就绪（与参考设备相同的检查） |

如果某行写着受阻，先完成它之前的各节，然后把这个缺口连同 “已知缺口” 中的可选方案一起报告给机主。

## 1. 要求

### 1.1 主机：Apple 芯片的 Mac

运行 `bash tooling/repro/check-host.sh`。它只读取，不写入。每项要求输出一行：`ok`、`WARN`（未测试）、`FAIL`（此主机无法构建）或
`GAP`（只在本地存在的输入，见已知缺口）。只有在 `summary: 0 FAIL` 时才继续。

**结论：必须使用 Apple 芯片的 Mac（M1 或更新）。不支持 Intel Mac。** 证据取自参考构建主机（macOS 27.2.1 26B131，Xcode 27.2 27B52）：

| 组件 | 证据 | 结果 |
|---|---|---|
| Xcode 27 | 对 `Xcode.app/Contents/MacOS/Xcode`、工具链的 `clang` 和 `ld` 运行 `file`：`Mach-O 64-bit executable arm64`（没有 x86_64 切片） | 构建调用的编译器和链接器（`xcrun clang`、`xcrun ld`）无法在 Intel 上运行 |
| macOS 27 | `/usr/bin/kmutil`、`/usr/bin/compression_tool`、`/usr/bin/hdiutil`：只有 `arm64e` 和 `arm64e.x1` 切片 | macOS 27 本身只能在 Apple 芯片上运行。只有少数工具（`aea`、`codesign`、`newfs_apfs`）仍带 x86_64 切片 |
| 会运行自己编译产物的构建步骤 | `tooling/k27/build-piano-micns.sh` 用 `-arch arm64e` 编译 `plugin_host` 并运行它来加载和评测 CoreAudio 插件。`tooling/metal/check-turnip-a830-host.py` 运行 arm64e 的 Turnip 测试进程 | Intel CPU 无法执行 arm64e 代码（Rosetta 只把 x86_64 翻译到 arm64，不能反过来） |
| Homebrew 前缀 | `/opt/homebrew`（Apple 芯片的前缀）被写死在 `tooling/metal/build-turnip-a830.sh`、`tooling/kc/build-kc-k27.sh`、`tooling/k27/build-native-gop-display.sh`、`build-native-piano-touch.sh`、`tooling/persist/maint-session.py` 和 k27 内核脚本中 | Intel 的 Homebrew（`/usr/local`）找不到这些路径。部分有覆盖变量（`PYTHON315`、`LLVM_NM`、`LLVM_BIN`），但不是全部 |
| 不构成障碍 | `ld-classic`（ld64-957，内核的链接器）是通用二进制（x86_64 + arm64）。`fastboot` 是通用的。`ipsw` 有 x86_64 版本。不使用 `kmutil`：KC 由 k27 树的 Python 构建器链接。临时签名的 `codesign` 两种平台都可用 | - |

注意：只测试过 macOS 27.2.1 加 Xcode 27.2。Xcode 27 声明的最低系统是 macOS 26.6（`LSMinimumSystemVersion`），因此运行 macOS 26.6
或更新版本的 Apple 芯片 Mac 也许可行，但未测试（`check-host.sh` 会给出 WARN）。Intel 路线需要 macOS 26 上的 Xcode 26、在每个脚本中
改用 Intel 的 Homebrew 前缀，并去掉两个 arm64e 自测。这条路从未尝试过，不受支持。请在原生 arm64 shell 中运行所有命令，不要在
Rosetta 下运行（`check-host.sh` 会对被翻译的 shell 报 FAIL）。

### 1.2 主机软件

- **Xcode 27**（完整的应用，不只是命令行工具）：包含 `Kernel.framework` 的 macOS SDK 和 iphoneos SDK（k27 内核用
  `SDKROOT=iphoneos` 构建）。运行 `sudo xcode-select -s /Applications/Xcode.app`。
- **Homebrew**，位于 `/opt/homebrew`。安装：`python@3.15 llvm gnu-sed bison mtools dtc zstd`、`blacktop/tap/ipsw`（获取 AEA 密钥）
  以及 cask `android-platform-tools`（`fastboot`）。
- **python3 3.11 或更新版本**。只需要标准库：Turnip 构建会自行创建 venv，并使用固定版本的 meson、mako、PyYAML 和 packaging。
- **磁盘：** 约 90 GiB 可用空间。IPSW 输入最多 32 GB，根卷镜像 21 GB，SystemOS 窗口 6 GB，其余是构建和镜像。
- 在前台终端中使用 **sudo**。只有 `make-systemos.sh`（newfs_apfs、保留所有权的 ditto）和 `volume-manifest.py`（读取 0600 的 root 文件）需要它。
- 在前台终端会话（`open -a Terminal`）中运行与平板相关的步骤。后台进程可能因 macOS 的 “本地网络” 隐私限制而失去 USB 网络。

### 1.3 平板

- 小米平板 8 Pro **25091RP04C，512 GB**。工具中的分区布局是 512 GB 型号的（G10）。
- 原厂系统 **OS3.0.303.0.WPYCNXM**，位于 **slot A**，与 Ubuntu 项目的要求相同。
- 通过小米官方流程**解锁 Bootloader**。`fastboot getvar unlocked` 必须返回 `yes`。
- **先安装 Ubuntu**，使用 Ubuntu 项目自己的安装程序：
  [xiaomipad-8pro-mainline](https://github.com/yzddmr6/xiaomipad-8pro-mainline) 的 `docs/INSTALL.en.md`。该安装程序会：
  - 备份本机的每个分区和 GPT（请保留这份备份，它是回到出厂状态的途径）；
  - 创建 `linux_rootfs`（60 GiB）和 `linux_data`（约 201 GiB），这是 macOS 唯一使用的空间；
  - 收集本机自己的文件：`boot-profile.json`、你自己的 AVB 密钥 `boot-signing.pem`、触摸校准和传感器注册表；
  - 保留 Android 可启动（`userdata` 200 GiB），并提供 `tools/piano-switch.py` 和 slot A 的 Android 组件用于切换回去。
  本仓库从不重新分区，也从不触碰 Android 的分区。
- **数据线：** 一根 USB-C 数据线直接连接平板和 Mac，不要经过扩展坞。同一根线承载 fastboot、维护镜像的网络和 macOS 的 USB 控制台
  （`tooling/native/usb-console.py --list`）。
- 电量高于 50 %（`fastboot getvar battery-voltage`）。在本机验证 macOS 充电之前，Android 仍是充电的后备方案。
- **机主的退路：** 按住电源键 + 音量减 15-20 秒。由于 ABL 从不被写入，这总能进入 ABL fastboot。

## 2. 输入

### 2.1 本仓库及其固定版本的源码

```sh
git clone https://github.com/lenzhang/UniXNU-xiaomipad8pro.git UniXNU-piano && cd UniXNU-piano
```

`unixnu.lock` 按 URL 和提交固定其他所有检出（UniXNU、Mu-Silicium 及其子模块、piano-uefi、DarwinBoot、SoC 层）。把每个
`[pin."NAME"]` 按其 `commit` 克隆到同一个目录，并把 `UNIXNU_RESEARCH` 设为该目录。`tooling/unixnu.sh` 和
`tooling/firmware/build-fd.sh` 从那里读取。它们都是公开的；k27 树不是（G1）。

### 2.2 来自 Apple 的固定版本 IPSW，以及来自 Apple 的 AEA 密钥

```sh
python3 -I tooling/repro/fetch-ipsw.py --dry-run          # the plan, the tools, the disk space; no network, no writes
python3 -I tooling/repro/fetch-ipsw.py --remove-aea       # fetch, decrypt, decompress; checks every file against tooling/repro/pins.json
python3 -I tooling/repro/fetch-ipsw.py --check            # later: no network, everything still equal to its pin
```

它的步骤如下，每一步出错都会安全地停止：

1. 对固定的 URL（`UniversalMac_27.3_26C5035r_Restore.ipsw`，27206093542 字节）发 HEAD 请求。Apple 返回的
   `x-amz-meta-digest-sha256` 必须等于固定值 `1f372fa1...`。否则说明 CDN 提供的是另一个文件：停止。
2. `tooling/k273/ipsw-members.py fetch` 只下载 K114c 标识的 11 个组成文件（27 GB 中的 14 GB），支持断点续传，并校验每个文件的
   zip CRC-32。随后再次计算每个文件的哈希并与固定值比较。`BuildManifest.plist` 也一样（`58f18ab4...`）。
3. `tooling/k273/decrypt-aea.py` 处理两个 AEA 磁盘镜像。密钥通过 `ipsw fw aea --key` 从 Apple 的密钥服务获取（归档头部写明了密钥 URL）。
   由 macOS 的 `/usr/bin/aea` 解密。密钥从不写入磁盘或打印。解密后的 sha256：OS `40d8282e...`，SystemOS cryptex `14330ed9...`。
4. `tooling/k273/decompress-kernelcache.py` 生成 KC 基础文件 `kc/kc273-mac17j.bin`，sha256 `d1003b51...`（UniXNU
   macos-27.3-26C5035r 配置的固定值）。

通过标志：`PASS: every pinned Apple input is present and equal to its pin`。文件放在 `out/inputs/k273/26C5035r/` 和
`out/inputs/k273/kc/`（被 git 忽略；绝不要把它们复制到任何公开的地方）。

### 2.3 启动 DTB 和维护镜像（公开，来自 Ubuntu 项目）

下载 Ubuntu v1.1.0 发行包（`piano-ubuntu-v1.1.0-boot.zip`，用其 `SHA256SUMS.assets` 校验）。解压到一个目录，并把 `PIANO_V110` 设为该目录。
文件 `release/boot/boot-ram-maintenance.img` 的哈希必须是 `54001a50...`（`pins.json` 的 `maintenance_image`）。每个写入会话都运行在这个
只在内存中运行的 Linux 里（`maint-session.py maint-boot`）。它的 DTB 段就是每个镜像都携带的启动 DTB：

```sh
python3 -I tooling/repro/boot-dtb.py "$PIANO_V110/release/boot/boot-ram-maintenance.img"    # -> out/inputs/piano-boot.dtb, 50636eff...
```

### 2.4 单机数据：只能来自本机，绝不能来自其他设备

这些文件都不能放进任何仓库、issue 或聊天。请保存在 `private/`（被 git 忽略）或 `~/.config/piano/` 下。

| 数据 | 来源 | 获取方式 | 使用者 |
|---|---|---|---|
| 完整分区备份 + 每个 LUN 的 GPT | 本机 | Ubuntu INSTALL §2（在任何写入之前） | 回滚到出厂状态；`unit-profile.py` |
| AVB 密钥 `boot-signing.pem`、`boot-profile.json` | 在 Ubuntu INSTALL §3 生成 / 读取 | Ubuntu 的工具 | 为 `boot_a` 签名（G7） |
| slot A 的 Android 组件（原厂 `boot`、`init_boot`、`vendor_boot`） | 本机的备份 | Ubuntu INSTALL §10（`android-set`） | Android 后备方案（`piano-switch.py`） |
| 触摸校准 `thp-calibration.json` | 厂商的 `piano_nova_<panel>_thp_config.ini` | Ubuntu 的 `tools/extract-thp-calibration.py`；`PIANO_TOUCH_CALIBRATION`（`docs/drivers/touch.md`） | PianoTouch |
| 传感器注册表 -> 传感器包 | `persist`/厂商传感器文件，只读 | Ubuntu INSTALL §3 "prepare your sensor registry"，然后 `package/PianoSensors/tools/make-sensor-pack.py`（`docs/drivers/sensors.md` §5b） | PianoSensors（`PIANO_SENSOR_PACK`） |
| 扬声器 Re25 + 保护参数 | `persist` 的 `fsalgo_rdc.bin` + 原厂 ACDB | `tooling/audio/spk-prot-extract.py re25 / params`（`docs/drivers/audio.md` §12） | PianoAudio（`piano-audio-spk`，可选） |
| 蓝牙和 Wi-Fi 地址 | `persist` | 只读，从备份中读取 | 蓝牙、Wi-Fi |
| 原厂固件（DSP、GPU zap、Wi-Fi、蓝牙、触摸、功放预设） | OS3.0.303 的每台设备都相同；Wi-Fi 的 `board.bin` 因地区而异 | Ubuntu 项目的 `firmware/` 目录（按名称、大小、sha256 锁定）或本机自己的原厂分区 | PianoAudio、PianoVideo、Wi-Fi、GPU、蓝牙附加组件 |

绝不要以可写方式挂载 `persist`。绝不要使用其他设备的校准数据。

### 2.5 本机的分区布局是工具所知道的那种吗？

```sh
python3 -I tooling/repro/unit-profile.py --lun0 <LUN 0 MBR+primary GPT from the Ubuntu backup> --lun4 <LUN 4 likewise>
```

它读取两个文件，把 `linux_rootfs`、`linux_data`、LUN 0 大小和 `boot_a` 与 `tooling/persist/common.py` 比较。
`SAME-LAYOUT` 表示这些分区位于工具预期的位置。GPT 哈希仍然不同（GUID 每台设备不同），所以安装仍被 G6 阻挡。`DIFFERENT`（退出码 2）
表示本机完全无法使用这套工具的布局。无论哪种结果都要报告。

## 3. 构建镜像（只在 Mac 上；不触碰平板）

一个入口脚本按顺序运行所有步骤。先用 `--dry-run`，再正式运行，最后用 `--check`：

```sh
bash tooling/repro/fetch-and-build.sh --dry-run                 # every command it would run; nothing fetched, built or written
bash tooling/repro/fetch-and-build.sh                           # host inputs systemos root kernel fd image
bash tooling/repro/fetch-and-build.sh --check                   # each step's outputs against the pins (no network)
bash tooling/repro/fetch-and-build.sh root                      # or any single step
```

### 3.1 在本地重建根卷镜像（从不分发）

20.9 GB 的根卷镜像由 SHNGROOT 清单加两个窗口组成（`docs/k273-line.md` §3）：

| 部分 | 生成方式 | 检查 | 可复现吗？ |
|---|---|---|---|
| OS 窗口（14493417472 B） | IPSW 的 `141-22233-021.dmg`，解密后不作改动 | sha256 `40d8282e...`（固定值） | 可以，逐字节一致 |
| SystemOS 窗口（6442450944 B） | `tooling/k273/make-systemos.sh OS.dmg 141-22145-021.dmg 141-22106-021.dmg out/inputs/k273/root/systemos.img`（sudo）：SystemOS 和 App cryptex、Data 模板的只读部分、启动垫片（G1b） | `sudo python3 -I tooling/repro/volume-manifest.py --image out/inputs/k273/root/systemos.img --compare tooling/repro/pins.json`。文件级清单摘要必须是 `070934ff...`（27352 项） | **字节：不能；文件树：能** |
| 清单（块 0） | `tooling/native/make-root-manifest.py`，在 `make-root-image.sh` 内部调用 | `verify-root.py` 从两个窗口重新计算 | 给定两个窗口则可以 |
| `root.img`、`root.json` | `tooling/k273/make-root-image.sh out/inputs/k273/root` | `python3 -I tooling/repro/verify-root.py out/inputs/k273/root --manifest-json out/inputs/k273/root/systemos.manifest.json` | 跟随 SystemOS 窗口 |

为什么 SystemOS 的字节不同：`newfs_apfs` 会给每个新容器一个随机的卷 UUID，APFS 还会记录创建时间和分配顺序。两次运行的镜像
sha256 永远不同。2026-11-25，参考窗口（`a5346509...`，UUID 07AD53B3...）和用同一批固定组成文件新重建的窗口（`30818f46...`，另一个 UUID）
得到了相同的文件清单 `070934ff...`。该清单记录路径、类型、权限、所有者、标志（不含 UF_COMPRESSED）、大小、内容 sha256 和链接目标，
不包括时间和根目录的 `.fseventsd`。你的 SystemOS UUID 是你自己的。`make-root-image.sh` 把它写进 `root.json`，`build-k273-image.sh`
把它传给 ADT（`PIANO_SYSTEMOS_UUID`），所以它不需要与参考值相同。你的 `root.img` sha256 也是你自己的。`verify-root.py` 检查所有可以
检查的部分：镜像内的固定 OS 窗口、镜像内你的 SystemOS 窗口、重新计算的清单、布局、UUID。记下你的 `root.json` 中的 `image_sha256`：
安装步骤会把它作为 `--root-sha256` 使用。

`make-root-image.sh` 还会生成信任包（`make-trust.py`：基础信任包自己的模块（G2）+ IPSW 中的三个信任缓存，均有固定值），并复制 Data
种子（`K273_SEED`，G3）。缺少这两者时，入口脚本拒绝开始这一步。注意：`root.json` 的 `write` 块写的是参考设备 `linux_data` 的 LBA
（common.py），这也是 G6 的原因之一。

### 3.2 内核、固件、内核集合和 RAM 镜像

```sh
bash tooling/k273/prepare-k27-tree.sh                     # needs PIANO_K27_TREE (G1); prints the relinked kernel's path
bash tooling/firmware/build-fd.sh                          # the UEFI FD from the unixnu.lock checkouts; stamps ram-payload-first
K273_ROOT=k273 K273_ROOT_PART=linux_data PIANO_GPU=2 PIANO_DATA_PERSIST=0 PIANO_RESET_AFTER=900 \
  bash tooling/k273/build-k273-image.sh                    # KC (only this repository's kexts), ADT, out/ram and out/ram-dump
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
```

通过标志：KC 日志中的 `native kext rule: ... all this repository's`；`verify-ram-images.py` 退出码为 0；镜像不超过 250000000 字节
（ABL 的 256 MiB 内核区域）。`PIANO_DATA_PERSIST=0` 让 Data 为易失的，因此测试镜像永远不会打开 Data 存储。`PIANO_RESET_AFTER=900`
在 900 秒后让平板回到 ABL fastboot。`K273_ROOT_PART=linux_data` 是第二台设备上根卷的第一个位置（Ubuntu 的根卷仍留在 `linux_rootfs`）。

## 4. 先做 RAM 启动验证（只用 `fastboot boot`；不写入任何东西）

RAM 启动把镜像载入内存，不写任何分区。在任何写入之前，按顺序完成下面的 RAM 步骤。

1. ABL，只读：`python3 tooling/persist/maint-session.py abl-check`。通过条件：产品 `piano`，已解锁 `yes`，current-slot `a`，
   slot A 可启动，电量正常。
2. 开始控制台抓取：`python3 tooling/native/usb-console.py --capture 960`。同一时间只能有一个抓取占用该端口。
3. `fastboot boot out/ram/boot-piano-uefi-ram-A-bundled.img`，然后等待。在磁盘上还没有根卷时，启动必须走到：
   - UEFI：`UNIXNU: config: volume VenHw(A9C6827C-...)`：镜像自带的载荷，而不是 ESP；
   - DarwinBoot -> XNU：`NativeRamBlock: RAM window ...` 这一行；
   - `NativeUfs: start ... root-part=linux_data` 和分区扫描，然后等待根卷（`linux_data` 上还没有 SHNGROOT：属于预期）；
   - USB 控制台有响应（`usb-console.py` 显示内核日志）；
   - 通过 `piano-reset-after` 回到 ABL fastboot（或在控制台发送 `reset fastboot`）。
   panic、没有复位的卡死或 `Load Error` 都算失败。各种现象的处理见 `docs/bringup-debugging.md` §1，邮箱日志见
   `tooling/native/native-loop.sh` 的转储镜像。
4. 维护镜像，只读：`maint-session.py --dry-run maint-boot map maint-exit`（逐行阅读），然后去掉 `--dry-run` 再运行。在第二台设备上，
   `map` 会在 GPT 和启动分区检查处停下（G6）。这个停止就是预期结果；它证明保护机制有效。记录它的输出。

**在任何写入之前，以下全部必须通过：** 第 1-3 节（`check-host.sh` 0 FAIL、`fetch-ipsw.py --check` PASS、带清单的 `verify-root.py` PASS、
`verify-ram-images.py` 0）、上面的 RAM 启动、本机自己的完整备份（Ubuntu INSTALL §2）存在且哈希已校验、G6 已解决，以及机主在场。

完整桌面资格测试需要磁盘上有根卷，因此在第一次写入（5.2）之后进行：同一个镜像从 `linux_data` 启动到桌面，Data 为易失的。
在写入其他任何东西之前，检查登录窗口、触摸、键盘保护套、USB 网络/ssh、Wi-Fi 扫描和电池读数。

## 5. 安装（只能通过 `tooling/persist/maint-session.py`，只能在 G6 之后）

### 5.1 规则：永不违反，没有例外

- **绝不写入** `xbl`、`abl`、`tz`、`hyp`、`vbmeta`、`dtbo`、`persist`、任何固件/基带/efs/校准分区，或**任何 `_b` 分区**。
  绝不写入 `userdata`、`super`、`metadata`、`vendor_boot_a`、`init_boot_a`。它们只会被读取和备份。
- **绝不**使用 `fastboot flash`、`fastboot erase`、`fastboot --set-active` / `set_active`、任何 `fastboot oem`、`flash_all_lock`。
  写入只能由 `maint-session.py` 在维护 RAM 镜像中用 `dd` 完成，别无他法。
- **先备份：** 每次写入之前，`backup` 步骤通过同一个连接把它要写的每个区域导出到 `PIANO_BACKUP_DIR`（在 `~/.config/piano/env` 中设置）。
  三次 sha256 校验：平板自己计算的区域哈希、收到的字节、以及绕过页缓存读回的保存副本。备份缺失或不一致都会停止该步骤。
- **每次写入后回读：** 在丢弃缓存后从介质读回该区域，其 sha256 必须等于镜像的哈希。任何不一致都会停止会话。
- 每次都先 `--dry-run`。阅读它打印的每条命令。同一时间只进行一个平板会话。绝不要中断正在进行的写入。
- 只写 `docs/persistent-boot.md` §1 和 §13.8 列出的区域：LUN 0 的 GPT（一个新分区 37 `esp`，从 `linux_rootfs` 末尾切出）、ESP、
  `boot_a`、根卷镜像、Data 存储区域。在第二台设备上，这也意味着 Ubuntu 时期 `linux_rootfs`/`linux_data` 的内容；如果其中有你想保留的
  东西，请先自行备份（`linux-data-root` 不会保留 `linux_data` 原有内容的副本）。

### 5.2 各个会话（参考设备的顺序，`docs/persistent-boot.md` §5、§12、§13.8）

`P=tooling/persist/maint-session.py`。每次都先加 `--dry-run`：

0. 先离线构建 ESP：`tooling/k273/build-k273-esp.py --root-part linux_data ...`（`docs/persistent-boot.md` §13.8；它的记录写明根卷分区、
   Data 存储所在的分区和 LBA，以及种子）。下面每个会话都把它作为 `--esp` 传入，因为 `map` 会按这个记录检查磁盘。`--dry-run` 会列出
   某一步还需要的其他东西。
1. 把根卷镜像写到 `linux_data`（Ubuntu 的根卷保留）：`python3 $P maint-boot map linux-data-root check maint-exit --esp <esp>.img
   --root-part linux_data --root-image out/inputs/k273/root/root.img --root-sha256 <root.json image_sha256>`。该步骤先在主机上检查镜像的
   哈希，写入，再按镜像长度读回 `linux_data`。然后进行完整桌面的 RAM 资格测试（第 4 节最后一段）。
2. ESP 和 GPT：`python3 $P maint-boot map backup gpt esp check maint-exit --esp <esp>-diag.img`，然后在不写入任何东西的情况下对刷写路径
   做 RAM 测试：`ram-esp-test`、`ram-v4-signed`、`ram-precedence`（§5）。测试阶段 ESP 的默认启动项会自行复位到 ABL fastboot（`-diag`）；
   日常用的 ESP 由 `esp-final` 写入。
3. `boot_a`：用你自己的 AVB 密钥为启动镜像签名（G7；`tooling/persist/build-boot.py`、`sign.sh`），然后 `python3 $P
   maint-boot map backup boot-a esp-final check maint-exit --esp <esp>-diag.img --esp-final <esp>.img`，再运行 `abl-final reboot`。
   之后 slot A 会启动 macOS。最后一项测试：关机、拔线、开机。
4. 如果 `abl-check` 显示 `slot-successful:a: no`，运行 `slot-success-crc`。它只写 `boot_a` 在 LUN 4 中条目的第 54 位和两个 CRC，
   并在之前备份两份副本。绝不要使用 `set_active`。

ESP 和根卷镜像必须一致：ESP 的参数写明了根卷分区（`native-ufs-root-part`）以及 Data 存储所在的分区和 LBA；如果 ESP 中的存储不是磁盘上的
那个，`map`/`esp-final` 会拒绝（`docs/data-store.md`）。

### 5.3 回滚，从影响最小的开始（`docs/persistent-boot.md` §7，模式见 `docs/install-esp-v3.md` §5）

1. **卡死或 panic：** 启动保护（`piano-boot-guard=180`，`docs/boot-guard.md`）会回到 ABL fastboot。否则按住电源键 + 音量减 15-20 秒。
   在 ABL 中，所有 RAM 镜像都能像以前一样启动。RAM 镜像自带的载荷优先于 ESP（`ram-precedence`）。
2. **撤销某一步：** `python3 $P maint-boot`，然后 `python3 $P rollback STEP`，再 `maint-exit`。每次回滚都会把该步骤经过校验的备份写回并读回检查。
   完整撤销顺序：`boot-a`、`esp`、`store`、`gpt`。
3. **Android（充电的后备方案）：** 在 ABL 中，用 Ubuntu 发行版的 `piano-switch.py` 加本机自己的 `android-set` 把 slot A 的 Android 组件写回。
   `userdata`、`super` 和 `metadata` 从未被触碰。用同样的方式加 macOS 组件切回 macOS，然后运行 `slot-success-crc`
   （`docs/persistent-boot.md` §7，Android 一行）。
4. **出厂状态：** 通过维护镜像写回 Ubuntu INSTALL §2 中本机自己的完整备份。绝不要使用 `flash_all_lock`。

## 6. 安装后检查

在最初几次从磁盘（而不是 RAM 镜像）启动时，逐项记录，并附上对应的控制台或 shell 输出行：

- `piano-boot-guard: healthy after 180 s`，重启原因正常（`docs/boot-guard.md` §5）；
- Data 存储的状态行（新存储为 `NativeRamBlock: Data store state 3`，之后为 `state 2`）。遇到 `state 7`、`kept` 或 `started over`
  时**停止并回滚**（`docs/data-store.md` §3）；
- 根卷来自预期的分区（`NativeUfs ... root-part=...`，窗口已发布，根卷已挂载）；
- 通过 USB shell 或 ssh：`sw_vers` 显示 27.3 26C5035r，`sysctl hw.model` 显示 Mac17,11；
- 使用补丁 0076 时，`sysctl -n kern.piano_bootargs` 显示合并后的完整启动参数。检查配置中的预期参数，包括旧接口 `kern.bootargs`
  在 1024 字节处截断之后的参数。更新后的辅助程序读取完整参数。如果配置启用了 USB 调试 shell，确认它已启动。
- 登录窗口，然后是桌面：触摸、键盘保护套、Wi-Fi 连接、声音、亮度；
- 电池电量百分比，以及用 USB-PD 充电器充电（在看到这一点之前，Android 仍是后备方案）；
- 在 ABL 中前后各运行一次 `fastboot getvar battery-voltage`；
- 冷启动（关机、拔线、开机）能再次启动 macOS；Android 仍能来回切换（5.3 第 3 步）。

## 已知缺口（只在本地存在的输入）以及给机主的可选方案

以下内容都不能公开。每一项都由原始环境的机主决定。本指南从不绕过其中任何一项。

| # | 缺口 | 为什么只在本地 | 可选方案 |
|---|---|---|---|
| G1 | **k27 树**（`PIANO_K27_TREE`，`unixnu.lock` 固定值 `k27` 11ac316b）：xnu-12377.121.6 + 基线补丁 0001-0052、DroidVM 引导部分（fakeroot 头文件、firehose、ArmDTPlatform/ArmCpu 目标文件）、KC 构建器 `k27/kc-build273-piano.py`、27.0.1 的 KC 基础文件、`ld-classic`（来自较旧命令行工具的 ld64-957）及其库 | DroidVM 内容不公开（项目规则），部分内容来自 Apple 的 KDK 和工具链下载 | (a) 一个从公开来源重建该树的脚本：Apple 的 xnu-12377.121.6 标签 + 以补丁形式提供的补丁系列 + 用户自己从 Apple 开发者网站下载的 KDK 和命令行工具，并按文件哈希校验；需要机主决定是否公开 0001-0052 和引导部分；(b) 该树的哈希清单，让持有者能校验副本；(c) 机主私下把树提供给指定的构建者 |
| G1b | SystemOS 启动垫片（`overlay/systemos-shims/sbin/fsck`、`mount`：两个分别为 17 和 358 字节的 shell 脚本） | 属于 k27 树 | 在本仓库中自行重写垫片，并在 `make-systemos.sh` 中加入 `SYSTEMOS_SHIMS` 开关；这会改变固定清单的 `sbin` 组，因此需要新的固定值和一次平板验证（已安装的版本保留其窗口） |
| G2 | 基础信任包 `out/inputs/kernel.macho.trustcache`（`2b87b224...`），`make-trust.py` 会保留其中自己的模块 | 由 k27 树的 `scripts/make-trust-bundle.py` 从暂存的代码生成 | 从 k27 树重新生成（需要 G1）；或者让 `make-trust.py` 从零个自有模块开始，由暂存工具加入它们自己的模块 |
| G3 | Data 种子（`out/inputs/scratch-data.img` `cb50ceac...`；日常种子） | 包含机主的账户和数据 | 从 IPSW 的 Data 模板生成新种子（k27 树的 `make-macos-data-seed.sh`、UniXNU 的 `seed-data-skip-setup.py`），在首次启动时创建用户自己的账户。需要构建并做一次 RAM 测试 |
| G4 | Turnip Mesa 树（`TURNIP_TREE`，UniXNU-turnip）及其暂存的 Metal 插件（`out/turnip-a830`） | 不公开 | 以补丁形式在固定的上游 Mesa 提交（MIT）上发布改动，由机主决定。没有它就没有 A830 桌面 |
| G5 | 单机数据（2.4）和 MiPPS 密钥文件 `private/charging/mipps-keys.bin` | 每台设备不同，或属于第三方协议资料 | 单机数据：在每台设备上用 Ubuntu 的工具和我们的工具提取（2.4）。MiPPS：从不分发；`PIANO_POWER_MIPPS=0` 可在没有它的情况下构建（电荷泵默认本就关闭） |
| G6 | **写入工具固定为参考设备**：`tooling/persist/common.py`（GPT sha256、LBA、启动分区哈希、`CRITICAL-BACKUP` 路径）、`build-gpt.py`（来自该设备的分区表）、`make-root-image.sh` 的写入计划 | 它是为一台平板写的 | 从本机自己的备份生成设备配置 JSON（`tooling/repro/unit-profile.py` 是只读的起点），由 `common.py` 读取以替代其常量，并对每个配置运行 `test-persist.py` 的模拟平板会话。在此之前，第二台设备上不做任何写入 |
| G7 | `boot_a` 签名：`tooling/bootimg/sign-on-remote.sh` 在一台远程主机上用参考设备的密钥签名 | 私钥从不离开那台主机 | 用本地 `avbtool` 路径加本机自己的 `boot-signing.pem`（Ubuntu INSTALL §3），使用相同的尾部参数（`boot-profile.json`） |
| G8 | 日常配方：精确参数配置、用户层信任缓存、用户层和机主日常镜像使用的固件附加组件 | 记录在私有笔记中 | 在 `configs/` 中提供公开的日常配置（不含私有值的参数），并用 `tooling/userland/` 构建用户层 |
| G9 | `build-fd.sh` 生成的 `out/fd` 不是逐位可复现的（两次干净构建结果不同） | EDK II 构建时间戳 | 只按哈希识别（与现在相同）；可选：固定 EDK II 的可复现构建选项 |
| G10 | 256 GB 设备：`linux_data` 约 61 GiB，其他 LBA 也不同 | 工具只知道 512 GB 的布局 | 由 G6 的设备配置覆盖，并使用更小的 Data 存储 |
| G11 | 其他原厂版本和地区（Wi-Fi `board.bin` 因地区而异） | 锁定文件固定为 OS3.0.303.0.WPYCNXM 国行 | 像 Ubuntu 项目那样，为每个版本/地区建立新的锁定文件 |

## 本工具包的文件

- `tooling/repro/pins.json`：所有固定值（IPSW、组成文件、解密后的镜像、KC、根卷布局、SystemOS 清单、维护镜像、DTB）。
- `tooling/repro/check-host.sh`：要求（1）。
- `tooling/repro/fetch-ipsw.py`：Apple 输入（2.2）。
- `tooling/repro/boot-dtb.py`：启动 DTB（2.3）。
- `tooling/repro/unit-profile.py`：本机布局与工具布局的比较（2.5）。
- `tooling/repro/volume-manifest.py`、`verify-root.py`：根卷检查（3.1）。
- `tooling/repro/fetch-and-build.sh`：统一入口（3）。
- `tooling/repro/test-repro.py`：主机测试，不联网，不连平板。
