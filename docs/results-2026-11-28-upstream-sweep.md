# 2026-11-28 上游巡查：piano 仓库没有新提交，动的是隔壁三条线

只读巡查，未改任何代码，`unixnu.lock` / `feeds.toml` 一个字没动，装机集（`boot_a` `df6d5a18…`、ESP `44de650a…`、
Data seed `b2583ea5`）也没动。时间全部写成北京时间（GitHub API 返回 UTC，已换算）。

## 1. 作者仓库 lenzhang/UniXNU-xiaomipad8pro：没有更新

- `main` 仍是 `751a0a12`（2026-11-27 10:30:25 +0800，"docs: record installed v13.1y12u daily line, 5 V charging and closed BH-2 root cause"），
  `git rev-list HEAD..upstream/main` = 0 —— 这个提交及它以前的全部历史都在我们本地。
- `README.md` / `README.zh-CN.md` / `skills/install-piano-macos/` 与本地逐字节一致（`git diff upstream/main HEAD` 对这几个路径为空）。
- 议题区没有任何 open 的 issue/PR；`#1`–`#5` 全部 closed（11-26），其中 `#2`–`#5` 是 CI 打包草稿与三个 Actions 版本升级，
  `#1` 是"用 RAM 镜像在第二台 piano 上验证原生启动（不写分区）"。
- 结论：没有需要合并的上游提交，也不需要重新锁版本。

## 2. fork 网络：今天有两个 fork 推过，但都不是可用的新实现

| fork | 最后推送（北京） | 相对 `751a0a12` | 内容 |
| --- | --- | --- | --- |
| `liao2333-me/UniXNU-xiaomipad8pro` | 11-28 14:00 | ahead 5 / behind 0 | 5 个提交只碰 `tooling/ci/k27-kernel.sh` 和 `.github/workflows/build.yml` |
| `SorasakiHinaHusband/UniXNU-xiaomipad8pro` | 11-28 15:29 | diverged（ahead 1 / behind 1089） | 只有那条 CI 打包草稿分支 |
| `alghiffaryfa19/…`、`shmbpp201205/…` | 11-25 / 11-27 | 未推进 | |
`liao2333-me` 那 5 个提交里唯一有实质内容的地方，是把 CI 里 KDK 的挂载改得更耐操：`hdiutil attach` 失败会明确报错；
`.pkg` 不再假定在卷根（`find -maxdepth 2 -name '*.pkg'`）；解包后的 payload 不再只认 `…/KDKs/*.kdk`，改成锚在
`*/System/Library/Frameworks/Kernel.framework` 上（两种 KDK 布局都含它）。这条我们自己踩得到：我们在 Mac 上手工
stage KDK 走的是同一份逻辑，以后换 KDK 版本时值得把这几行合进 `tooling/ci/k27-kernel.sh`。除此之外没有新驱动、新配置或新镜像。

## 3. 我们 UniXNU 核心的血统在 CrossDarwin/UniXNU_macOS，它也没动；忙的是另一条线

- `unixnu.lock` 的 `pin."UniXNU"` = `d830d23b`。它是 `CrossDarwin/UniXNU_macOS` `main`（`392a41a2`，11-10 06:57
  "fix(macos): mount the application cryptex for Safari"）之后 6 个提交的子嗣（compare ahead 6 / behind 0），
  所以我们的核心上游是 CrossDarwin 这条 macOS 分支，`main` 自 11-10 起没动。它的默认分支 `test_27`（11-14 起）与
  `UniXNU/UniXNU` `main` 又是另外两条线。
- `UniXNU/UniXNU` `main`（`fdf81eaa`，11-28 03:10）和我们的 pin **分叉**：它多 649 个提交、我们多 331 个 —— 那是 sheng 板的
  macOS 线，不能整批 bump。它 11-27 并了五个 PR，值得当参考读：
  - `#317` 去掉 iOS 专用的全局代码签名强制选项；`#318` 带上 `AppleCredentialManager` 并说明每用户 AKS 语义；
  - `#328` 拒绝 `wifip2pd` 以解决 30 s 睡眠超时（我们仓库里 `wifip2pd` 零命中，睡眠验收卡住时可以照这个思路试）；
  - `#319` 向 `corebrightnessd` 上报物理平台以打开背光控制 —— 我们已经走在前面：`PianoBacklight` +
    `piano-brightness-sync` 在做自动亮度与用户滑动同步（`docs/drivers/backlight.md`），不需要这个补丁；
  - `#324` FairPlay 本地包装 selector 0x899（SoftwareAES）+ 预链接 `FairPlayIOKit`。
## 4. k27 树源头 Droid-VM/macOS-DroidVM：pin 落后 85 个提交，native 那条分支仍然私有

- `pin."k27"` = `abcbc18f`；`main` 已经到 `524d27bf`（11-16 23:31），compare ahead 85 / behind 0。另有两个分支：
  `macOS` = `abe33e0f`（11-20 19:40，比 `main` 再前 6 个提交）、`wip/hackintosh` = `f7d070fe`（11-16 09:57）。
- 原生 k27 那个提交 `11ac316b789d81fc9f734e15645456f536c05141` 仍然取不到：对该仓库按哈希查询返回 404，
  `git ls-remote` 里也没有 `el1/native-variant`。维护者的原生分支照旧没公开。
- 这 85 个提交里和我们待办重叠的几条（都还是 DroidVM 虚拟机环境里的结论，不是原生硬件）：
  `976433e7` 把 `com.apple.driver.driverkit.serial` 留在 KC 里（网络不再在开机时等 240 s）—— 我们已经有这一行，
  `patches/k27-tree/0001…patch:168`；`d7edf6aa` Storage 面板认 MMIO Data store（`NativeRamBlockPartition`）；
  `c56d69fc` GPU bundle 允许在沙盒进程里映射（WebKit 的 GPU 进程拿到 Metal）；`ccd1a6a4` 向 Metal 应用暴露 XR10 格式；
  `31c356d8`、`3b20511d` 把 virtio 声音的完成改为电平中断；`b29557da` 光标走 virtio GPU cursor plane；
  `cfde579d`、`cf924b27` 藏 `hv_vmm_present` 让 Weather 的云能画。
- 处理方式：只登记，不动 pin。原生线靠的是我们 `patches/k27-tree` + `patches/xnu-12377` 那一套，中途换基线会把已经
  验收过的装机集变成不可比。

## 5. 本次巡查之后本地状态

- 没有新依赖要拉：`IssacL04/UniXNU-sm8750-common` 的 `main` 与 pin `feaa135d` 相同；`piano-uefi` 的 `unixnu` 分支与
  pin `31545f93` 相同；`xiaomi-pad-8-pro-development/android_device_xiaomi_piano-unixnu` 的 `main` 走到 `d4f160c8`
  （pin 是 `377ad539`），但那是社区 iPadOS 设备仓库，我们只当参考。
- `origin/main` = 本地 `HEAD` = `2639a365`，`git rev-list origin/main..HEAD` = 0。
- 待办顺序不变：先收口视频那条结论（`piano-play-probe` 的 `frame0` 行 + 播放时 `VTDecoderXPCService` 的 CPU 采样），
  再让 A/B 的 bit 54 长期置位，然后回到 v2 日用清单。