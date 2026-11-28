# 2026-11-28 上游第二轮扫描：piano 仓一个提交都没加，动的是依赖侧那一整批 PR

扫描时间 2026-11-28 17:38~17:47（本地）。只读操作：`git fetch upstream`、`git ls-remote`、GitHub REST。
本轮**没有** bump 任何 pin，**没有**改构建，**没有**动装机集（`boot_a` `df6d5a18…`、ESP `44de650a…`（ul11）、
Data seed `b2583ea5` 都不变）。

## 0. 先说结论

1. `lenzhang/UniXNU-xiaomipad8pro` `main` 仍是 `751a0a12`（2026-11-27 10:30:25 +0800），
   `git merge-base --is-ancestor 751a0a12 HEAD` 成立（退出码 0）。也就是说上游 piano 仓自昨天第一轮扫描
   （`docs/results-2026-11-28-upstream-sweep.md`）之后**没有任何新提交**，我们的 `HEAD`（`117c761d`）已经把它全量吃掉。
   open issue / open PR 都是 0：`#1`…`#5` 全 closed，其中 `#2`/`#3`/`#4` 是 dependabot 的 actions 版本升级，
   `#1` 是"给第二台 piano 出不写分区的 RAM 镜像"（3 条评论），`#5` 是 upload-artifact 升级。
2. 真正动的是**依赖侧**：`UniXNU` 组织在北京 11-28 02:29~03:10 合了一批 PR，其中 5 条正好落在
   `profiles/os/macos-27.3-26C5035r`（就是我们装机的这个 profile）。这 5 条里有 4 条压在我们今天还开着的
   未完项上：`watchdogd` 的 `hv_vmm_present`、`deny-exec` 名单（cryptexd/wifip2pd）、iOS 全局代码签名强制、
   亮度（corebrightnessd）。
3. 结论的可用性判断放在第 4 节，逐条写"上游改了什么、我们拿的代价、风险"。**未在本机验证**的部分都标了。

## 1. piano 仓自身的证据

```
git fetch upstream --prune            # 只删了 3 个 dependabot 分支和 refs/pull/{2,3,4,5}/head 的镜像 ref
git ls-remote upstream                # HEAD == refs/heads/main == 751a0a12a6169c1af1d1d9f3e2b7ae2a8bd004cb
git merge-base --is-ancestor 751a0a12 HEAD   # 0 = 是祖先
```

`/tmp` 之外没有新分支；`refs/pull/*/head` 那四个还是昨天看过的那四个（`7228bb95`/`345bef08`/`8edf2c9d`/`e4234ef4`）。

## 2. 27 个 pin 与上游 tip 的对照（`git ls-remote <url> HEAD`，17:41）

标 "近期动过" 的判据是 GitHub 给的 `pushed_at`（UTC）晚于 11-26；其余是"tip 与 pin 不同但近期无人推送"，
其中按分支记录的 pin（`piano-uefi` 的 `unixnu` 分支、`piano-unixnu`、`Project-SunUEFI` 这类只做参考的）
与默认分支 tip 天然不同，不算漂移。

