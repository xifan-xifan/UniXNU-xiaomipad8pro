# 2026-11-28 上游第三轮扫描：piano 仓还是零提交，可这一轮挖到两条没读过的**开着**的 PR，其中 #326 直接落在我们改过的同一个内核文件上

扫描 2026-11-28 18:50~19:05（本地）。只读：`git fetch upstream --prune`、`git ls-remote`、`pull/<n>.diff`、GitHub REST。
本轮**没有** bump 任何 pin，**没有**重编内核，**没有**动装机集（`boot_a` `df6d5a18…`、日常 ESP 是 ul14 `252d2569…`、
Data seed `b2583ea5` 都不变）。

## 0. 结论

1. `lenzhang/UniXNU-xiaomipad8pro` `main` 仍然是 `751a0a12`，拉取之后 `git rev-list --count HEAD..upstream/main` = **0**、
   `upstream/main..HEAD` = **1937**：我们这边已经把上游全量吃掉，没有东西可合。仓库 `pushed_at` 还是 11-27 02:30 UTC
   （= 11-27 10:30 本地），`open_issues_count` = 0，`#1`…`#5` 最后一条动静停在 11-26。
2. 依赖侧那 14 个"近期动过"的仓，逐个 `git ls-remote … HEAD` 与 17:41 那轮记的 tip 逐个相同（`UniXNU` `fdf81eaa`、
   `AdrenoMetal` `85832c3a`、`Ath12kWifi` `61c3fac1`、`ShengUfs` `02a67a79`、`QcaBluetooth` `a4740937`、
   `AppleKeyStoreState` `309cf585`、`XnuPrelink` `34c80648`、`GopFramebuffer` `0fcda4f1`、`UniXNU-sheng` `993f6bdd`、
   `UniXNU-sm8750-common` `feaa135d`、`Adreno830` `1a132105`、`DarwinBoot` `5243c68e`、`Mu-Silicium` `30cadfb4`、
   `piano-uefi` `5b0ddb72`）。也就是说上一轮之后**一个提交都没多**，org 列表里最后一个 push 还是 11-27 19:10 UTC。
3. 真正没消化的是 **`UniXNU/UniXNU` 上开着的 PR**。上一轮只扫了已合并那批（#317/#318/#319/#324/#328），
   `pulls?state=all&sort=created` 里 `#320`、`#325`、`#326`、`#331` 四条 open 的没读。这轮读了差异全文，
   逐条判定放在第 2 节：**#326 值得进我们的内核系列（要重编内核+重封 boot，属换装机集那一档，本轮没做）**，
   #325 对我们**不适用**（有证据），#331/#320 不适用。

## 1. 拉取与对照的命令口径

```
git fetch upstream --prune                  # 没有新 ref
git ls-remote upstream                      # HEAD == refs/heads/main == 751a0a12…，refs/pull/{2,3,4,5}/head 还是那四个
git merge-base --is-ancestor upstream/main HEAD   # 退出码 0
git rev-list --count HEAD..upstream/main          # 0
git rev-list --count upstream/main..HEAD          # 1937
```

`CrossDarwin/UniXNU_macOS`（用户提过的那个仓）也照了一次：它是 `UniXNU/UniXNU` 的 fork，默认分支 `test_27`，
39 个分支，`pushed_at` = 11-25 15:03 UTC，**比上游 `main` 的最后一个 push（11-27 19:10 UTC）还早**。
那一堆 `build/*`、`boot/*`、`diagnostics/*` 分支是有料的参考，但没有比上游更新的东西可拿，所以不构成"上游更新"。

## 2. 四条 open PR 逐个（差异全文取 `https://github.com/UniXNU/UniXNU/pull/<n>.diff`）

### #326 `macos-27.3: Clear GOP framebuffer on graphics handoff without dc zva`（open，11-26 13:05）

新增 `profiles/os/macos-27.3-26C5035r/patches/xnu-12377/macos-clean-gop-handoff.patch`，并把它追加到
`profiles/os/macos-27.3-26C5035r/patches/xnu/series` 末尾（现有最后三行是 `cshash-*-revalidation`、
`macos-addon-applications`）。补丁只有 33 行，落点 `osfmk/console/video_console.c` 的
`initialize_screen()` → `case kPEGraphicsMode:`：在 `!boot_progress_active()` 且 `v_depth==32`、
`v_rowbytes >= v_width*4`、基址与行距 4 字节对齐时，把整块 GOP 帧缓冲刷成黑；能 8 字节对齐且宽度为偶数就走
`volatile uint64_t` 逐 64 位写，否则 `volatile uint32_t`。作者写的理由很具体：**不能用 `bzero`/`memset`**，
编译器会对 `ml_io_map_unmappable` 映射的 Device 内存发 `dc zva`，触发 `ESR=0x96000061` 对齐异常。

对上的点：

- 它改的文件和我们 `patches/k27/0073-k27-panic-text-on-linear-scanout.patch` 是**同一个** `video_console.c`
  （0073 还往 `osfmk/console/Makefile` 的 `PRIVATE_DATAFILES` 里加了两枚头文件）。两边都在
  `initialize_screen()` 附近，合的时候必须逐行确认上下文，不能指望 `git am` 直接过。
