# Deployment input recheck (2026-11-21)

Windows platform tools from the retained Android workspace detected piano over ADB. HyperOS reported `OS3.0.309.0.WPYCNXM`, slot `_a` and `sys.boot_completed=1`. Android shell permissions denied the sysfs start and size queries, so this run established no live geometry. No reboot or tablet write was issued.

The existing Mac connection was reachable and the retained stock-window SHNGROOT image was present at 17,547,919,360 bytes. The previous verified readback remains the content evidence. The maintainer Git remote advertised HEAD `2438556c77c90c22c7b76e57eb8b40c44b35f1af`, equal to the plan baseline.

The [SystemOS builder](../tooling/k273/make-systemos.sh) validates both `overlay/systemos-shims/sbin/mount` and `overlay/systemos-shims/sbin/fsck` against the locked k27 tree before creating an image. The checked stock-window image lacks those shims. Reusing that image for a desktop candidate would leave this dependency unresolved. A shim-bearing SystemOS would also require a matching UUID in the boot configuration and policy for the resulting window bytes. Firmware-only unattended return has device evidence after ExitBootServices. Recovery after DarwinEnter and XNU takeover remains unverified.