| pin | 上游 | 我们的 pin | tip | 差 | 近期动过 |
| --- | --- | --- | --- | --- | --- |
| `UniXNU` | `UniXNU/UniXNU` | `d830d23b` | `fdf81eaa` | 11-25 之后 24 个提交 | 是（11-28 03:10） |
| `AdrenoMetal` | `UniXNU/AdrenoMetal` | `c68f9a1d` | `85832c3a` | ahead 205 / behind 52 | 是（11-27 18:29 UTC） |
| `Ath12kWifi` | `UniXNU/Ath12kWifi` | `48e23bf1` | `61c3fac1` | ahead 16 / behind 0 | 是（11-28 03:05） |
| `ShengUfs` | `UniXNU/ShengUfs` | `b11de87c` | `02a67a79` | ahead 12 / behind 0 | 是（11-28 02:29） |
| `QcaBluetooth` | `UniXNU/QcaBluetooth` | `b2fbab61` | `a4740937` | ahead 15 / behind 0 | 是（11-28 02:29） |
| `AppleKeyStoreState` | `UniXNU/AppleKeyStoreState` | `58e5461e` | `309cf585` | ahead 4 / behind 0 | 是（11-28 02:29） |
| `XnuPrelink` | `UniXNU/XnuPrelink` | `8f4f3866` | `34c80648` | ahead 3 / behind 0 | 是（11-28 02:30） |
| `GopFramebuffer` | `UniXNU/GopFramebuffer` | `d711e8c9` | `0fcda4f1` | ahead 4 / behind 0 | 是（11-28 02:29） |
| `UniXNU-sheng` | `UniXNU/UniXNU-sheng` | `00ba9098` | `993f6bdd` | ahead 819 / behind 559 | 是（11-28 03:10） |
| `UniXNU-sm8750-common` | `IssacL04/…` | `feaa135d` | `feaa135d` | 相同 | 否 |
| `ArmDTPlatform` | `UniXNU/ArmDTPlatform` | `e8bd8d5d` | `e8bd8d5d` | 相同 | 否 |
| `Adreno830` / `AdrenoPM4` / `AdrenoFirmware` | `Shad0w23333/…`、`UniXNU/…` | 见 lock | 同 pin | 相同 | 否 |
| `UniXNU-Pagani_SM8750` | `Shad0w23333/…` | `25b11097` | `25b11097` | 相同 | 否 |
| 其余（`Mu-Silicium`、`Device-Binaries`、`piano-uefi`、`uefi-sheng`、`DarwinBoot`、`ArmCpu`、`RamBlock`、
`ArmSmmu4K`、`ShengNovatekTouch`、`QcomDwc3`、`piano-unixnu`、`Project-SunUEFI`） | — | 见 lock | 各自 tip | 有意冻结的差 | 否（最后一次 push 都在 11-18 及更早，除 `ShengNovatekTouch` 11-18、`ArmCpu`/`QcomDwc3`/`uefi-sheng` 11-17、`RamBlock` 11-12、`DarwinBoot` 11-10） |

`pin."k27"` 还是老样子：`abcbc18f`，`local` 那一格 `11ac316b…` 在 `Droid-VM/macOS-DroidVM` 上仍然取不到（`main` =
`524d27bf`、`macOS` = `abe33e0f`、`wip/hackintosh` = `f7d070fe`，和昨天一致），`el1/native-variant` 也没公开。

## 3. `UniXNU/UniXNU` 那批 PR 逐条（都是往 `profiles/os/macos-27.3-26C5035r` 里加东西）

下面每条给了合并提交和原始提交；"改动位置"是补丁在 UniXNU 核心树里的落点。全部**维护者/贡献者报告**，
本机上还没有任何一条被我们构建或跑过。

1. **#317 去掉 iOS 的全局代码签名强制**（合并 `4d3a2b38`，原始 `8f689ff5`，作者 withered，11-21 写、11-28 02:29 合）。
   新增 `patches/xnu-12377/macos-disable-ios-cs-enforcement.patch`：从 `config/MASTER.arm64.bcm2837` 的
   `KERNEL_BASE` 去掉 `config_enforce_signed_code`、从 `MACH_BASE` 去掉 `config_library_validation`，
   让 `cs_require_lv()` / `cs_process_enforcement()` / `csproc_disable_enforcement()` 回到 Apple
   `MASTER.arm64.MacOSX` 的逐进程语义（ad-hoc 签名的 macOS app 加载自带的 ad-hoc framework 不再被强制库校验）。
   只作用于 macOS profile，iPadOS 不受影响。series 里插在 `macos-machine-config.patch` 之后。
2. **#318 ACM 进 roots + 逐用户 AKS 语义**（合并 `c091fa6f`，原始 `ac1e68e2`）。`profile.toml` 的 `roots`
   加入 `com.apple.driver.AppleCredentialManager`，并把 `!com.apple.driver.AppleSEPManager`、
   `!com.apple.driver.IOSlaveProcessor` 排除；配套文档写明：macOS 图形登录（coreauthd/coreautha、系统设置）
   要 `ACMContextCreate` 和用户客户端凭据上下文，没有 SEP 硬件的板子用 **`acm_fastsim=1`** 选 Apple 自带的软件
   命令分发器，这不提供 SEP 密钥。同一批把 `AppleKeyStoreState` 的 `os_abi` 加上 `macos-27.3-26C5035r`，
   并写明 macOS 上设备锁状态停在 0（解锁），逐用户会话句柄（< -3，例如 -501）返回 `kIOReturnNotFound`，
   使 `MobileKeyBag` 报 `NotFound`(-8) 而不是"空口令/缺逐用户 SEP keybag"。
