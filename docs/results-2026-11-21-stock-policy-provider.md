# Stock-policy KC provider and connected baseline

The retained stock-policy KC read back with SHA-256 `a927b53d3ebca0e257d945d89a6ba0594835834a30f2a2bef374c83c1a178fbe`. Its NativeUfs descriptor points to `objects.json`, which explicitly includes `host-poll.o` alongside the stock-policy UFS object and NativeUfs startup object. This resolves the import for the host replay. The provider's native macOS provenance remains open.

Retained iPadOS Piano and Pagani BootProgress sources define `void boot_progress_poll(void) {}`. That behavior is evidence for the historical iPadOS configuration. Qualify the macOS provider or establish that the frozen macOS configuration intentionally compiles out the callback before selecting the deployment KC.

A read-only Windows ADB check reported `piano`, `OS3.0.309.0.WPYCNXM`, slot `_a`, and `sys.boot_completed=1`. No tablet boot or flash was issued during this audit. The timed DarwinBoot results still leave post-handoff recovery unverified.
