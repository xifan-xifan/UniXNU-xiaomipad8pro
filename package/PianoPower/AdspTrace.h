/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
/* Read-only diagnostic decoding. Never used to accept, route, retry or reject a transport packet. */
#ifndef PIANO_ADSP_TRACE_H
#define PIANO_ADSP_TRACE_H
#include <stdint.h>

struct AdspTraceHeader {
    uint32_t version, type, src_node, src_port, dst_node, dst_port, size, confirm, owner, op;
    int32_t txn, qmi_type, msg, sns;
};
static inline uint32_t atr16(const uint8_t *p) {return p[0] | (uint32_t)p[1] << 8;}
static inline uint32_t atr32(const uint8_t *p) {return atr16(p) | atr16(p + 2) << 16;}
static inline int atrVar(const uint8_t *p, uint32_t n, uint32_t *at, uint32_t *v) {
    *v = 0;
    for (unsigned shift = 0; shift < 35 && *at < n; shift += 7) {
        const uint32_t b = p[(*at)++];
        if (shift == 28 && (b & 0xf0)) return 0;
        *v |= (b & 127) << shift;
        if (!(b & 128)) return 1;
    }
    return 0;
}
/* SSC request field 2 is fixed32; skip fields by wire type, with all reads bounded by the TLV/protobuf lengths. */
static inline int32_t atrSns(const uint8_t *p, uint32_t n) {
    uint32_t at = 0, tag, size;
    while (at < n && atrVar(p, n, &at, &tag)) {
        if (tag == 21 && n - at >= 4) return (int32_t)atr32(p + at);
        switch (tag & 7) {
        case 0: if (!atrVar(p, n, &at, &size)) return -1; continue;
        case 1: size = 8; break;
        case 2: if (!atrVar(p, n, &at, &size)) return -1; break;
        case 5: size = 4; break;
        default: return -1;
        }
        if (size > n - at) return -1;
        at += size;
    }
    return -1;
}
static inline AdspTraceHeader atrHeader(const uint8_t *p, uint32_t n, bool qrtr, bool rx) {
    AdspTraceHeader h = {};
    h.txn = h.qmi_type = h.msg = h.sns = -1;
    h.size = n;
    if (!qrtr) {
        if (n >= 12) {h.owner = atr32(p); h.type = atr32(p + 4); h.op = atr32(p + 8);}
        /* Only MCA read/write has a sequence: PAN/UCSI headers have no transaction field. */
        if (h.owner == 0x800a && h.type == 1 && (h.op == 1 || h.op == 2) && n >= (rx ? 24u : 20u))
            h.txn = (int32_t)atr32(p + (rx ? 20 : 16));
        return h;
    }
    if (n < 32) return h;
    h.version = atr32(p); h.type = atr32(p + 4); h.src_node = atr32(p + 8); h.src_port = atr32(p + 12);
    h.confirm = atr32(p + 16); h.size = atr32(p + 20); h.dst_node = atr32(p + 24); h.dst_port = atr32(p + 28);
    if (h.version != 1 || h.type != 1 || h.size > n - 32 || h.size < 7) return h;
    p += 32; n = h.size;
    h.qmi_type = p[0]; h.txn = (int32_t)atr16(p + 1); h.msg = (int32_t)atr16(p + 3);
    if (h.qmi_type != 0 || h.msg != 0x20 || atr16(p + 5) > n - 7) return h;
    const uint32_t end = 7 + atr16(p + 5);
    for (uint32_t at = 7; end - at >= 3;) {
        const uint32_t tag = p[at], size = atr16(p + at + 1);
        at += 3;
        if (size > end - at) break;
        if (tag == 1 && size >= 2 && atr16(p + at) <= size - 2) {
            h.sns = atrSns(p + at + 2, atr16(p + at));
            break;
        }
        at += size;
    }
    return h;
}
/* Offset advance in a circular FIFO. A missed full lap undercounts consumption; it cannot invent a read advance. */
static inline uint32_t atrDistance(uint32_t from, uint32_t to, uint32_t length) {
    return to >= from ? to - from : length - from + to;
}
#endif
