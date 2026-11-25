# Contract-bounded buck charging

The `piano-power=2 piano-power-policy=1` path now observes UCSI through the existing
charger GLINK client. A source advertisement and a voltage-request ACK do not
prove a contract. A current, connected PD sink snapshot must include the selected
raw source PDO and decoded RDO. Fixed RDO operating current uses 10 mA units;
PPS uses 20 mV and 50 mA. Malformed, stale or failed reads revoke authorization.
This implementation is host-tested; PD/PPS and runtime health actuator behavior
still need tablet verification.

The observer polls status every five seconds and on port events, including a
connected non-PD SDP sink. Such a sample has a fresh status sequence and age,
with PD contract validity and budget remaining zero. CONTROL readback is only
diagnostic: completion follows serialized CCI command/ACK indications, even when
CONTROL reads zero or an older value. Transport timeouts still quarantine the
unsequenced channel until relink. Status diagnostics distinguish the built-in
observer from the legacy probe, and include stage, CCI and quarantine state.

The first tablet run booted and charged on SDP, but the extra CONTROL-echo check
prevented status completion and voided its actuator precondition. One later gauge
report also contained an invalid all-ones voltage. Gauge voltage/error/failed
command are now exposed per pack. The follow-up spec waits within a shared
75-second budget for a newer valid dual-gauge report; it retains rejected
attempts and never treats a failed read or an older valid report as fresh.

The first buck profile caps HV input at 1600 mA and battery current at 1500 mA,
requires the existing 4400 mV float setting, and reads SC8541 CTRL2 to confirm the
pump is off. It never enables the pump. Missing pump state prevents source voltage
changes. Successful board configuration, fresh dual gauges, temperature, dry LPD,
clear ISC/SOA and the current contract are required for new HV. Voltage changes
wait for reduced-current ACKs and an ICL readback; the new RDO and measured bus
must settle before current can rise. An expected RDO re-read has a bounded
15-second window at ICL 100/FCC 500 mA; a failed read withdraws that exception.
An idle or active UCSI notification suspends authorization until CCI separates a
command completion from a connector event. A healthy existing HV session waits
at ICL 100/FCC 500 for at most five seconds, without renewing the snapshot age or
extending the deadline under notification storms. A pending 5 V restore remains
an obligation across cable and mode changes until its ACK and readback agree.
The existing protected 5 V SDP/Rp ramp remains available without a PD contract.

Boot with `piano-power-health=1 piano-power-hv=1` for the 80/75 vote and fixed 9 V
preference. Mode 2 explicitly selects a 9 V PPS APDO. Firmware safety gates remain
in force. No PD PHY, role swap, second GLINK owner or separate ADSP lifecycle is
introduced. Protocol state and short `buck-*` lines are published alongside the
`PianoCharge` IORegistry dictionary. `Seq` advances with decisions; connection
`Generation` changes on actual invalidation/connection changes; `HealthGeneration`
and `ConfigRevision` advance with complete admin transactions.

Administrators can use the staged `piano-charge-config` command:

```
sudo piano-charge-config 1 80 75 1
piano-charge-config --show
sudo piano-charge-config --save 1 80 75 1
```

The four values are enabled, stop, resume and mode (0=5 V, 1=fixed, 2=PPS).
The kernel validates the entire revision-checked transaction and applies it on its
worker. Stop is 50–100 and resume is 40–stop−1. Disabling health clears only the SOC
vote; thermal and firmware stops remain. Missing gauges retain an established
hold and repeated failures stop controlled charging. Optional saved daily values
are reapplied by a local launchd job; test thresholds are never saved.

For an actuator test when real SOC is below the legal threshold range, a fifth
argument requests an explicit admin inhibit for at most 180 seconds. It uses the
same real stop/resume actuator, keeps SOC genuine and expires automatically. It
proves the actuator/UI chain; it does not prove a natural 80→75 battery cycle.
Restore with `sudo piano-charge-config 1 80 75 1 0` in unconditional cleanup.
The portable regressions include exact 79/80/76/75 hysteresis, leak detection,
firmware disable, atomic rejection, RDO units, generation and transition barriers:
`bash package/PianoPower/tests/run-charge-host.sh`.

