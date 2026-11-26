# 2026-11-26 - 本机 Data store 建成了（state 3，窗口绑在 117735999），但 owner Data 卷仍找不到：`offline scratch Data mount failed: 2`

镜像 `c00039100df40233b75fbecfe647386308c1282f59ec13e45596f93216daf3ea`（160,735,232 B；Mac `out/ram-verified-store/`，
Windows `out\k273-ram-verified-store\`）：`2ffa854c…` 同一装配，只改 `data-persist=2 data-store-part=linux_data
data-store-lba=117735999`。构建脚本 `out/build-verified-store.sh`。

## 为什么要先按本机 profile 覆盖几何

`tooling/persist/common.py` 把参考机的 LUN 0 几何写死在 `DATA_LAST = 124552186`，`k273_store_first()` 用它算 store 窗口，
`check_store_size()` 又只接受这个值，于是本机构建直接拒绝：
`a Data store on linux_data needs data-store-lba=117998394 ... not None`。本机 `linux_data` 是 107512576..124289791
（`out/unit-profile/piano-7ccdd855.json`），窗口 `store_first(124289791, 6291649) = 117735999`，与已装 ESP
（`68c28df1…`）里 `macos` 项记的 `store_first_lba=117735999` 一致。fork 已有正解：`apply_unit_profile()` 在 import 时读
`PIANO_UNIT_PROFILE`，把 `DATA_LAST` 等换成本机的值。Mac 上的 `upstream-tip` 树里那份 `common.py` 还是上游的旧版（没有
`apply_unit_profile`），已用 fork 版覆盖（上游版备份在 Mac `/tmp/common-upstream-backup.py`），并把本机 profile 传到
`upstream-tip/out/unit-profile/piano-7ccdd855.json`。核对：`DATA_LAST=124289791`、`k273_store_first=117735999` ✓。

## 实机结果

无人值守跑 `tooling/native/run-console-ram-k273.ps1 -Tag verifiedstore`：`20:48:26 fastboot boot` → 100 s 回 ABL →
`dump rc=0`。控制台（`logs/native-runs/ram-k273-verifiedstore-20261126-204807/`，7,340,032 B 转储）里 store 确实起来了：

    NativeRamBlock: data-store-lba=117735999: window at LBA 117735999, store new, bound to it
    NativeRamBlock: Data store state 3 (1 blank, 2 resumed, 3 fresh, 4 failed, 7 kept), header 9, first LBA 117735999,
                    6291456 pages, direct 1, seed 7e9bd39b, store seed -, flags 2, generation 1, present 0

死亡点没变，仍是 `load_init_program` 里 `native_owner_data_prepare()` 的第一步：

    panic(cpu 0 caller 0xfffffff000bfa2c28): offline scratch Data mount failed: 2 @kern_exec.c:8681

即 `native_ram_block_volume_seed(XNU_OWNER_DATA_VOLUME_UUID)` 返回 0。`kern_exec.c:8274` 里那个 UUID 没被集合覆盖，
就是默认值 `8748FE31-7E7F-4BFA-A7E2-7A4F5E14D0F6`。`NativeRamBlock.cpp:1239 selectVolumeSeed` 的返回 0 只有两种可能：

- 迭代 IOMedia 时带该 `UUID` 属性的对象一个都没有（`matches == 0`）；
- 或者多于一（`if (++matches != 1) { selected = false; break; }`，`matches != 1` 也返回 0）。

转储里能读到的旁证：Data 容器（disk0，6,291,456 块）确实被 APFS 认出来了（`nx_mount: disk0 checkpoint search:
largest xid 11`），但 `stable checkpoint indices: desc 0 data 0`，且始终没有出现 `disk0s1`（对照 OS 容器：`disk5s1
vol-uuid: 84581E99-…` 是挂载时才出现的）。两个已装的 seed 输入是同一份（`esp-device.json` 记 seed sha `48dbc08d…`、
identity `7e9bd39b…`，8 extents / 22,200,480 B），所以不是"拿错 seed"。

因此两种解释都还站着：
1. 卷媒体不存在：seed 里没有能建立卷对象的稳定 checkpoint（`desc 0 data 0`），参考机上是因为**已有 store** 提供了这些页
   （`tooling/persist/base-seed.py` 的说明：读写挂载总会重写 page 0 和 checkpoint，store 的页覆盖 seed）。也就是说
   "blank 介质 + `data-persist=2` 新建 store" 不足以让 owner Data 卷出现，需要先有一份带卷的 base（或 flatten 过的自足 store）。
2. 卷媒体出现两次：verified NativeUfs 在本轮把两个根窗口各发布了两遍（同一尺寸出现两次：disk2/disk3 = 712,192 块，
   disk4/disk5 = 3,538,432 块），若 Data 侧同理翻倍，`matches == 2` 也会返回 0。

## 下一轮的最小探针（一次构建 + 一次启动就能判 1 还是 2）

在 Mac 那份 `package/NativeRamBlock/NativeRamBlock.cpp` 的 `selectVolumeSeed` 循环里临时加一条 IOLog（不进仓库）：
在 `if (++matches != 1)` 之前打印每个 IOMedia 的 `UUID` 与 `kIOBSDNameKey`，并在 `return 0/1` 前打印 `matches`、
`selected`。判读：只有 disk0 的容器/卷媒体名单 → 解释 1；出现两个同 UUID 的卷媒体 → 解释 2（那时先修重复发布）。
注意本轮转储的 pstore 解码质量下降（`xnu-log.py decode` 的阈值从 11 份里的 6 份降到 3 份，正文有零星位翻转）；
兜底办法是直接对转储抽可打印串（`re.finditer(rb"[\x20-\x7e]{10,}", raw)`），关键行仍能读出来。

## 顺带

- `PIANO_EXTRA_ARGS` 可以带 `data-store-part` / `data-store-lba`，`build-ram-images.py` 只拒绝
  `data-persist` / `data-store-replace` / `data-store-flatten` 三个（前者的值仍由 `check_store_size` 校验）。
- 本轮 store 的写入全部落在 `linux_data` 窗口 117735999..124027647，与 `linux_rootfs` 上的根镜像
  （102269696..106553855）不重叠；`linux_data` 出厂后本来空白，无用户数据。
