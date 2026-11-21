# Retained k27 baseline replay

UbuntuPiano replayed all 47 retained VM baseline patches in an isolated clone from local Apple import `55d3eeb8d0db929c806232675ad9e53606ee64f6`. The replay tree `d6661f2467e485a06334f6b42c89597c3d2768e6` exactly matches the retained `c2936ef` commit. The patch README records `c97cc109b2b49890091f3fb035777204c2fd1e4b`, which the retained source database did not resolve. The retained working tree separately has 60 tracked files with local modifications.

A dry run of native patch 0053 against the replay failed in `vfs_subr.c`, `machine_routines.c`, and `zalloc.c`. The respective replay blobs begin `229d6a1`, `f456377`, and `9edf8c5`. The patch expects `282c705`, `2b62e95`, and `c831172`. The replay establishes a reproducible starting point for a separate reconstruction. The native prerequisite remains unresolved. No tablet test was performed.
