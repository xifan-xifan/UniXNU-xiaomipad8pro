# Stock-window SHNGROOT v2 assembly

On November 21, 2026, Mac assembled a host-only SHNGROOT v2 candidate from the retained unmodified 26C5035r windows and matching block tables at `/private/tmp/piano-stock-root.1ubLtk/root.img`.

The assembled layout was 17,547,919,360 bytes. Readback passed for all four entries:

- OS: offset `0x100000`, 14,493,417,472 bytes, SHA-256 `40d8282eb8650da9b4d7437a981764c48d47ee4bdc5b8b362248f25ecbce0e70`
- SystemOS: offset `0x35ff00000`, 2,917,138,432 bytes, SHA-256 `14330ed990fe7022114b10718f4defe3049d49b6e5d52fd3ac71a487541cd1f3`
- OS.vroot: offset `0x40dd00000`, 113,229,824 bytes, SHA-256 `f10b77a0b2f0475c296a66c69ec3c1d26598dbca3e6b09751a0b1cc31af533b0`
- SystemOS.vroot: offset `0x414900000`, 22,790,144 bytes, SHA-256 `48402d8eccea26cd5e226830f97a08fe3c1d8d2265bdd38977c39b761a79d608`

The complete host image SHA-256 is `db3629e5da012ce76e64c0f42739ada9541ed8cca5adaab32256fc9b64b34ef7`. This does not prove owner policy registration, native mount/fsck behavior, XNU handoff, or device boot.

A follow-up compiled the retained pinned VerifiedRoot C parser on the Mac and supplied SHA-256 and RSA verification through Python callbacks because the OpenSSL 3 development prefix was missing. For each window, the test joined the archived signed header with the table read from root.img and opened the complete envelope with the corresponding archived public anchor. It verified all 3,538,432 OS blocks and all 712,192 SystemOS blocks directly from root.img. A one-byte change to the final block in each window was rejected. The report is retained at /private/tmp/piano-stock-root.1ubLtk/verified-root-readback.json. Kernel crypto and registration remain device checks.