3. **#319 亮度走物理 BLControl**（合并 `971e946f`，原始 `dec0ac6a`，11-22 写、11-28 02:31 合）。
   新增 `macos-corebrightness-physical-blcontrol.patch`：`bsd/kern/hvg_sysctl.c` 的 `sysctl_vmm_present`
   里，当 `proc_selfname()` 恰为 `corebrightnessd` 时把 `vmm_present` 置 0。理由写得很清楚：
   `CoreBrightness -[BrightnessSystemInternal init]` 走 `CBU_IsVirtualMachine()` → `MGGetBoolAnswer("IsVirtualDevice")`
   → 读 `kern.hv_vmm_present`，为真就进 VMBLControl(mode 3) 丢掉所有亮度请求（`DisplayServicesCanChangeBrightness`
   返回 false）。他们说配合板子的背光驱动发布 `NativeSandboxGrantProcess`（给 WindowServer/corebrightnessd）
   加 1 字节 OSData 的 force-brightness-control，macOS 27.3 就能进 mode 1，且不用往用户态注入 dylib。
4. **#324 预链 FairPlayIOKit + 用 `vmm-hide` 按进程屏蔽 `hv_vmm_present`**（合并 `c385e25e`，原始 `cf924b27`，11-26 19:48 写、11-28 02:31 合）。
   两件事：`profile.toml` 的 `roots` 加 `com.apple.driver.FairPlayIOKit`（给 adid/fairplayd/fairplaydeviceidentityd
   在 Anisette 置备和 FairPlay 本地包裹时用）；新增 `macos-corebrightness` 的通用版
   `macos-vmm-present-proc-filter.patch`（95 行）：新增 boot-arg **`vmm-hide=<逗号分隔进程名>`**，
   在 `sysctl_vmm_present` 里拿 `req->p` 的 `proc_best_name`（32 字节全等）匹配，命中就把
   `kern.hv_vmm_present` 报 0 并累加 `native_vmm_hide_hits`；设备树 `/defaults/vmm-present` 保持 1，
   AMFI 直接读设备树所以不受影响。boot-arg 用 `PE_parse_boot_arg_str` 读一次，三态原子缓存（0 未读/1 正在读/2 没有/3 有），
   并发下还有个 256 字节的 fallback 缓冲。作者配了主机测试 `tests/macos/test_macos27_fairplay.py`
   （查预链根、查 series，并把那个列表匹配器在 ASan+UBSan 下跑）。**注意他们的设计注释明确说"不打扰 watchdogd"**，
   也就是说这条机制我们是反着用（把 watchdogd 放进名单让它走安静出口）。
5. **#328 `deny-exec` 名单加 wifip2pd**（合并 `9587d87e`，原始 `07f09162`）。改的是
   `macos-droidvm-k27-parity.patch` 里那一行 `TUNABLE_STR(native_deny_exec, …)`，默认值从
   `mobileactivationd,cryptexd` 变成 `mobileactivationd,cryptexd,wifip2pd`，治的是"30 秒睡眠超时"。
6. 同一批收尾：`8e2d85fe`（Pin merged component revisions for 2026-11-28 macOS PR batch）、
   `f5c9e727`（对齐 components 目录 pin、给 `FairPlayDiagExport.h` 记哈希、更新 SoftwareAES 测试）、
   `3767655c`/`fdf81eaa`（`devices.toml` 里 sheng 版本号）。
7. 11-25 那批 GPU 的（`241dadcd` 拒绝混用 Adreno 内核与运行时 ABI、`fce0acbe`/`d73e814c` metal2vulkan fork pin、
   `2fb08828` turnip 原生能力 + 中断驱动的 timeline 等待、`b04f131d` Adreno740 空地址空间 map 失败修复）
   走的是 sheng/A740 那条线，对我们的 A830 只有参考价值。另外 11-25 还加了一个可选的 `boot_menu_theme`
   （oc-like 菜单主题），纯外观。

组件小仓本轮值得单独看的三条：

- `UniXNU/AdrenoMetal`：`e0d8c3c9` `fix(frontend): answer 16-byte IOSurface row alignment on runtime v2`（11-26）。
  我们这两天正好在 IOSurface 后端 `CVPixelBuffer` 上栽过（不 lock 就没基地址），行对齐这条要对着读。
- `UniXNU/Ath12kWifi`：11-25~11-28 03:05 一连串 supplicant/加密改动 —— NIST P-256/384/521 EC 与大数原语、
  SAE 载荷、WPA1/TKIP/WPA3-SAE 打开、"给 PMK 就只按 PSK 加入"、口令原样传给 wpa_supplicant、
  TKIP Michael MIC 失败上报，最后一笔 `61c3fac1` 修 SAE 的条件交换阶梯、Jacobian 初始化泄漏和
  SAE confirm 的 WRONG_KEY 判定。这解释得通上游为什么现在能连 WPA3 网络。
