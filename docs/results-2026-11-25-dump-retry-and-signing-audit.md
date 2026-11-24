# dump 重试与签名链审计 — 2026-11-25

## dump 通道仍未恢复

`tooling/native/dump-first.ps1 -Addr 8c0004000 -Size 100` 执行了一次无人值守 reader 启动：`fastboot boot reader.img` 后第一条设备命令即 `oem dump`，返回 `FAILED (Status read failed (Too many links))`。失败后平板自行离开 fastboot 并回到 HyperOS `OS3.0.309.0.WPYCNXM`，`sys.boot_completed=1`，没有分区写入。

这与 2026-11-25 早先的记录一致：同一地址早前能读 256 B，现在第一条命令就失败，`oem log` 仍正常。主机侧 USB 接口没有重新枚举前，不再重复同一实验。

## 签名链：私钥与 v5 镜像都不在本机

- `E:\codex\2026-11-01\ni` 顶层没有 `keys` 目录；`work/piano-macos-c146/private` 只有空的 `.keep`。历史 `keys/piano-boot-signing.avbpubkey`（AVB sha1 `f5151930…`，与本机 boot_a 的 key 相同）只有公钥记录，私钥不在。
- `work/piano-macos-c146/artifacts/ARTIFACTS.md` 明确写着签名“done on the signing host, where the private key stays”；`tooling/persist/sign.sh` 在签名主机上生成 `-signed` 镜像。本机没有该脚本可用的私钥。
- Mac `~/Codex/k273-esp` 下没有大于 100 MiB 的镜像；`find ~ -maxdepth 6` 也没有 `boot-piano-macos-persistent*` 或 `*persistent*v5*`。
- 本机与历史工作区都没有 `boot-piano-macos-persistent-v5-signed.img`。此前 `docs/persistent-boot.md` 记录的“v5 已签名安装、27.3 在其下启动”是 2026-11-13 的历史事件，对应文件已不在任何可访问位置。

结论：持久 boot 的写入前提仍然缺失。要么从签名主机取回 v5 签名镜像，要么取得与设备 boot_a AVB key 匹配的签名能力；在此之前不能写 `boot_a`。

## 当前已就绪的存储状态

候选 GPT、`linux_rootfs`（root 17,547,919,360 B，读回 `db3629e5…`）和 `esp`（1 GiB，双路读回 `68c28df1…`）都已落地并验证。Data store 仍是 blank，`store_matches_esp: false`。也就是说，除签名启动镜像外，持久部署所需的磁盘侧准备已经完成。

本轮只执行了一次 `fastboot boot` 和一个失败的 `oem dump`，没有写入。