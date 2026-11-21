# Archived input integrity and device state (2026-11-21)

All 12 files listed in the prior workspace research manifest `work/research/2026-11-19/native-source-manifest.json` match their recorded lengths and SHA-256 digests in the adjacent `native-upstream` directory. The archive identifies device baseline `2438556c77c90c22c7b76e57eb8b40c44b35f1af`. These checks cover the archived references. The native k27 tree and the two SystemOS shims remain missing.

ADB returned `OS3.0.309.0.WPYCNXM`, `_a`, and `sys.boot_completed=1`. This check issued only property reads. The renewed upstream Git request from Windows failed with a connection reset. The alternate SSH request was canceled after it stalled. The previous successful ref observation remains the current evidence for upstream HEAD.

The Mac host `fsck_apfs` usage distinguishes `-n` verification, `-q` checkpoint checks and `-y` repair. No device was passed to that command. For the launchd `-fy` task, the outstanding step is to establish the resolved target and underlying write policy before testing a repair path. The earlier read-only APFS results cover the standalone images. Composed root device selection and exit status handling remain open.
