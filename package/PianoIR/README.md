# PianoIR

Piano's IR transmitter for native macOS 27: the MOSI line of QUP2 serial engine 7 ("SE15", `spi@89c000`), GPIO29, active high.
The waveform is the SPI bit stream itself at ~1.92 MHz with the carrier built from bits (as Linux's piano ir-spi variant does).
Owner item HW-IR; design, levels and the tablet checklist: `docs/drivers/ir.md`.

- `piano-ir=1`: a read-only survey (pins, the QUP2 votes and branches, the s7 clock and its DFS table, SE15's registers when
  clocked, the SMMU entry of stream 0x423). No store.
- `piano-ir=2`: the survey, then the transmitter (votes, the 0x423 bypass, GENI in SE DMA mode, the clock pick, an emission-free
  loopback, GPIO29 parked low); sends from `tooling/native/piano-ir` (`sudo piano-ir raw|nec|blink|status`).

Files: `IrWave.h` (portable C: checks, the waveform, NEC, the parser, the clock pick; shared with the tool), `IrHw.h` (pins and
clock decoders), `IrEngine.h` (GENI transfer teardown), `PianoIRClient.h` (the IORegistry interface), `PianoIR.cpp` (the kext).
Run the host suite with `bash package/PianoIR/tests/run-ir-host.sh` or add `--sanitize` for ASan and UBSan. The GENI engine is
`package/PianoTouch/GeniSpi.c`, compiled in. Build: `tooling/k27/build-native-piano-ir.sh`.
