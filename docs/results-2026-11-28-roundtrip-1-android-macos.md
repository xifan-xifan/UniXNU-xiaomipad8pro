# 2026-11-28 - 第一轮 Android→macOS 往返实机通过（含槽位成功位/重试次数的完整来回）

切换机制就是 `boot_a` 换镜像（电脑端，无人值守，不需要按键）：Android 侧写原厂备份
`E:\piano-backup\persistent-boot-20261128-010938-fe5vsrzx\boot_a.img`（`7c444a2d6aa930cd79e2d48c569f6c891fbaff054c63fc85c3a57fb61c660ac2`），
macOS 侧写 `out\persistent\boot-piano-macos-persistent-v6-sealed.img`（`df6d5a18f27054e69a394810e2b4d3452818d428cbed8795ea0b11641c5b1f30`）。
ESP 不动（`esp-7ccdd855-k273-ul3.img` `28592e9d...`）。往返日志：`logs\roundtrip-macos-to-android-1.txt`、
`logs\roundtrip-android-to-macos-1.txt`；发送前槽位快照 `logs\switch-before-macos-to-android-1.txt`。

## 时间线（本机时钟 06:04:01 → 06:05:27 + 回到桌面）

1. macOS 里 `piano-console.ps1 -Line 'reset fastboot'` → ABL fastboot（约 10 s，无按键）。
2. 发送前：`slot-successful:a yes`、`slot-unbootable:a no`、`slot-retry-count:a 6`。
3. `fastboot flash boot_a <原厂>`：`Sending OKAY 2.551s` / `Writing OKAY 0.181s`。
   发送后立刻读回：**成功位被清掉**（`successful no`）、`retry 7`。这正是 `HANDOFF` 记的那条规则：刷 `boot_a` 会清 GPT 属性 bit 54。
4. `fastboot reboot` → 约 20 s 后 `adb devices` 出现 `7ccdd855 device`：
   `ro.build.version.incremental = OS3.0.309.0.WPYCNXM`、`ro.product.model = 25091RP04C`、`ro.build.version.release_or_codename = 16`、
   `sys.boot_completed = 1`、`getenforce = Enforcing`、`/data` 挂在 `dm-56`（376 G，已用 13 G）。**HyperOS 3.0.309 起来了。**
   Android 侧留了标记 `/data/local/tmp/piano-rt1.txt`（`PIANO-RT1-ANDROID-20261128`，sha1 `7f984d37a5f1b61565a5c7b1e462d70ff5fe4b5f`），
   下一轮进 Android 时读回，证明 Android 的 Data 也没被动过。
5. `adb reboot bootloader` 回 ABL：**`successful:a yes`、`retry 6`** —— 是 Android 把成功位写回来的（macOS 启动集永不写回），
   和 `HANDOFF` 里"Android 会把成功位写回"的取证一致。
6. `fastboot flash boot_a <v6-sealed>` → 发送后 `successful no`、`retry 7`（同一规则，方向反过来也一样）→ `fastboot reboot`。
7. 约 25 s 后 COM5/COM6/COM7 全部回来，root shell 读回 macOS 的两个标记，MD5 与切换前完全一致：
   `persist-normal.txt` `b88c264588d85dc3f76957506b07ccdb`、`persist-normal2.txt` `8bd0e7a8811c747617ce8a54441a81fa`。
   `uptime` `up 7 mins` 对应这一次切换后的启动，说明读的是切换后新会话的盘，不是缓存。

## 这条规则现在有了双向证据，验收时要当成预算来算

`slot-retry-count:a` 在 macOS 启动集装着期间每开一次机减一（成功位被刷 `boot_a` 清掉，macOS 永不写回），
减到 0 会被 ABL 标 unbootable，然后连 RAM 镜像一起拒绝。本轮的读数是 6 →（刷 Android）7 → Android 起来写回 →（回 ABL）6 →（刷 macOS）7。
所以**每次连续做 macOS 冷启动之前先看一次 `slot-retry-count:a`**，接近 0 就先回一趟 Android 或按 `slot-success-crc` 把 bit 54 补上，
不要等它归零后再补救。

## 第二轮往返（同一台、同一对镜像，06:14:26 → 06:15:10 + 回桌面）

`logs\roundtrip-macos-to-android-2.txt`、`logs\roundtrip-android-to-macos-2.txt`。

- 发送前 `successful:no`、`retry 6`（上一轮 macOS 起来后没有写回成功位，retry 从 7 掉到 6 —— 与规则一致，见下）。
- 刷原厂 `boot_a` → `retry 7`；`adb` 约 20 s 内出现 `7ccdd855 device`，`sys.boot_completed=1`、`OS3.0.309.0.WPYCNXM`、`Enforcing`。
- **Android 侧的 Data 也没被碰**：第一轮留的 `/data/local/tmp/piano-rt1.txt` 读回仍是 `PIANO-RT1-ANDROID-20261128`，
  sha1 `7f984d37a5f1b61565a5c7b1e462d70ff5fe4b5f` 一字不差；这轮又留了 `piano-rt2.txt`（sha1 `abd9fd6e095c32ffadfb1e10043434a2da786287`）。
- `adb reboot bootloader` 回 ABL：`successful:a yes`、`retry 6`（Android 又一次写回成功位）。
- 刷回 `boot-piano-macos-persistent-v6-sealed.img`（`df6d5a18…`）→ `retry 7` → `reboot`；约 25 s 后 COM5/6/7 回来，
  `piano-boot-guard: healthy after 180 s, restart reason normal`，root shell 读回两个 macOS 标记：
  `persist-normal.txt` `b88c264588d85dc3f76957506b07ccdb`、`persist-normal2.txt` `8bd0e7a8811c747617ce8a54441a81fa`（与切换前一致），`uptime up 5 mins` 对应这一轮的新会话。

两轮 `Android→macOS→Android` 都成立：两个系统各自的 Data 都活着，切换只需要写 `boot_a` 一个分区，全程无按键。

