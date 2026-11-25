# PMK8550 Gen3 PON history

`piano-pon-history=1` logs one read-only snapshot at every native boot, through
PianoPower's existing shared SPMI lock. It is off by default. It also works with
`piano-power=0`; that combination only maps the arbiter and reads history, without
starting the charger, gauges or ADSP. With normal charging enabled the snapshot
precedes charger discovery and policy work.

The board assigns SID0 SDAM pages `0x7400` and `0x7500` to PON history. Each page
contains 117 FIFO bytes at `+0x4b..0xbf`; a four-byte entry can cross pages or the
ring boundary. The reader preserves two complete `+0x40..0xbf` copies, identity
bytes, and every transaction's result/status. It requires unchanged metadata,
identities and data, a two-page count and an in-range next-write pointer before
decoding all 58 entries. Unknown events and unused entries retain their raw bytes.
It never writes a PMIC register, consumes history, clears status or accesses PBS
trigger registers. Observer-channel command stores are the existing read primitive.

The layout and event codes were studied from the third-party reference
MiCode/Xiaomi_Kernel_OpenSource commit
`45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71`,
`drivers/soc/qcom/pmic-pon-log.c` (SHA256
`87700773cfd244d426c20d1b8166e8b530b1374f1c89ebf947fc69cc2caf8918`),
and the board's firmware device-tree bindings. The implementation is independently
written; no reference driver source is imported. Gen3 does not use the older
PON/POFF reason-register layout. Map ID/version/size and TYPE/SUBTYPE are logged,
without assuming the identity constants of unrelated ADC SDAM pages.

The probe is diagnostic and awaits tablet qualification. A valid FIFO can identify
UVLO, OTST3, PMIC watchdog, reset trigger/type and related PMIC events. PS_HOLD or
SW_RESET alone cannot identify the SoC caller, exclude panic or distinguish EL1
from firmware/hypervisor. History is bounded and may be overwritten or lost on
power failure. Compare complete snapshots across consecutive boots and retain the
producer's host console; a missing or ambiguous record means UNKNOWN.