- `UniXNU/ShengUfs`：APFS 容器式存储布局（`72ddbec6`）、root window 布局恢复（`b6fe07b3`）、
  verified UFS window 认成内部介质（`a4547682`）、11-21 的 pooled UFS descriptor read 与 RPMB W-LUN SCSI 钩子。
  `UniXNU/XnuPrelink`：native `__TEXT` 按 section 放置（`440589b0`）、FairPlayIOKit 内部
  `_ipc_kernel_map`/`_kernel_map` 槽位绑定（`e1b222b7`）。`UniXNU/QcaBluetooth`：macOS Type 0 LE event mask
  对齐标准 BLE 4.1~5.1 能力（`279e1caf`）。`UniXNU/AppleKeyStoreState`：#318 那句逐用户句柄返回
  `kIOReturnNotFound`（`a6f1434d`）。
- `UniXNU/SoftwareAES`（我们没 pin）：按设备派生 0x899 包裹密钥、所有出口擦除派生密钥、能力用 unique-chip-id 把关。
- `UniXNU/NativeMetal`（新仓，11-26 起 58 个提交）：A740 的 air-to-nir/pm4/GMEM/UBWC4 全套 + macOS 27.3 的
  MTL* SPI 补全。是 sheng/A740 那条线，不是我们的 A830；只作为"别人怎么把 Metal 后端做全"的参考。

## 4. 这些改动对着我们哪一条未完项（本轮只做到"能不能拿"，都没在本机验过）

| 我们的现象 | 上游本轮的对应改动 | 拿的代价 | 风险 / 前置条件 |
| --- | --- | --- | --- |
| `watchdogd` 每次 SIGTRAP（现在靠 `piano-watchdog-policy` 关掉它，已装机验证） | #324 的 `vmm-hide` boot-arg（xnu 级，按进程把 `kern.hv_vmm_present` 报 0） | 把 `macos-vmm-present-proc-filter.patch` 接到我们 `patches/k27` 尾部（现在到 `0077-…`）→ 重编内核 → 重打 boot → 持久刷 `boot_a`；boot-arg 走 `tooling/k273/build-k273-esp.py --arg vmm-hide=watchdogd` 现成机制 | 上游用它是为了让 FairPlay 那几个进程走物理路径，注释里明确"不打扰 watchdogd"，我们属于反向使用；真正的修法仍是发一个原生 `IOWatchdog`（接 `kIOWatchdogDaemonCheckEnabled`/`CheckUserspaceDefanged`/`Checkin`/`TestCPUHang`/panic）。用了 `vmm-hide` 之后策略单元可以留着当双保险 |
| 登录页要口令、`AppSSODaemon` 崩溃循环（这次开机窗口里只剩它那 2 条崩溃） | #317（去掉 iOS 全局 CS/库校验强制）+ #318（ACM 进 roots、`acm_fastsim=1`、排除 SEPManager/IOSlaveProcessor）+ `AppleKeyStoreState` 的逐用户句柄 `kIOReturnNotFound` | #317 是两行机器配置的反向补丁，接进我们的 xnu 系列成本低；#318 的 roots 需要 KC 基里**有** `com.apple.driver.AppleCredentialManager` 这个 kext | 我们的 KC 基是 k27 那条 VM 线（`tooling/kc/build-kc-k27.sh` 的 `KC_BASE` 默认 `$PIANO_K27_TREE/out/kc/kc27.bin`，27.0.1 vma2），keep 列表里是 `com.apple.driver.AppleVPCredentialManager` 而不是 `AppleCredentialManager`：真机 kext 大概不在我们基里，要走 `docs/identity.md` 里 option C（换 Mac17,11 的 27.3 KC 做基）才拿得到。这一条我**没验**，只是从两份 `kc-keep` 快照（`out/k27-independent-20261120/host-run/kc-preflight/configs/kc-keep.txt`、`out/upstream-pub-20261125/configs/kc-keep.txt`，26 条里没有 ARMWatchdogTimer/AppleCredentialManager）推的 |
| 亮度（`docs/drivers/backlight.md` 2026-11-21 那两轮里 corebrightnessd 的滑杆陈旧值） | #319（`sysctl_vmm_present` 只对 `corebrightnessd` 报 0）；#324 是它的通用版 | 和 watchdogd 同一批内核改动：`--arg vmm-hide=corebrightnessd` | 上游的完整链路还依赖他们背光驱动发布 `NativeSandboxGrantProcess` + 1 字节 OSData force-brightness-control；我们的 `PianoBacklight` 有没有对等的东西要单独核 |
| cryptexd 的 `Preboot/Cryptexes/{ExclaveOS,Rosetta}` 运行时补不出来（`/System/Volumes/Preboot` 与 `/` 同 dev，`mkdir`/`touch` 一律 `ENOENT`） | #328 把 `cryptexd`/`wifip2pd` 放进 `native_deny_exec` 默认名单，直接不让它 exec | 我们内核里现在**没有** `native_deny_exec`：在 `patches/` 全部 25+4+3+2+1 个补丁里 grep `deny-exec`/`native_deny_exec`/`k27-parity` 都是 0 命中。要拿就得连那段 parity 一起补 | 我们的路线是组装期在 `tooling/k273/make-systemos.sh:70` 补目录；deny-exec 是"补不出来就别让它跑"的替代，两条会互相掩盖，先别混 |
| `NativeGopDisplay::swap_wait` 偶发 `0xe00002d5`、帧率分析 | `GopFramebuffer` ahead 4：`b0d812d5` 统计presented 格式和每帧走的路径、`e9ed7639` 合成器接管面板时停掉内核进度条，附 `Test/check-console-handoff.py`、`Test/check-fence-wait.py` | 读他们的两个主机测试 + `NativeLegacyDisplay.cpp` 那 33/9 行，对照我们的 swap_wait；不改 pin | 我们的显示栈是自研 `NativeGopDisplay`，不是他们的 `NativeLegacyDisplay`，只能对思路 |
| 视频/IOSurface | `AdrenoMetal` `e0d8c3c9` 16 字节 IOSurface 行对齐（runtime v2） | 对读；他们动的是 `NativeBridgeMetalFrontend.mm`，我们树里也躺着两份该文件的副本（`.tmp-bridge/nm-lf/`、`.tmp-nm-c68/`，都是未跟踪的临时目录，别当资产） | 我们的 Metal 后端与他们的 bridge 不同源，行对齐只是同类问题参考 |
| Wi-Fi 只有家里那种网络连得上 | `Ath12kWifi` ahead 16：WPA1/TKIP/WPA3-SAE + SAE 的三处修复（最后一笔 11-28 03:05） | 想连 WPA3 网络才需要；拿就是 bump pin（本轮不做） | 他们改的是 supplicant 与椭圆曲线原语，属于要重新过一遍关联/掉线测试的那类改动 |
| RTC 纪元偏移（正在做的 `piano-rtc-offset=1776516615`） | 上游本轮**没有**相关改动 | — | 继续按我们自己的方案 |