The observer initializes each new PPM generation before issuing GET commands:
read VERSION/CCI, clear a latched reset if needed, PPM_RESET with RESET_COMPLETE
polling, enable command/error notifications and complete its CCI/ACK handshake,
then query capabilities and enable the supported charging events. A cold PPM in
notifications-disabled state need only process RESET and notification-enable;
a GLINK WRITE response alone does not complete GET_CAPABILITY. The independent
cold PPM regression enforces that boundary, including delayed reset, missing
notifications, old CONTROL, reconnect and bounded failure. `PianoUcsiMessageInV4`
identifies the initialization trace; `PianoCharge` also exposes initialization,
enabled events, request/reply counts and the timeout layer. The tablet has
confirmed RESET, basic notification enable, ACK and GET_CAPABILITY completion;
the tablet has also confirmed fresh non-PD status and the real stop/resume
actuator. PD/PPS charging still needs tablet qualification.

COMMAND_COMPLETE with CCI `0x80001000` carries a successful 16-byte capability
response. UCSI 1.1 MessageIn is at offset 16 in the 48-byte mailbox, or offset 28
in the GLINK READ_BUF response. Connector count uses seven bits; optional PD/PDO
capabilities do not prevent observing a non-PD sink. A PD contract still requires
advertised PDO-query support and the complete selected PDO/RDO validation.
Locally rejected capability data is acknowledged before quarantine, without
restarting the PPM. The bounded `ucsi-message` trace and `UCSIMessageIn0..3`,
`UCSIMessageLength`, `UCSIMessageCommand`, `UCSIAttributes`, `UCSIFeatures` and
`UCSIConnectors` retain the evidence for the next status-path qualification.

`PianoBuckAlertsV5` identifies the gauge alert decoder. ISC level 3 is an
alarm; bytes outside 0..3 are unknown and clear that field's validity bit.
Unknown safety inputs forbid new HV and cap FCC at 500 mA. They do not create
a new stop vote. SOA nonzero remains an alarm, including an all-ones byte.
A confirmed alarm survives unreadable samples until that field reads clear;
another missing field cannot hide it. `SafetyValid`, raw `Pack0/1ISC` and
`Pack0/1SOA`, `DTPT` and `SafetyAlarmMask` distinguish these states.

## The source list from the charger firmware (`piano-power-pdo-mca=1`, default 0)

This tablet's PPM is UCSI 1.1 with `bmOptionalFeatures` 0 (`UCSIFeatures` 0 in the
saved chg-buck4 and installed-v12 snapshots): it offers no PDO details, so the
observer never sends GET_PDOS and a PD sink status has an RDO but no source
list. Without a list no PD contract is ever valid: the strict policy holds a PD
charger's input at the firmware's 100 mA and never asks for 9 V, whatever
`piano-power-hv` says. With `piano-power-pdo-mca=1` the owner offers, after each
charger-firmware cycle whose PD state (0x2100d) reads SNK_Ready, that cycle's
decoded source list (0x21011 {min mV, max mV, max mA}). A PD sink status then
gets a second, agreeing status read and the list is re-encoded as standard
source PDO words in object order (fixed: 50 mV / 10 mA; PPS: 100 mV / 50 mA,
rounded inward). The list must start with vSafe5V, keep fixed objects strictly
ascending and before any APDO, and stay within 5 A / 21 V; anything else is
rejected. With the option the codec also keeps a list whose object exceeds 5 A within its PD
field (a 67 W 3.3-11 V 6.1 A APDO) and reads that object as 5 A; today one such object
discards the whole list. RDO, PD opmode, sink and connection still come from UCSI alone, and
the RDO must fit its object. A connection or opmode change drops the offer;
two cycles without a ready list withdraw it. A PPM that does offer PDO details
keeps its own list. All later gates (transition barrier, 15 s wait, VBUS
8.0–9.6 V, HV ICL 1600 / FCC 1500, 80/75, CP off) are unchanged.
`SourcePDOOrigin` (1 UCSI, 2 firmware), `FirmwarePDO1..7`, `FirmwarePDOOffer`,
`FirmwarePDORejects` and `FirmwarePDOWithdrawals` in `PianoCharge`, the
`ucsi: firmware source list offered` / `ucsi: PD contract from the ...` lines and
the `PianoPdoFirmwareV1` marker identify it. Host replay:
`tests/pdo_firmware_host.cpp` (part of `run-charge-host.sh`). Not run on the
tablet yet.
