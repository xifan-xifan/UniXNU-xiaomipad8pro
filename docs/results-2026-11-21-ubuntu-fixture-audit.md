# Ubuntu fixture audit (2026-11-21)

UbuntuPiano contains two temporary `overlay/systemos-shims` directories. Both are synthetic test fixtures: their `mount` and `fsck` files contain only fixture text, and one fixture intentionally fails the locked-source hash check. The associated persistence regression stopped at the NAS-backup gate. They are not native macOS boot helpers and were not copied into any image.

The real Mac input directory remains `/tmp/piano-root-26C5035r.593r2m`; no matching shim source was found there or in the clean independent UniXNU checkout. No tablet write was made.