## 5. 本轮没做、以及查了但不足以下结论的

- 没 bump 任何 pin，没改构建脚本，没重编内核，没动 `boot_a`/ESP。AGENTS.md 写明不按要求不动 `unixnu.lock`/`feeds.toml`。
- 想直接判"我们的装机 KC 里有没有 `AppleARMWatchdogTimer`"用了一个笨办法：在
  `out/persistent/boot-piano-macos-persistent-v6-sealed.img`（100,663,296 字节）上 `grep -a -o -c`，
  结果 `AppleARMWatchdogTimer`、`com.apple.driver.AppleCredentialManager`、`IOWatchdog`、
  连我们确定在 keep 列表里的 `com.apple.driver.AppleVPCredentialManager` 全是 0 —— boot 镜像负载是压缩的，
  这种字符串搜索**无效**，别拿它当证据。要判得在解包后的 KC 上查。
- 设备上 `kextstat` 会挂住（跑 30 s 无输出），`ioreg` 那条路还没试；这类"查已加载 kext"的只读操作下次换成
  直接读 `out/` 里未压缩的 KC 产物。
- `UniXNU/UniXNU` 与我们 pin 的整体 ahead/behind 是昨天记的 649/331（分叉，不是落后），今天只核了
  11-25 之后的 24 个提交里跟 macOS profile 有关的部分。

## 6. 顺带记一笔本机现状（不是验收，别当结论）

17:15:01 那次无人值守重启之后设备一直在 macOS；17:47:06 root SSH 正常，`kern.boottime` = 1795857382
（Sat Nov 28 17:16:22 2026），`uptime` = 29 分钟，但 load average 报 **730.90 / 723.34 / 608.11**，
而 `ps -o pcpu` 里最忙的两个只是 `VTDecoderXPCService`（pid 542，43.0 %）和 `WindowServer`（pid 170，13.4 %），
`WallpaperAerialsExtension` 2.6 %。load 和 ps 不成比例，倾向于有一批线程卡在不可中断等待 ——
和 `swap_wait` 的 `0xe00002d5` 可能是同一件事，明天单独查，别和上游这批混在一起。