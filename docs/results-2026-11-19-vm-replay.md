# DroidVM k27 Replay Check

Audit date: 2026-11-19. The check used only disposable host directories and did not access or modify the tablet.

The verified XNU base `ac9718fb1af618d5ce8678d0dc6e8a58f252216f` was copied from the retained, archive-matching tree. The 47 patch files extracted from `external/macOS-DroidVM-xnu12377-patches.tar` were applied in filename order with `git am --3way`; the command exited `0` and produced 47 patch commits after the base commit. No patch conflict or failed hunk was reported.

The disposable replay's final tree hash was not accepted as a comparison with the archive README's VM tree `c97cc109b2b49890091f3fb035777204c2fd1e4b`. The replay ran in a Windows checkout, where file mode and checkout normalization can differ from the Linux environment that produced the published hash. The replay therefore proves ordered patch applicability against the retained base, not byte-identical VM provenance.

This replay contains no native-only `el1/native-variant` source and does not test native prepared tree `de16623d957cccff6dfb78d8eca4a79842af9702`. It cannot be used as the native k27 source input.
