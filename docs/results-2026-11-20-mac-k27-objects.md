# Mac 上的 k27 编译交接（2026-11-20）

Apple clang 17.0.0 从冻结的 XNU 源码和生成输入编译出 1,093 个目标文件。回传后逐文件核对 SHA-256 和 Mach-O 文件头，全部为 arm64e 内核子类型 `0xc0000002`。Mac 内核链接、26C5035r 段布局及平板验收均待开展。WSL 内核的 32 KiB 布局差额仅适用于该 WSL 产物。

Mac 编译使用 `arm64e-apple-ios27.0` 和 `XNU_VM_GUEST=0`。旧的 `assym.s` 缺少 Apple 工具链使用的指针认证常量。重新编译冻结树中的 `osfmk/arm64/genassym.c` 后，Apple clang 输出 161 项布局常量，其中 14 项以 `.byte` 表示单个 ASCII 数字。首次转换遗漏这 14 项，导致四个汇编文件失败。修正转换后重新编译了全部 27 个汇编文件，全部通过。`mac-full-results.json` 保存首次结果，最终结果以 `mac-verified-result.json` 和归档内的对象清单为准。

本机接手目录为 `out/k27-independent-20261120/host-run/`。其中 `mac-object-checkpoint.tar.gz` 长 67,178,724 字节，SHA-256 为 `3c04ed4434e30a079fd604872334c63d4d4d846229b7078f296e7b679c42e09b`。归档含目标文件、每个文件的校验值、生成的 `assym.s`、三个编译脚本和日志。上一级的 `mac-path.txt` 记录 Mac 上的临时目录。SSH 仍使用 `mac-lan` 的账号与密钥，本次用同机 mDNS 名称连接，并用原主机名对应的已可信密钥严格校验。

恢复工作时先核对 `firehose_kernel`、`corecrypto`、`ArmDTPlatform`、`ArmCpu`、`libpthread_kernel` 五个外部静态库的 ABI 与来源。对构造器目标执行 XNU 的 `setsegname` 步骤，再链接并复测 Apple mac17j 布局。此次构建只用了主机，没有操作平板。