- 我们文档里记着交接期屏幕上的 leftovers：`docs/drivers/native-gop-display.md:353` 写 DarwinBoot 在 XNU 之前
  会在顶上 1/16 压一条绿带，`native-gop-display-bars=1` 是我们自己查通道顺序用的八条色带。
  "进图形模式先刷黑"正是这一类残留的最省事解法，而且带 `dc zva` 这条硬约束的现成写法。
- 代价说清楚：这条要**重编内核**（我们的系列现在到 `patches/k27/0077-…`）→ 重出 kext/collection → 重封 →
  **重写 `boot_a`**。我们的 `boot_a` 是 v6 密封镜像 `df6d5a18…`，装机以来从未重写过，所以这是"换装机集"那一档动作，
  要排在当前 ul14 的验收项之后单独开一轮，不能顺手塞。
- 状态：**维护者/贡献者报告**（他们的补丁说明），我们没有构建过、没有本机验证过，也没验证 `!boot_progress_active()`
  这个闸门在我们的启动路径上是不是真的成立（我们启动期有进度条吗，这点还没查）。

### #325 `runtime: Prune /private/var/folders/zz in icon cache refresh job`（open，11-26 13:05）

改 `scripts/build-boot-runtime.py` 里 `sign_launchd_cache` 生成的那段清理脚本：原来
`find -P "$folders" -mindepth 4 -maxdepth 4 \( -path '*/C/com.apple.iconservices' -o -path '…store' \)`，
改成 `-maxdepth 4 -path "$folders/zz" -prune -o \( -path "$folders/*/*/C/com.apple.iconservices" -o … \)`，
配一个新主机测试 `test_unreadable_zz_bucket_is_pruned`（把 `zz/zyxvpxvq6csfxvn_n0000000000000` chmod 0 之后
`clear()` 仍返回 0，且那个受保护的桶还在）。动机是能读：老写法 `-path '*/C/…'` 会走进 0 权限的 `zz` 桶，
`find` 报错 + `|| exit 1` 让这个 job 直接失败。

**对我们不适用，两条独立证据**：

1. 我们的装机集里没有这个 job。设备 `/Library/LaunchDaemons/` 一共 18 条，全是 `com.unixnu.piano-*` /
   `com.unixnu.pianousb-shell`（airportd-kick、camera-grab、charge-config、data-concurrent、keyboard-type、
   lw-quiet、pmset-defaults、policy-agent、powerlog-policy、sshd、still-wallpaper、usbnet-address、
   watchdog-policy、wifi-controls、wifi-join、wifi-supplicant 等），没有任何 icon-cache 清理 job；
   我们仓库里 `scripts/build-boot-runtime.py` 只在 `docs/results-2026-11-26-…launchd-stops-at-mount-phase-1.md:63`
   被引用过一次（分析上游行为），构建链路 `tooling/k273/*` 不调用它。
2. 那个故障形态在我们这台机器上也不存在：`/private/var/folders/zz/zyxvpxvq6csfxvn_n0000000000000` 是
   `drwxr-xr-x root wheel`，`zz` 下 30 个桶没有一个 0 权限。

留着当口径：以后我们若真加 icon 缓存清理 job，直接抄他们 `-prune` 的写法，别再写 `*/C/` 通配。

### #331 `devices: add nx769j (Orange381)`（open，11-28 02:17）

`devices.toml` 加一条 format 2 记录：nubia RedMagic 9 Pro/9S Pro（`nx769j`），**SM8650**，
`os_profile = "ipados-27.0.1-24A446"`，组件表里带 `ShengUfs`、`VirtioMmioBlock`。不是我们的 SoC（我们 SM8750）、
不是 macOS profile。唯一可看的是他们记录设备页的字段形状（`repository_id`、`revision`、`record_state = "available"`）。不适用。

### #320 `fix(catalog): render areas without an evidence key`（open，11-22 12:10）

`scripts/porting-record.py` 三处 `area['evidence']` 改 `area.get('evidence', [])`，让没有证据键的 area 能渲染。
纯上游设备页目录工具，我们仓库全文 grep `porting-record` 零命中（我们的结论自己写在 `docs/results-*.md`）。不适用。

## 3. 顺带记一笔本机现状（不是验收，别当结论）

ul14 那一轮一直在 macOS：`kern.boottime` = 1795862016（18:33:36），18:52:13 root SSH 正常，`uptime` 19 分钟，
`load averages` 63.61 / 118.60 / 116.61（开机时记到 581/349/162，随时间衰减，这条另案）。
`/Users/piano/Desktop/` 八件标记逐个 MD5 与记录一致：`ul11-rt1` `a3f8428d…`、`ul11-rt2` `843081c6…`、
`ul11-hard` `ef00c0de…`、`ul11-cold1` `0480425f…`、`ul11-cold2` `4b90aae3…`、`ul11-cold3` `80e33cf1…`、
`ul12-c1` `fe817565…`、`ul12-dirty2.log` 242,094 字节 `dcf0b777…`；目录里另外 9 项是更早几轮留下的
`cold1/2/3`、`persist-normal`、`persist-normal2`、`persist-ul7`、`ul10-cold-1` 加 `.DS_Store`、`.localized`，
不是新写入的验收标记。