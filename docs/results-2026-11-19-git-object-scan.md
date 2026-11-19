# Historical Git Object Scan

Audit date: 2026-11-19. Read-only host scan; no device access or write was performed.

The retained `E:\codex\2026-11-01\ni\work` tree contains 84 distinct Git repositories. Each repository was queried directly with `git cat-file -t` for both the locked native commit `11ac316b789d81fc9f734e15645456f536c05141` and the recorded prepared-tree object `de16623d957cccff6dfb78d8eca4a79842af9702`.

Neither object exists in any of the 84 repositories. The scan includes the retained DroidVM checkout, historical piano checkouts, downloaded upstream repositories, and source checkouts. Lock files and `UPSTREAM` markers that mention these hashes are metadata only; they do not imply that the Git objects are locally recoverable.

This closes the current local Git-object lookup path. The native source gate remains unresolved and must be satisfied by a separately supplied archive/tree or by a reconstruction whose resulting tree is independently proven to match `de16623d...`.
