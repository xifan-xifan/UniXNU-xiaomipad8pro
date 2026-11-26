# 2026-11-27 - 第三发也证否（禁 `/sbin/mount` 变 `posix_spawn … Operation not permitted`）；上游 11-26 晚间状态与安装指南读毕：这道墙就是上游自述缺口 G1b

## 第三发：`k27-deny-exec=/sbin/mount`

镜像 `bd128e978fa3436cc4fdd93fbef1ca4f580a74c3baa6beb430575c68c12ad4b0`（Mac `out/ram-verified-8748-mnt/`，
Windows `out\k273-ram-8748-mnt\`），运行 `logs/native-runs/ram-k273-8748mnt-20261127-002026`：00:20:45 起，113 s 回 ABL。

```
com.apple.xpc.launchd <Error>: (mount-phase-1) <Error>: posix_spawn(): 1: Operation not permitted
com.apple.xpc.launchd <Emergency>: Boot task failed: mount-phase-1 - posix_spawn(): 1: Operation not permitted
```

`RequireSuccess` 的 boot task 连"起不动"也算失败。至此三个纯配置口子全部证否，且都是同一道墙：
`mount -P 1` 必须**成功**，而它只能靠 `/sbin/mount` 本身是替身。

顺带把上一轮的推断纠正过来：SystemOS 替身的写法是 `[ "$1" = "-P" ] && exit 0` 再 `exec /sbin/mount.real "$@"`，
`&&` 短路 —— 带 `-P` 时**不会** exec 原版。也就是说只要替身在，phase 1 就直接成功；上一轮我写"它会先 exec 原版"是错的，
`-mr`（禁 `mount.real`）的结果也证实了这点：那次 `apfs_boot_util[4]` 照样跑，说明设备上 `/sbin/mount` 就是 Apple 原版，
替身压根不在设备上。

## 设备上跑的 SystemOS 是哪个（这一条把选择空间钉死了）

`docs/results-2026-11-21-stock-root-v2-assembly.md` 记的四个窗口就是本机 `linux_rootfs` 上那份（`db3629e5…`）：

| 窗口 | 偏移 | 字节 | SHA-256 |
| --- | --- | --- | --- |
| OS | `0x100000` | 14,493,417,472 | `40d8282e…`（IPSW 解密后的 System 卷，未改） |
| SystemOS | `0x35ff00000` | 2,917,138,432 | `14330ed9…`（IPSW 解密后的 **Apple SystemOS cryptex**，未改） |
| OS.vroot | `0x40dd00000` | 113,229,824 | `f10b77a0…`（3,538,432 × 32 哈希表） |
| SystemOS.vroot | `0x414900000` | 22,790,144 | `48402d8e…`（712,192 × 32 哈希表） |

Apple 的 SystemOS cryptex 是一个不含 `/sbin` 的密码学扩展卷；联合挂载后 `/` 的上层没有 `/sbin/mount`，
查询落到 OS 卷那层，拿到的是 Apple 的 `mount`（206,992 B）+ Apple 的 `apfs_boot_util`。这与日志完全吻合
（真 `fsck_apfs` 在跑、`apfs_boot_util[4]`、`mount: apfs_boot_util exec failed`）。
参考机跑的是 `tooling/k273/make-systemos.sh` 造出来的 SystemOS 卷（卷名 `SystemOS`，Mac 上挂
`out/inputs/k273/root/systemos.img` 见到 `sbin/fsck` 17 B / `sbin/mount` 358 B / `sbin/mount.real` 206,992 B），
所以它一辈子看不见这道墙，上游文档里 `mount-phase`、`apfs_boot_util` 两个词一次都没出现（对 `d5f0c76d` 整树 grep 为空）。

## 上游 11-26 的状态（读 `README.zh-CN.md`、`skills/install-piano-macos/SKILL.zh-CN.md`，FETCH_HEAD `d5f0c76d`）

- 基线 `2438556c` 之后 201 个提交。日常安装版本 **v13.1y12q**（安装检查 75 通过 / 0 失败）：**从磁盘持久启动并保留机主数据**、
  60 Hz 显示、原生触摸屏、键盘套与触控板、Wi-Fi 自连、扬声器、电量、PD 9 V 与 80 % 上限、自动亮度、
  Adreno 830 上的 Metal 与分页显存、4 GiB GPU 窗口、Photo Booth 录像、QuickTime/Safari 硬解（H.264 到 1080p）、
  Safari 的 WebGL2/WebGPU、开沙盒的 Chrome GPU 加速、v13.1y12m 起免参数 Chrome WebGPU（内核补丁 0075）、
  保护套/电源键策略、原生截图、BLE 事件掩码修复；已安装系统用 13 GiB 内存窗口；UFS Data 并发模式已启用
  （QD8/QD32 随机读 4.9×/5.6×，混合负载读 p99 2528 µs → 269 µs，重启后校验记录完整）。
  内核补丁 0076 让 USB 调试 shell 等助手读完整启动参数（旧接口在 1024 字节截断）。
- 缺口表里 **G1b 就是我们这一堵墙**（原文）：
  > SystemOS 启动垫片（`overlay/systemos-shims/sbin/fsck`、`mount`：两个分别为 17 和 358 字节的 shell 脚本）｜属于 k27 树｜
  > **在本仓库中自行重写垫片，并在 `make-systemos.sh` 中加入 `SYSTEMOS_SHIMS` 开关；这会改变固定清单的 `sbin` 组，
  > 因此需要新的固定值和一次平板验证（已安装的版本保留其窗口）**
- 其余相关条目：G3（Data 种子要自己造，我们已用 `make-macos-data-seed.py --volume-uuid` 造过并验过）、
  G6（写盘工具钉参考机，正解是本机 profile JSON —— 我们已经用 `PIANO_UNIT_PROFILE` 走通）、
  G7（`boot_a` 签名要用自己的 `boot-signing.pem`）、G8（日常配方在 `configs/` + `tooling/userland/`）、
  G11（本机出厂 309 与钉的 303 不同代）。
- 输入侧新东西：`tooling/repro/fetch-ipsw.py` 从 Apple CDN 只取 K114c 身份的成员（13.3 GB / 全量 27.2 GB），
  逐成员比 pin，AEA 解密的密钥经 `ipsw fw aea --key` 现取现用、不落盘不打印（`--dry-run` 已跑通，见下）。
- 安装纪律与我们的做法一致，且更硬：绝不写 `xbl/abl/tz/hyp/vbmeta/dtbo/persist`、任何 `_b`、`userdata/super/metadata/
  vendor_boot_a/init_boot_a`；只用 `maint-session.py` 在维护镜像里 `dd`；每次写前三路 sha256 备份、写后绕页缓存回读比对；
  只写 GPT 新分区 37 `esp`、ESP、`boot_a`、根卷、Data store 区。

## 由此定的主线（不再找绕过）

设备上的根卷要换成"我们自己的 SystemOS 窗口"，这与上游 G1b 给的方案同一条：

1. Mac 上取两个 cryptex 成员（`Cryptex1,SystemOS` 2,368 MiB → 解密后 sha `14330ed9…`、`Cryptex1,AppOS` 52 MiB 已在本机），
   OS 盘不必下载 13.8 GB：`out/inputs/k273/root/root.img` 的 OS 窗口（offset `0x100000`，13,822 MiB）逐字节就是 pin 的 `40d8282e…`。
2. `tooling/k273/make-systemos.sh OS.dmg 141-22145-021.dmg 141-22106-021.dmg out/inputs/k273-8748-shim/root/systemos.img`
   （sudo：`newfs_apfs` + `ditto`；它本来就吃 `$PIANO_K27_TREE/overlay/systemos-shims/sbin`，Mac 上那份 k27 树里两个脚本都在）。
3. 出 vroot 表与信封：`kits/VerifiedRoot` 的 `tools/manifest.py`（`--key/--public_key`，anchor = `VROOTKEY` + 3072 位模数 + 65537），
   **用我们自己生成的 RSA-3072 密钥签**：内核比对的那把 key 是我们自己 kext 里的 `verified_root_key` 符号
   （`tooling/k27/make-root-policy-source.py` 从 `trusted-key.bin` 生成），不需要厂商私钥。这一步要把
   "策略可信根就是我们嵌进去的那把公钥"这条口径在文档里写清。
4. `make-root-image.sh` 组 v2 根镜像（OS 不变 + 新 SystemOS + 两份 vroot），`make-root-policy-source.py` 出新策略源，
   重建 verified 装配，`data-store-lba=117735999` 与新 seed 不变。
5. 写盘（已授权，出厂后无机主数据）：只写 `linux_rootfs` 的根卷区（102269696..107512575），
   ESP 与 store 窗口不动，写前备份 + 写后绕缓存回读；然后 RAM 镜像判读 `mount-phase-1` 是否 `exit 0`、
   桌面是否出来。之后才是 GPU（本机 zap `e73f3a4c` vs 上游钉 `bb942413` 的两难）、输入、网络、音频、
   Data 写后重启读回、Android 恢复、两轮双向切换、三次冷启动、正常重启/硬复位、v2 日用项。

## 输入侧的准备（本机实测）

`tooling/repro/fetch-ipsw.py --dry-run`（Mac `upstream-tip` @ `d70c16c`）通过：IPSW `UniversalMac_27.3_26C5035r_Restore.ipsw`
27,206,093,542 B，server sha `1f372fa1…`，峰值/保留 31.5 GB，输入卷剩余 95.0 GB；唯一缺的依赖是 `ipsw`，
实测 `/opt/homebrew/bin/ipsw` 已在（3.1.752，只是不在非登录 shell 的 PATH 里，脚本内补 PATH 即可）。
按成员取用 `ipsw-members.py fetch --out out/inputs/k273/26C5035r "Cryptex1,SystemOS" "Cryptex1,AppOS"`，
`141-22106-021.dmg` 本机已有（54,525,952 B），SystemOS cryptex 2,368 MiB 在取。