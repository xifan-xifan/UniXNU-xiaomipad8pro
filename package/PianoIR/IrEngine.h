// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PIANO_IR_ENGINE_H
#define PIANO_IR_ENGINE_H

#include "GeniSpi.h"
#include "IrHw.h"
#include "IrWave.h"

namespace pianoir {

inline int setSampleClock(geni_spi &spi, const ir_clock_pick &pick, uint32_t source)
{
  const uint32_t speed = (pick.source_hz + pick.divider - 1) / pick.divider;
  return geni_spi_set_clock(&spi, speed, source, pick.level);
}

inline bool dmaSpanValid(uintptr_t va, uint64_t pa, uint64_t contiguous)
{
  constexpr unsigned alignment = 4096;
  constexpr unsigned bytes = 2 * IR_MAX_WAVE_BYTES;
  return va && pa && !((va | pa) & (alignment - 1)) &&
         contiguous >= bytes && va <= UINTPTR_MAX - bytes && pa <= UINT64_MAX - (bytes - 1);
}

inline bool setDmaBuffers(geni_spi::geni_dma &dma, uint8_t *va, uint64_t pa, uint64_t contiguous)
{
  if (!dmaSpanValid(uintptr_t(va), pa, contiguous)) return false;
  constexpr unsigned half = IR_MAX_WAVE_BYTES;
  dma.tx_virt = va;
  dma.rx_virt = va + half;
  dma.tx_phys = pa;
  dma.rx_phys = pa + half;
  dma.size = half;
  return true;
}

template <class Read, class Wait>
bool waitForIdle(Read read, Wait wait)
{
  for (unsigned i = 0; i < 5000; ++i) {
    if (!(read(SE_GENI_STATUS) & GENI_STATUS_M_ACTIVE)) return true;
    wait();
  }
  return false;
}

// A failed transfer can still own the DMA addresses after the command goes idle.
// Require fresh reset acknowledgments from both FSMs before recycling the buffer.
template <class Read, class Write, class Wait>
bool resetTransfer(bool dma, Read read, Write write, Wait wait)
{
  if (read(SE_GENI_STATUS) & GENI_STATUS_M_ACTIVE) {
    write(SE_GENI_M_CMD_CTRL_REG, GENI_M_CMD_ABORT);
    if (!waitForIdle(read, wait)) return false;
  }
  if (!dma) return true;
  write(SE_DMA_TX_IRQ_CLR, GENI_DMA_RESET_DONE);
  write(SE_DMA_RX_IRQ_CLR, GENI_DMA_RESET_DONE);
  write(SE_DMA_TX_FSM_RST, 1);
  write(SE_DMA_RX_FSM_RST, 1);
  for (unsigned i = 0; i < 5000; ++i) {
    const uint32_t tx = read(SE_DMA_TX_IRQ_STAT), rx = read(SE_DMA_RX_IRQ_STAT);
    if ((tx & rx & GENI_DMA_RESET_DONE) && !(read(SE_GENI_STATUS) & GENI_STATUS_M_ACTIVE)) return true;
    wait();
  }
  return false;
}

struct TransferEnd {
  int status;
  bool parked, reusable;
};

template <class Park>
bool beginLoopback(geni_spi &spi, Park park)
{
  if (!park()) return false;
  return geni_spi_setup(&spi, GENI_SPI_LOOP, 0) == 0;
}

// Re-park before recovery, including when GENI reports completion for only part of a frame.
template <class Read, class Write, class Wait, class Park>
TransferEnd finishTransfer(const geni_spi &spi, int rc, size_t bytes, bool loopback, bool muxed,
                           Read read, Write write, Wait wait, Park park)
{
  if (!rc && spi.use_dma && (spi.last_tx_in != bytes || (loopback && spi.last_rx_in != bytes)))
    rc = GENI_ERR_IO;
  const bool idle = !rc && waitForIdle(read, wait);
  const bool parked = park();
  const bool reusable = idle || resetTransfer(spi.use_dma, read, write, wait);
  if (!rc && (!idle || !parked || (!loopback && !muxed))) rc = GENI_ERR_IO;
  return {rc, parked, reusable};
}

}  // namespace pianoir
#endif
