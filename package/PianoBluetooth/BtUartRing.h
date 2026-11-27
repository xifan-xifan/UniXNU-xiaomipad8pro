/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_BT_UART_RING_H
#define PIANO_BT_UART_RING_H

#include <stdint.h>
#if KERNEL
#include <sys/systm.h>
#else
#include <string.h>
#endif

#define BT_TTY_FIFO 8192u

/* The tty serializes access and bounds n to the available space or data. */
static inline void bt_tty_fifo_put(uint8_t *fifo, uint32_t head, uint32_t *len, const uint8_t *data, uint32_t n)
{
    if (!n) return;
    const uint32_t at = (head + *len) % BT_TTY_FIFO;
    const uint32_t room = BT_TTY_FIFO - at;
    const uint32_t first = n < room ? n : room;
    memcpy(fifo + at, data, first);
    if (n > first) memcpy(fifo, data + first, n - first);
    *len += n;
}

static inline void bt_tty_fifo_get(const uint8_t *fifo, uint32_t *head, uint32_t *len, uint8_t *data, uint32_t n)
{
    if (!n) return;
    const uint32_t room = BT_TTY_FIFO - *head;
    const uint32_t first = n < room ? n : room;
    memcpy(data, fifo + *head, first);
    if (n > first) memcpy(data + first, fifo, n - first);
    *head = (*head + n) % BT_TTY_FIFO;
    *len -= n;
}

#endif
