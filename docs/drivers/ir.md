# IR transmitter: SE15 / GPIO29 (`PianoIR`)

**Emission-free bring-up passed on RAM `v131y12u-ram1` (2026-11-27).** Actual emission awaits an attended receiver
check. The kext is listed in `configs/k27-native-kexts.txt`.
Only `piano-ir=1` and `piano-ir=2` pass `probe()`. With the boot argument absent, PianoIR maps no MMIO, allocates no DMA and
starts no worker. The userland layer includes the `piano-ir` CLI without a launch job. The Ubuntu port drives the same
emitter through LIRC (`ir-ctl`).

## 1. Hardware

The emitter is the MOSI line of QUP2 serial engine 7 ("SE15", `spi@89c000`), GPIO29, active high, with no supply of its own. The
waveform is the SPI bit stream at ~1.92 MHz: a mark is the carrier built from bits (period round(rate / carrier) samples, the
first duty % of them high, the phase restarting at every mark), a space is zero, and a zero word ends every transfer. Pins
gpio28 MISO, 29 MOSI, 30 CLK, 31 CS (function 1 `qup2_se7`); between sends GPIO29 is a GPIO driven low. Clocks: GCC vote word
0x52010 (bits 0-3 the QUP2 wrapper, 17 s7), the s7 DFS RCG 0x1e8b8. SE DMA through stream 0x423 (bypass bank); Linux and Android
use GPI DMA instead. Facts and sources: `package/PianoIR/IrHw.h`, `UPSTREAM`.

## 2. Levels and boot arguments

| Arg | Default | Effect |
|---|---|---|
| `piano-ir=1` | 0 | read-only survey: pins as found, the vote word and five branches, the s7 rates (DFS table), SE15's FW/IF_DISABLE/STATUS/HW_PARAM_0 only when clocked, the QUP2 version, the SMMU entry of 0x423; `survey done: N loads, 0 stores` |
| `piano-ir=2` | 0 | the survey, then GCC votes under PianoA830's lock, stream 0x423 bypass if configured, GENI init (SE DMA when the FIFO is off), the clock pick and the **emission-free loopback**. The driver checks the GPIO29 mux, output latch and pad before it starts loopback. Only an administrator can request sends after `ready`. |
| `piano-ir-test=N` | 0 | for N from 1 to 20, send N bursts of 500 ms (38 kHz, 40%) after `ready`, each followed by 1.5 s dark, then one NEC frame (0x00/0x45) |
| `piano-ir-sample=1` | 0 | sample GPIO29's pad during each send (edges, high share vs the waveform's); with the test, a 50 ms idle sample first (known-negative) |
| `piano-ir-smmu=0` | 1 | leave the SMMU alone (SE DMA then needs 0x423 to bypass already) |
| `piano-ir-pins=4` | 1 | hand all four SE15 pins to the engine during a send (Linux's default state), then restore the non-MOSI muxes. The default only changes MOSI. |
| `piano-ir-delay=S` | 0 | wait S s before the survey |

Each raw request takes an odd count of up to 1024 durations, starting and ending with a mark, totaling at most 500 ms.
The waveform must fit 32 KiB (about 136 ms at 1.92 MHz). Carrier is 30–56 kHz and duty is 10–70%. The separate
`piano-ir-burst` request used by `blink` caps each burst at 500 ms and splits it into transfers of up to 120 ms, sized
to the actual sample rate.

## 3. Tool

`sudo piano-ir status`; `sudo piano-ir raw [--carrier HZ] [--duty PCT] 9000 4500 562 ...` (ir-ctl `+`/`-` signs and `pulse`/`space`
words accepted, `-` reads standard input, a trailing space is dropped); `sudo piano-ir nec ADDR CMD [--repeat N]` (extended
address above 0xff; repeats 108 ms apart); `sudo piano-ir blink [SECONDS]`. Exit 0 sent, 1 failed, 2 usage / no service.

## 4. Expected lines (piano-ir=2)

```
PianoIR: start: piano-ir=2, ...
PianoIR: smmu: stream 0x423: ... -> bypass bank N  /  smmu: stream 0x423: bypass set
PianoIR: pins as found: gpio28 ... gpio29 ... ; pins: gpio29 (the emitter) dark (GPIO, not driven high)
PianoIR: gcc: vote 0x52010 = ... ; clock: s7 rcg ... (DFS on, 8 levels): rates 0:... 7:... Hz
PianoIR: se15: FW ... (protocol 1: SPI), IF_DISABLE 00000001 (FIFO disabled: SE DMA), ...
PianoIR: survey done: N MMIO loads, 0 stores; 0 map failures
PianoIR: gcc: votes ... (checked under lock, ...); branches on
PianoIR: geni: SE DMA, 32768-byte aligned-buffer halves
PianoIR: clock: level L ... Hz, GENI divider D -> R Hz sample rate (exactly 1.92 MHz)
PianoIR: loopback PASSED (rc 0, N us): 64/64 bytes back, moved 64/64 (GPIO29 held as GPIO)
PianoIR: stage 3: ready
PianoIR: send #K sent (...): ... wire N us (expected N), moved B of B bytes; pad: S samples, E edges, high H% (waveform W%)
PianoIR: pins after send: gpio29 ctl ... io ...
```

## 5. Qualification

The host suite checks encoding, bounds, GPIO park readback and GENI transfer and recovery against register models.
The ASan/UBSan frame safety test models the RAM qualification with emission held at level 2 after loopback.
It checks that suspend and disconnect park GPIO29 before aborting an in-flight internal DMA command and retain
the DMA buffer until both fresh reset acknowledgments arrive. [Host test](../../package/PianoIR/tests/ir_frame_safety_model_host.cpp)

With `piano-ir=2` on `v131y12u-ram1`, stream `0x423` used SMMU bypass bank 81. The read-only survey made
**45 MMIO loads, 0 stores and 0 map failures**. GPIO29 stayed dark as a GPIO input. The survey skipped the
SE15 read while QUP2/s7 was unclocked. After the GCC vote, SE15 brought up GENI SE DMA at **1,920,000 Hz**.
The internal loopback moved and returned **64/64 bytes in 499 µs (`rc=0`)**, with GPIO29 held as GPIO.
`piano-ir status` and IORegistry agreed on `ready: GENI SE15 at 1920000 Hz` and zero sends.
[November 27 register and loopback results](../results-2026-11-27.md#se15-infrared-bring-up)

Optical IR emission verification remains **HOLD** pending an attended receiver check that also verifies GPIO29
returns low after a send.
The unprivileged-send refusal and an administrator send after sleep/wake also remain open.

## 6. Teardown and failures

GENI is the only transmit path. GPIO bit-banging can leave the emitter high if its thread is preempted, so the driver
keeps GPIO29 parked when GENI is unavailable. A transfer failure disconnects MOSI and checks that the engine has stopped.
Sleep, shutdown and stop release DMA memory once the driver has confirmed the shifter and DMA are idle. The next
transmission after wake allocates a fresh buffer. If DMA cannot be confirmed idle, the driver disables sends and keeps
that allocation pinned to protect memory from a late DMA access.
