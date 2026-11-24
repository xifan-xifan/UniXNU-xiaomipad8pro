# 布局复核 — 2026-11-25

只读检查，未写入任何分区。设备 `7ccdd855` 在线，HyperOS `OS3.0.309.0.WPYCNXM`，槽 `_a`，`sys.boot_completed=1`。

## 当前 GPT 是候选布局，不是原厂布局

`ls -l /dev/block/by-name` 显示：

| 名称 | 节点 | 大小 |
| --- | --- | ---: |
| `linux_rootfs` | `sda35` | 20 GiB |
| `linux_data` | `sda36` | 64 GiB |
| `esp` | `sda37` | 1 GiB |
| `switch` | `sda1` | 8 KiB |
| `logfs` | `sda14` | 8 MiB |

`df -h /data` 报 376 GiB，对应候选 GPT 的 userdata（376.272 GiB）；原厂 GPT 的 userdata 是 461.273 GiB。因此设备当前运行的是 `out/maintenance-gpt/verified-trial-inputs/candidate/gpt-head.bin`（sha256 `12710135…`）描述的候选表。`out/persistent/session/state.json` 也记录 `gpt_state: candidate`、`esp_first_lba: 124289792`。

这纠正了两处旧记录：manifest 的 2026-11-19 `current_device_recheck` 和此前的口头汇报都仍写着“当前没有 linux_rootfs/linux_data/ESP”。那是 11-19 的状态，11-21 的候选 GPT 试验之后已经改变。

## ESP 已写入并双路读回

`out/persistent/installed.json`（2026-11-24 23:23:25）记录：`esp-device.img` 1,073,741,824 B，sha256 `68c28df15bcf55a589441df37e9c5fd94614e2771c437e955f190e3eb0aab898`，`layout: candidate`，`first_lba: 124289792`。

`out/persistent/session/session.log` 显示写入后做了两次独立读回：

- 经 `/dev/sda37` 整块哈希：`68c28df1…`。
- 经 LUN 0 `/dev/sda` 偏移 485,507 块、长 1 MiB：同为 `68c28df1…`。

两次一致，ESP 阶段已安装并验证。构建输入是 `piano-ufs264.kc`、`paclr53.adt`、`kernel.macho.trustcache.3mod` 和 `scratch-data.img`（seed sha256 `48dbc08d…`）。

## root 已写入并读回，Data store 仍是空白

2026-11-22 的记录（`docs/results-2026-11-22-native-root-install.md`）显示 `linux_rootfs` 已接收完整的 17,547,919,360 B root，直接读回 sha256 `db3629e5da012ce76e64c0f42739ada9541ed8cca5adaab32256fc9b64b34ef7`，尾部与两份 GPT 未变，随后回到 HyperOS 3.0.309 槽 A。

`state.json` 记录 `esp_store` 在 `linux_data` 的 LBA 117,735,999，header `state: blank`、`fresh: true`、`store_matches_esp: false`。也就是说 Data seed 还没有写入，Data 持久化未开始。

## Mac 输入盘点

`~/Codex/k273-esp/inputs` 只有 `piano-ufs264.kc`、`paclr53.adt`、`kernel.macho.trustcache.3mod`、`scratch-data.img` 和 `firmware/`；整个 `~/Codex/k273-esp` 下没有大于 100 MiB 的镜像。**没有 root 镜像，也没有签名 v5 持久启动镜像。** 这与既有结论一致：签名 v5 仍缺失，持久 boot 不能开始。

## 当前剩余阻塞

1. 签名 v5 持久启动镜像不存在，写 `boot_a` 的前提不成立。
2. ufs264 内核在 37–52 s 因同步异常自行回 fastboot，死亡点未前移。
3. `oem dump` 通道退化为持续 `Too many links`，需要平板 USB 重新枚举；且每次 reader 启动只能发一条 dump。
4. native k27 锁定提交 `11ac316b…` 仍无权威来源。

本轮没有执行 `fastboot boot`、分区写入或任何重启。