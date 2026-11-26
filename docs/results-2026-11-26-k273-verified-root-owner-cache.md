# 2026-11-26 - verified 根的 policy 必须由 C 单元发出：汇编 `.quad` 造出的指针不是认证 fixup，内核解引用即 data abort

上游签名的 27.3 窗口 policy 已在 fork 内（`out/upstream-code002-2-20261121/scripts/policies/macos-27.3-26C5035r/`，
源 `code002-2/piano-macos-unixnu@e9ff4f0`，归档公锚 sha256 `896b32469a0214ea…`）。OS 窗口 3,538,432 块 /
14,493,417,472 B / 图 `40d8282eb8650da9…` / 卷 `84581E99-85BE-4957-AC53-2947E887B3E4`；SystemOS 窗口 712,192 块 /
2,917,138,432 B / 图 `14330ed990fe7022…` / 卷 `6678E466-2F88-4C56-9408-DC7361BAA0BA` —— 与本机 `linux_rootfs` 上那份
stock 根逐窗口对应（参考机 Mac 上那份 6 GiB SystemOS 没有签名，不能用）。本机重算的两个窗口块表
`OS.vroot` `f10b77a0…`（113,229,824 B）与 `SystemOS.vroot` `48402d8e…`（22,790,144 B）与 signed header 偏移 88 处的
表摘要逐字节一致，所以盘上的根不需要重做，块表本来就是 root.img 里现成的 object。

第一次带 policy 的 RAM 图 `83109f8c07054611f43fc52250c719264fa09a860858b3af6a867e361` 实机结果：控制台到
`NativeUfs: stage policy selection` 之后

    panic: Kernel data abort at pc 0xfffffff0008b515b0

不是 EAUTH，是 policy 表指针解引用炸。根因在生成器：`make-root-policy-object.py` 用汇编 `.quad _label` 造
`verified_root_policies[]`，k27 树的 KC 收集器（`scripts/kc_native.py`）把该 quad 当**普通 rebase**收进 chained-fixup 链，
内核里读到的是 `0x460000000f419a0` 这种垃圾（低 48 位指向 policies 数组自身 + 0x1a0），普通 rebase 内核不会 bind。
同一 kext 的 C 指针初始化器（`__kalloc_type` 那些）能工作，因为 clang 发出的是**认证** fixup。

改成 C 翻译单元：`tooling/k27/make-root-policy-source.py` 把 policy 目录编成 `root-policies.cpp`（两个
`static const uint8_t policy_XX[544]` 加 `verified_root_policies[] = {{policy_OS,544},…}` 与 count），
`build-native-ufs.sh` 的 verified 分支不再 `--objects-extra` 预编对象，而是把该 .cpp 交给 `build-native-kext.py` 一起编。
两处链接细节是必须的：NativeUfs 用 `extern "C" { extern const uint8_t verified_root_key[]; }` 要这把锚，而 C++ 命名域里
裸 `const` 数组是**内部链接**，所以定义前要先写 `extern` 声明并包在 `extern "C"` 里；另外归档锚就是 `verified_root_key`
本身（`VROOTKEY` + RSA-3072 模数 + 65537 = 395 B = `VR_KEY_BYTES`），旧写法把同一份字节发两遍，`-Werror` 下先编不过。
verified 构建还需要 `_boot_progress_poll`，由 `tooling/k27/BootProgressPoll.c` 提供（`docs/results-2026-11-21-verified-ufs-without-poll.md`）。

构建产物 `2ffa854c6a441e468b30642cc14347286b32c5ac327d6ea082b8b02ba973ef0c`（160,735,232 B；UEFI 侧 `eb570025…` 未变）。
上机前的静态验收（不烧设备循环）：`out/k27/kc-k27-native.natives.json` 给出 `_verified_root_key` = `0xfffffff00708e1e8`、
`_verified_root_policies` = `0xfffffff007f45800`，按 `VROOTKEY` 文件偏移定出差值 `0xfffffff007004000` 后，policies 数组
（文件偏移 `0xf41800`）的两个指针低 48 位是 `0x8a373` 与 `0x8a593` —— 相差正好 544（一个 header-only envelope），
`0x8a373` 恰在锚（`0x8a1e8`）之后 395 字节，两处都以 `VROOT01\0` 开头且块数/容量/卷 UUID 与上表一致，
count 读回 2。也就是说指针这次真的解析到 policy 头上，剩下的判读只能上机看 owner cache 阶段。

## 实机结果（`2ffa854c…`，2026-11-26 20:12 无人值守 RAM 启动）

owner cache 授权门**已过**。控制台里两个窗口都走完 verified 流水线（`stage policy selection` → `policy storage` →
`policy allocation` → `policy table object read` → `policy read-only protection` → `owner signature and hash table` →
`root window attachment` → `stage signed window identities` → `root publisher thread` → `media publication`，
不再有 `UNVERIFIED development root` 那行）。随后

    apfs: mountroot → disk5s1 mount-complete volume Macintosh HD（vol-uuid 84581E99-…，3,538,432 块）
    init (pid 1) 在跑；disk3s1 mount-complete volume FizzSeedUpdate26C5035r.arm64eSystemCryptex（6678E466-…）

即 launchd 已经起来、SystemOS cryptex 也挂上了。新的死点在同一处 `load_init_program` 里再往下一步：

    panic(cpu 0 caller 0xfffffff000bfa2c28): offline scratch Data mount failed: 2 @kern_exec.c:8681

`2` 是 ENOENT，来自 `native_owner_data_prepare()` 的第一步 `native_ram_block_volume_seed(XNU_OWNER_DATA_VOLUME_UUID …)`：
按卷 UUID 找那块由 NativeRamBlock 提供的 IOMedia，找不到就 ENOENT（`NativeRamBlock.cpp:1298 selectVolumeSeed`，
匹配数必须正好为 1）。控制台同义：

    NativeRamBlock: Data is volatile this boot (state 0, data-persist=0): its changes stay in RAM and are gone at the next boot
    NativeRamBlock: Data store state 0 (1 blank, 2 resumed, 3 fresh, 4 failed, 7 kept), header 9, first LBA 0, 0 pages,
                    direct 0, seed 7e9bd39b, store seed -, flags 0, generation 0, present 0

也就是本机 `linux_data` 上**没有 store**（`present 0`），`data-persist=0` 只给一块易失盘，APFS 容器起了但没有 owner Data
卷可挂（日志里 `disk0` 容器初始化过，却始终没有 `disk0s1` 卷）。维护者那台机器是从 27.0.1 线继承的 store（seed `a53a3c05…`），
所以同样的 `data-persist=0` 能挂上 Data；`docs/k273-line.md:121` 的 `k27: firmlink …` 正发生在这个函数挂上 Data 之后。

**下一步（正在做）**：同一装配加 `PIANO_DATA_PERSIST=2` 与 `PIANO_EXTRA_ARGS="data-store-part=linux_data"`（`data-store-part`
不在 `build-ram-images.py` 拒绝的 `data-persist/store-replace/store-flatten` 三件套里），让 NativeUfs 在 `linux_data` 开 store
窗口并由 NativeRamBlock 绑成 state 1/3；不给 `data-store-lba`，免得上机后 NativeUfs 因窗口位置不符直接 `refused`。
