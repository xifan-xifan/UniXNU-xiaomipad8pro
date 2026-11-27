# Troubleshooting native macOS on piano

The installed reference is **v13.1y12q** on macOS 27.3 (`26C5035r`). Identify the build and boot medium before comparing results.
The daily system reached the login window at **+32 s** on four recorded disk boots, each with the owner's Data store resumed.
Use `data-persist=0` for routine RAM tests and follow the partition safety rules for any authorized disk maintenance.
[Installed boot results](results-2026-11-27.md#installed-boot) · [Partition safety](persistent-boot.md#partition-safety)

## Boot arguments missing from `kern.bootargs`

`kern.bootargs` uses a 1024-byte buffer, including the terminating NUL, and clips at a whole-argument boundary. A key near
the end can be absent from this view even when XNU applied it. On v13.1y12q, read `sysctl -n kern.piano_bootargs` for the
full ~3.8 KB merged line from DarwinBoot and the ADT tail. The shell helper `piano-bootargs.sh` tries the new sysctl first
and falls back to the legacy view only on the unknown-OID response from a kernel without it. Other sysctl errors fail the
read. If a late key is also absent from the full line, compare the intended entry and ADT tail with the build record. If 0076
should be present and the sysctl is missing, use `tooling/k27/check-piano-bootargs-kc.py --kc` with the built collection to
check patch inclusion. [Boot-argument details](bringup-debugging.md#52-the-boot-arguments) ·
[Shell reader](../tooling/native/piano-usbnet-address/piano-bootargs.sh) · [Kernel collection check](../tooling/k27/check-piano-bootargs-kc.py)

## Chrome resets a RAM boot

`LT-B4` is the known volatile Data exhaustion reset. In the `RXB` runs on `ram16g-tier3w` and its tier-off control,
`install-chrome-pad.sh` completed and Chrome launched on RAM-backed Data before the system reset to ABL. The paired runs
classified this as a test-environment failure. Keep RAM qualification on volatile Data and use the installed line for Chrome
qualification. [RAM failure and control](drivers/ram-extra.md#turnip-0015-private-tier-ram-qualification-ram16g-tier3w-2026-11-27)

The installed v13.1y12q check launched Chrome without GPU, WebGPU or sandbox flags. All **17/17** processes reported secure
V8 sandbox mode (`SandboxMode` bucket 0, `ReservationSizeGB = 1152`) on the 47-bit kernel. WebGPU opened **6/6** contexts
on `qualcomm/adreno-8xx` with `fallback_adapter = false`, correct compute and exact pixel readback. WebGL 2 measured
**37.89 fps** through ANGLE Metal at 3200 × 1738. Use those signals to check a suspected fallback or sandbox regression.
[Installed Chrome results](results-2026-11-27.md#chrome)

## `GUI_TCC_EXISTING_POLICY` on installed Data

When the existing SSH-wrapper Accessibility policy in `TCC.db` differs from the expected grant, the helper preserves the
existing row and returns `GUI_TCC_EXISTING_POLICY` with exit `2`. The trust gate marks GUI Wi-Fi and both brightness checks
`VOID`. Leave the policy in place and have the owner review the Accessibility permission through the normal macOS
authorization flow. Keep the GUI checks blocked until the same console-user probe reports trusted. Never edit or delete
existing TCC rows or bypass TCC to clear this result. [Session policy check](../tooling/native/piano-gui-checks/session.sh) ·
[GUI trust probe](../tooling/native/piano-gui-checks/run.py) · [Policy fixtures](../tooling/native/piano-gui-checks/tests/test_session_tcc.py)

## VideoToolbox stalls at `cancel-recreate`

On the pre-fix images, `video-v6 --qualify` timed out with `V6_TOOL_EXIT=124` after 12 phases. Eight AVC frames had been
accepted and four were pending. Native close completed, but the guest lost the callbacks needed to make progress.
Commit `a426ce70` defers the `dinv` reply until the pending terminal `Demt` dispositions are acknowledged. Verify the fix
on the qualified RAM build `v131y12u-ram1` or a build containing the commit. The recorded RAM run used the standard
256 MiB arena and passed **13/13** phases with `V6_TOOL_EXIT=0`, **4/4** terminal callbacks and **30/30** images after
recreation. Installed **v13.1y12q** predates this fix. [Cancellation analysis](drivers/video-hevc-cancel.md) ·
[RAM qualification](results-2026-11-27.md#videotoolbox-pending-cancellation)

## Wi-Fi power, rejoin and country code

- **Radio toggle.** `Off (forced)` from `networksetup` points to missing SystemConfiguration AirPort service or interface
  records. Wait for the service and check CoreWLAN power and GET19. On a diagnostic RAM image, the boot-ON guard can return
  `EBUSY` on SET19 OFF. Use the documented RAM power arm for a physical-off test and keep the debug connection over USB.
  [Wi-Fi power and desktop handoff](drivers/wifi-menu.md#10-user-disconnect-and-radio-power-2026-11-17-tablet-qualification-pending)
- **After ON or wake.** Check the radio's stop and restart serials, fresh scan, airportd's SET20 association request, an
  authorized link on `en0` and a current DHCP lease. Scan results and `GET103` errno `2` without a fresh SET20 match the
  saved y12l no-rejoin case. A helper kickstart can skip on a receipt from the earlier join. A reboot recovered the saved
  case. The short installed off/on check and the v13.1y12m Wi-Fi and USB wake recovery passed. The longer, repeated radio
  toggle failure still needs a paired trace. [Radio toggle analysis](drivers/wifi-toggle-rejoin.md) · [Sleep and wake results](power.md#status-as-of-2026-11-27)
- **Country code.** A nil public CoreWLAN value can reflect location privacy filtering. Check Apple80211 `COUNTRY_CODE`
  GET51 and the firmware regulatory and channel-list logs. GET51 reports the firmware country. SET51 accepts a well-formed
  request for that same country and refuses a different one. Use the normal macOS location authorization flow for the UI
  reading. [Country-code behaviour](drivers/wifi-menu.md#desktop-power-handoff) · [Apple80211 handler](../package/PianoWifi/WifiApple80211.h)

## Bluetooth preflight is not ready

Use the read-only preflight in the Bluetooth owner checklist. Allow the default 90 s settling window for the matching
`piano-bluetooth` Path A runtime and `bluetoothd` restart. Check `piano-bt=5`, all four runtime hashes, a running daemon with
a PID, the published `IOBluetoothHCIController`, `piano-bt-probe --status` with `power=1`, and System Profiler showing
Bluetooth On. Confirm a positive `piano-bt-h4-le-masks-patched` counter and LE event-mask bits 6 and 11. The grader accepts
the same-boot console as a fallback if the mask line has aged out of the unified log. `NOT-READY` means a failed gate or
settle timeout. `VOID` means incomplete evidence. Installed v13.1y12q returned `BT_PREFLIGHT READY` with **32/32** checks
after 48 s. Keyboard and mouse pairing still require the attended check. [Bluetooth owner checklist](tools/bt-session.md#before-the-owner-sits-down) ·
[Path A runtime](../tooling/native/piano-bluetooth/README.md) · [Installed preflight results](results-2026-11-27.md#bluetooth-preflight)

## RAM image or kernel collection fails preflight

Before a boot, run the verifier on the build JSON and the exact image. It checks Android v2 header and section bounds,
the image ID, FD and DTB, payload hashes, addon/ADT consistency, and effective boot arguments and Data store settings.
Use the matching reference, FD and DTB for an archived build. Supply `--avbtool` and `--pubkey` to check a signed image's
AVB footer. Investigate any `FAIL` with the matching build inputs before booting. The regression suite runs entirely on the
host with fixture images.

```sh
python3 tooling/bootimg/verify-ram-images.py out/ram/build-A-bundled.json
python3 tooling/bootimg/test-verify-ram-images.py
```

[Image verifier](../tooling/bootimg/verify-ram-images.py) · [Verifier fixtures](../tooling/bootimg/test-verify-ram-images.py)

For the default collection, compare the build's descriptors and native-placement record with the manifest's **27 native
kexts**, including `PianoIR`. The KC wrapper checks the kernel segment layout before linking, validates the native
descriptors and checks the link log for VM-era bundles. On `layout CONFLICT`, regenerate `k27/segaddrs.sh` with the layout
tool using the matching `KC_BASE` and kernel, then relink as the diagnostic directs. Use the rebuilt collection and its
matching payload record for the image check. [Default kext manifest](../configs/k27-native-kexts.txt) ·
[Collection builder](../tooling/kc/build-kc-k27.sh) · [Collection layout and headroom](kc-space.md)
