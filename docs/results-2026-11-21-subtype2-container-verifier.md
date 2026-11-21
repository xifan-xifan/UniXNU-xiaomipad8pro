# Subtype-2 RAM container verification

The repository verifier compared `47125fbd…` with the frozen Ubuntu v1.1.0 maintenance container (`54001a50…`). Two header fields differed: `os_version` was 536871330 and `recovery_dtbo_offset` was 32505856. The reference has zero in both fields and the candidate contains no recovery DTBO section.

A separate host copy with only those two fields set to zero passed all checks, including the archived FD, packaging DTB, section ID, FAT payload, subtype-2 KC, ADT, trust cache and scratch Data hashes. Its SHA-256 is `c957f87278d22e2f586da33d065176fbe5fce60d38b8ebda6940ef03bdc2973d`. The copy, reconstructed build record and logs are in `out/subtype2-verification/`.

The verifier confirmed the image is unsigned. It checks packaging and payload identity. Actual firmware execution, the authenticated root pairing and the unattended recovery of this candidate remain unverified.
