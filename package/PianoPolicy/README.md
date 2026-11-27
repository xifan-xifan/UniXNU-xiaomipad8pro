PianoPolicy: piano's laptop-class policy for native macOS 27 (backlog B2 and B5), so that the drivers stay mechanisms. It owns no
hardware. It reads what PianoKeyboard (cover hall, attach, gpio95), PianoPower (the power source), NativeGopDisplay (display power),
the kernel (k27 0062's system sleep and wake mask) and a userland agent (macOS Low Power Mode) publish, decides with Policy.h, and
tells IOPMrootDomain (clamshell, clamshell sleep, AC adaptor, sleep now, user active), PianoThermal (its profile) and PianoA830 (the
GPU ceiling) through their public calls. Inert without the boot argument piano-policy (1: decisions logged, 2: act; the cover only
with piano-policy-lid=1). The page is docs/policy.md; provenance in UPSTREAM. Build: tooling/k27/build-native-piano-policy.sh (host
tests, then the objects); it is in configs/k27-native-kexts.txt.

| File | What |
|---|---|
| `PianoPolicy.cpp` | the IOService on IOPlatformExpertDevice: inputs, the thread, actions, platform functions, `machdep.piano_policy*` |
| `Policy.h` | portable: the rules, the order of the root domain's notifications, the GPU ceiling: client 1 of PianoA830's slots, or the slot shared with PianoThermal when PianoA830 has none |
| `tests/policy_host.cpp` | host tests (every cover x power x display x Low Power Mode x sleep combination, the sequences, the profile through PianoThermal's `Governor.h` to its CPU caps) |
| `SleepLidPolicyModel.h`, `tests/sleep_lid_policy_host.cpp` | offline Goal 9 model and ASan/UBSan tests for lid, freeze-abort and wake recovery |
| `AppleSmc.cpp` | the AppleSMC stand-in (docs/policy.md §12): `AppleSMC` on IOResources (its own personality, inert without `piano-smc`) and its user client `AppleSMCClient`; only `MSHP` = 1 (macOS then offers the Energy Mode picker), every other key not found; `machdep.piano_smc` |
| `Smc.h` | portable: the parameter structure's fields and the answers |
| `tests/smc_host.cpp` | host tests: IOKit's and powerd's call sequences (80 and 168 bytes), writes, refused sizes, `MSHP` off, client types |

The userland half (Low Power Mode, the lock on a cover closed on power): `tooling/native/piano-policy-agent/`.

Status (2026-11-11): built, host-tested, linked into the KC (kernels 0059 and 0062); not run on the tablet. 2026-11-12: no timer in
the normal case: NativeGopDisplay's display power is listened to (its `piano-display-power-messages`), the 5 s tick is only the
fallback for a NativeGopDisplay without messages (logged once) and `piano-policy-display-tick=1`.

## Goal 9 offline sleep and lid model

Run `python3 scripts/ci/self-tests.py --only package/PianoPolicy/tests/` from the repository root. The new suite uses a synthetic
clock and the existing close-policy decision in `Policy.h`. The disarmed fixture represents `piano-policy-lid-arm=0` and
checks that a confirmed close blanks and requests a lock without entering system sleep. Armed fixtures cover both Hall
polarities and require a confirmed open before a close can initiate sleep. A detach discards the previous attachment's
Hall state and lid wake eligibility. On sustained chatter or an unknown reading, the clamshell releases its display hold.
The power button or bounded timer can still end a system sleep.

The fixture uses 10 ms debounce, a 50 ms chatter window and a 2 s default `piano-sleep-max`. The model starts the freeze
ACK window after a successful park of CPUs 1–7, with CPU0 coordinating. It checks display/backlight → Wi-Fi/BT → touch/cover → timers
for both quiesce and restore, including freeze timeout rollback. This order is a host invariant. Native sibling PM callback
ordering is documented in the [sleep/wake audit](../../docs/drivers/sleep-wake-audit.md).

The pinned SM8750 references identify GPIO80 as the Hall input with raw low closed and raw high open. Inverted polarity
is synthetic coverage. The 100 ms freeze-ACK window comes from k27 0062. The CPU4 fixture exercises the reported natural
abort class by withholding that CPU's ACK and letting the window expire. The modeled wake precedence at a single poll is
power-button press, confirmed lid-open, then elapsed timer. See the [keyboard hardware notes](../../docs/drivers/keyboard.md),
[policy notes](../../docs/policy.md) and [k27 0062 sleep implementation](../../patches/k27/0062-k27-piano-sleep-1-macOS-s-system-sleep-as-suspend-to.patch).

**Fix-image gate: HOLD.** Physical lid close/open and uncapped sleep require hands-on owner testing. The fixture requires
a positive sleep cap and runs only in the host test runner. The live kext and boot configuration are outside its scope.
