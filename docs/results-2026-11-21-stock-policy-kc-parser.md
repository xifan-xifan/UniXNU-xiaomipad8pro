# Stock-policy KC parser check

The production DarwinBoot kernel parser accepted the stock-policy KC replay at `/tmp/piano-stock-kc.fzeopbza/stock.kc`. The parser's built-in malformed-range and arm64/arm64e capability tests also passed. This checks Mach-O/fileset structure and bounds only; it does not prove signature acceptance, root mounting, or device execution.
