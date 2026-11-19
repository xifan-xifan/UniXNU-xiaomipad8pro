# Upstream Pointer Check

Audit date: 2026-11-19. Read-only remote pointer check; no dependency was fetched or changed.

The configured device source `https://github.com/lenzhang/UniXNU-xiaomipad8pro` still points `main` to `2438556c77c90c22c7b76e57eb8b40c44b35f1af`, the plan baseline.

The current public DroidVM pointers are unchanged from the recorded lookup:

- `main`: `524d27bf57ae92375aa39e3a81e11727c08973ff`
- `macOS`: `854aac22da246ae8ddacf253e19ad9b04c37d2dd`
- `wip/hackintosh`: `f7d070fe55d01c08f3a5862ba446887968679773`

The requested `el1/native-variant` ref was not advertised by the remote. No new upstream pointer supersedes the locked native input, and no dependency pin was changed.
