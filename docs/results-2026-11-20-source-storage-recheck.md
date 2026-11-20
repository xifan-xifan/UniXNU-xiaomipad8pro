# Source and storage recheck (2026-11-20)

This was a read-only check. No tablet command, reboot, write, userdata read, or backup was performed.

The live `main` ref for `lenzhang/UniXNU-xiaomipad8pro` remains the selected baseline `2438556c77c90c22c7b76e57eb8b40c44b35f1af`, confirmed by `git ls-remote` and the GitHub REST API. Issue #1 remains open for second-device RAM validation; PR #2 remains an open staged packaging draft. Neither is evidence of a completed native boot on this tablet.

The exact native k27 pin `11ac316b789d81fc9f734e15645456f536c05141` remains unavailable through public GitHub sources checked today: commit search returned zero results, the Droid-VM/macOS-DroidVM commit endpoint returned HTTP 422 (`No commit found for SHA`), and none of the eight listed public forks returned that commit. This check does not rule out an unlisted private archive or an inaccessible repository. Continue the frozen independent build without claiming source equivalence.

The Ubuntu mainline release API still reports v1.6.1 (published 2026-11-18) as latest. Its `tools/piano-switch.py` on `main` is still Git blob `c504b603ba1cb3629057afeb501afe5740819766`, last changed in commit `ec551c584c8080cfbaf20797a0ee00e8978e6538`. The existing locally verified v1.6.1 tools archive was reused; no download was made. This release targets HyperOS 3.0.303, not 3.0.309. The script audit establishes compatible Fastboot preflight fields only; successful-slot/retry validation and readback are absent, and no image exchange was attempted.

The existing E: and F: GPT boundary capture sets both passed `tooling/persist/audit-gpt-space.py` again. Each of six LUNs passed capture hashes and primary/backup GPT header and entry-array checks. LUN 0 (`sda`) still has no free GPT extent; the largest secondary-LUN gap is 16,175,104 bytes on `sde`, too small for the native layout. These are boundary captures, not userdata backups. The owner's report that HyperOS 3.0.309 was factory-reset and has no personal data does not change the GPT map.

No concrete storage WRITE-PLAN can yet specify final ranges: the matching root image, its exact size/hash, compatible Data seed/store sizing, and completed boot/KC set are not available. Online F2FS shrink remains unqualified. Do not begin a desktop boot that assumes the missing UFS root/Data, or a partition/write operation based on the author's historical LBAs or sizes.

The configured mac-lan alias failed name resolution during this check. The later [resumption audit](results-2026-11-20-resume-audit.md) records successful read-only Mac access through the alternate SSH configuration and the remaining source and image dependencies.

PR #2 confirms the k27 tree is intentionally a local-only input. Its CI workflow accepts an optional `PIANO_K27_TREE_URL`, skips the k27 stage when unset, and documents that the public runner cannot reconstruct the tree. The workflow has not published a URL or artifact in the reviewed draft. This is direct upstream evidence that the missing source is an external dependency, not a checkout step omitted locally.

A fresh API check shows PR #2 is still open and draft, unchanged since 2026-11-16 15:26:10 UTC. It still contains only the workflow, CI packaging note, and fetch-pins script. Issue #1 has one maintainer comment pointing to that draft and no k27 source or artifact attachment. No new upstream source became available.
