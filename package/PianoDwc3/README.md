# PianoDwc3

A USB device-mode debug channel for native macOS 27 on piano: the DWC3 controller behind the USB-C port, taken over from
the UEFI in device mode, enumerates on the Mac as CDC-ACM serial ports.

- port 0, the kernel console: the console log streamed live (replayed from boot when the Mac opens the port) and a kernel
  monitor (`help`, `usb`, `ioreg`, `peek`, `args`, `reset`, `panic`, ...);
- port 1, the user pipe `/dev/pianousb` (`piano-usb-shell=1`), for `tooling/native/pianousb-shell` (a shell) or `log stream`;
- port 2, only with `piano-usb-hid=1`: input from the Mac (`tooling/native/piano-input.py`) into a keyboard and a pointer HID
  device (`PianoUsbHID`, PianoKeyboard's report descriptors);
- only with `piano-usb-net=1`: a CDC-NCM network function after the ports, an Ethernet link to the Mac (the Mac's own NCM
  driver; on the tablet `PianoUsbNet`, an IOEthernetController, not built-in: en0 stays PianoNullEthernet's).

Design, hardware facts, boot arguments, test plan and the Mac-side commands: `docs/drivers/usb-device-debug.md`.

| File | What |
|---|---|
| `PianoDwc3.cpp` | the IOService on IOPlatformExpertDevice; read-only probe, takeover, log tail, monitor, `/dev/pianousb`, the HID devices and their command thread |
| `PianoUsbNet.cpp`, `UsbNet.h` | the network function's macOS side: the IOEthernetController, its frame queues and receive thread (`piano-usb-net=1`) |
| `Dwc3Gadget.h` | portable DWC3 gadget core: event buffer, EP0 (chapter 9, CDC-ACM and CDC-NCM requests), bulk transfers, NTBs, EP0 trace and watchdog, EP0 left with SET STALL and the controller recovery (soft reset) of a wedged core (`docs/drivers/usb-device-debug.md` §16.1) |
| `UsbDescriptors.h` | composite device, two or three ACM functions (IAD) and optionally the NCM function, USB 2.0 high/full speed |
| `Ncm.h` | NTB16 writer and parser, NTB parameters, NCM request codes, the frame queue, portable |
| `TxFifoPlan.h` | the TX FIFO layout of the IN endpoints in use (with the network function), portable |
| `HidInput.h` | the input port's line protocol (K/C/P/R/?) and line framing, portable |
| `SmmuStream.h` | SID 0x40 through a VA64 bypass context bank of the apps SMMU |
| `UsbIdle.h` | the USB thread's wait tiers (busy / open / configured / no host) and the core interrupt handler's decision, portable (`docs/power-irq.md`) |
| `UsbHandoff.h` | the controller hand-off to PianoUsbHost's host mode (`piano-usb-handoff=1`, default off): the platform function's asks and states, the USB thread's decision, portable (`docs/drivers/usb-host.md` §6.6) |
| `UsbCold.h` | the cold start (`piano-usb-cold`, default auto): the inherited state read and judged, then GDSC/clocks, stop, the rails (RPMh under PianoA830's DRV-2 lock), the eUSB2 repeater and the M31 PHY from this driver's own tables; the test hook (`piano-usb-cold-sim`) and the link watch, portable (`docs/drivers/usb-device-debug.md` §17) |
| `Dwc3Regs.h`, `PianoUsbHw.h` | register facts, with their sources |
| `tests/` | host tests: the gadget core (2 and 3 ports, with and without NCM), ACM descriptor bytes, EP0 recovery, sleep and re-enumeration, NTB16 framing, TX flow control, HID reports and rings, SMMU setup, the log cursor, idle tiers, hand-off and cold start. `tests/run.sh` and the standalone C++ build lines use ASan, UBSan and warnings as errors. |

`tests/usbnet_txflow_model_host.cpp` exercises the daily `piano-usb-net-txflow=1` contract by moving 69 MiB through the portable
frame rings and gadget with delayed host completions. It also checks NTB bounds and reassembly, stale completions, and reconnect
after reset, unplug, sleep, and a controller loan. The offline receiver enforces disjoint datagrams and table ranges.
`tests/dwc3_composite_churn_stress_host.cpp` checks NCM, console and shell bytes, HID reports, EP0 recovery, and transfer ownership
across repeated resets and controller loans. Live throughput and hardware handoff remain tablet qualification items.

Build: `tooling/k27/build-native-piano-dwc3.sh` (host tests, the objects, the import check, the PianoUsbNet vtable check). PianoDwc3 is in
`configs/k27-native-kexts.txt` and inert without `piano-usb`. Sources and licences: `UPSTREAM`, `LICENSE` (BSD-2-Clause-Patent).
Battery: the USB thread's wait adapts to the link (`piano-usb-idle=0`: the old 1 ms loop); `piano-usb-irq=1` adds the core's
interrupt, SPI 133, through `package/common/PianoIrq.h` (`docs/drivers/usb-device-debug.md` section 15, `docs/power-irq.md`).
Host mode: with `piano-usb-handoff=1` PianoUsbHost (levels 3/4 with `piano-usbhost-handoff=5|6`) may ask this driver to release
the controller (the device controller stopped, its event interrupt masked, the USB thread parked) and to reclaim it (the clocks
re-read, the whole takeover again); without it the controller is this driver's for the whole boot. The VBUS/session override store
re-reads the core's clocks right before it.
Normal boot path: the takeover no longer relies on what ABL fastboot leaves. `piano-usb-cold=auto` (default) reads the inherited state
and runs a cold start when it is not a running device controller, plus a link watch (no host within 15 s: a cold start; at most 3 a
boot). It takes PianoA830's GCC vote and DRV-2 lock words, so PianoA830 is a library of `PianoDwc3.json`. `piano-usb-cold=0` is the
takeover of before.
