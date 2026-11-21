# Stock-policy kernel collection replay

The retained owner KC recipe was replayed with the stock-root v2 policy object assembled from the actual OS/SystemOS `manifest.header.bin` files and shared trusted key. The NativeUfs object was replaced only for this host replay; all other registered native driver descriptors and the retained kernel inputs stayed unchanged.

The link completed with 26 native drivers. The resulting KC is 135,806,976 bytes and has SHA-256 `a927b53d3ebca0e257d945d89a6ba0594835834a30f2a2bef374c83c1a178fbe`. This proves the real stock policy symbols can be consumed by the retained arm64e collection recipe. The KC remains an unsigned host artifact; root pairing, DarwinBoot packaging, device execution and recovery are still unqualified.
