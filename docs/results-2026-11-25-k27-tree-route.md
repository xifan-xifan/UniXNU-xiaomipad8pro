# k27 tree reconstruction route (2026-11-25)

The locked k27 commit is reproducible from published sources. `unixnu.lock` `pin."k27"` records:

- xnu base: `xnu-12377.121.6` = commit `ac9718fb1af618d5ce8678d0dc6e8a58f252216f` — **verified live**: the tag
  `xnu-12377.121.6` in `apple-oss-distributions/xnu` resolves to exactly that commit (Apple's official OSS mirror).
- baseline series: `patches/xnu-12377` 0001-0054 — published in the UniXNU core repository
  (`UniXNU/UniXNU`, pinned `d830d23b…`), directory `patches/xnu-12377/` (verified present, ~50+ patch files).
- device series: this repository's `patches/k27` (0053-0066) and `patches/k273` — already committed here.
- tree head: `11ac316b789d81fc9f734e15645456f536c05141` on branch `el1/native-variant`, with the built
  objects/recipes (`out/k27`, `out/x12377`) and `out/kc/kc27.bin` inside the tree.

What is *not* published: the built recipes, generated headers and Apple's kernelcache bases. Those are exactly what
`tooling/k273/*` regenerates on a macOS host: `decompress-kernelcache.py` + `ipsw-members.py` derive the 27.3 inputs
from a macOS 27.3 IPSW, `prepare-k27-tree.sh` relinks the kernel, and `build-k273-image.sh` emits the collection, ADT
and RAM images. That removes the 2026-11-25 "no tree, no source-level candidates" blocker at its root: the tree can be
cloned from the public tag, the UniXNU series applied, and the 27.3 build inputs generated from the IPSW.

Open questions being worked: whether applying the published series reproduces tree head `11ac316b` byte-for-byte
(the build scripts refuse any other commit), and where the macOS 27.3 (26C5035r) IPSW comes from on this network.

This finding came from the upstream README (`tooling/k273/prepare-k27-tree.sh`, build §4) after the user pointed at
https://github.com/lenzhang/UniXNU-xiaomipad8pro. No tablet action was taken; the tablet stayed in ABL fastboot.
