/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. Synthetic protocol headers, never device payloads. */
#include "../AdspTrace.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static void put(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8*i));}
int main() {
    uint8_t p[100] = {};
    put(p, 1); put(p+4, 1); put(p+8, 1); put(p+12, 25); put(p+20, 45); put(p+24, 5); put(p+28, 19);
    p[33] = 7; p[35] = 0x20; p[37] = 38; p[39] = 1; p[40] = 35; p[42] = 33;
    p[44] = 10; p[45] = 18; p[64] = 21; put(p+65, 514); p[69] = 26; p[70] = 4;
    p[71] = 8; p[72] = 1; p[73] = 16; p[74] = 0; p[75] = 34; p[76] = 0;
    const auto before = atrHeader(p, 80, true, false);
    assert(before.sns == 514 && before.txn == 7 && before.src_port == 25 && before.dst_port == 19);
    for (uint32_t n = 0; n < 77; ++n) assert(atrHeader(p, n, true, false).sns == -1);
    put(p+20, 0xffffffff); assert(atrHeader(p, 80, true, false).sns == -1);
    put(p+20, 45); p[40] = 255; assert(atrHeader(p, 80, true, false).sns == -1); p[40] = 35;
    p[45] = 255; assert(atrHeader(p, 80, true, false).sns == -1); p[45] = 18;
    p[32] = 2; assert(atrHeader(p, 80, true, true).sns == -1); p[32] = 0;
    put(p+28, 25); put(p+12, 19); p[32] = 4; p[35] = 0x21;
    auto reply = atrHeader(p, 80, true, true);
    assert(reply.dst_port == 25 && reply.src_port == 19 && reply.qmi_type == 4 && reply.msg == 0x21 && reply.txn == 7);
    put(p, 3); assert(atrHeader(p, 80, true, true).qmi_type == -1);
    memset(p, 0, sizeof p); put(p, 0x800a); put(p+4, 1); put(p+8, 2); put(p+16, 13); put(p+20, 17);
    assert(atrHeader(p, 24, false, false).txn == 13 && atrHeader(p, 24, false, true).txn == 17);
    put(p, 0x800b); assert(atrHeader(p, 24, false, true).txn == -1);
    // Pending bytes before the packet, a wrapped publish, a partial read and then the exact packet-end read.
    const uint64_t read = 900, end = read + atrDistance(400, 480, 512) + 96;
    assert(end == 1076 && atrDistance(480, 64, 512) == 96);
    uint64_t consumed = read + atrDistance(400, 32, 512);
    assert(consumed < end);
    consumed += atrDistance(32, 64, 512);
    assert(consumed == end && atrDistance(64, 64, 512) == 0);
    // Every prefix and every single-byte corruption must stay within the supplied length.
    for (uint32_t n = 0; n <= sizeof p; ++n) {
        (void)atrHeader(p, n, false, true);
        for (unsigned i = 0; i < n; ++i) {p[i] ^= 255; (void)atrHeader(p, n, true, true); p[i] ^= 255;}
    }
    puts("AdspTrace: bounded headers, MCA sequences, truncated frames and FIFO wrap passed");
}
