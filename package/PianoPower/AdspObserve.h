/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
/* Header-only observation of a copied owner state and a bounded copy of its pending TX FIFO.
 * DATA payloads are never dumped: only GLINK and identified FastRPC/GPR/QRTR/QMI/PMIC headers. */
#pragma once
#include <PianoObserve.h>
#include "QcomGlink.h"
#include "AdspTrace.h"
namespace adspobserve {
using pianoobserve::Text;
inline uint64_t le64(const uint8_t *p) {return atr32(p) | uint64_t(atr32(p + 4)) << 32;}
inline const QcomGlinkChannel *channel(const QcomGlink &g, unsigned cid, bool remote) {
    for (const auto &c : g.channel) if (c.used && (remote ? c.rcid : c.lcid) == cid) return &c;
    return nullptr;
}
inline bool named(const QcomGlinkChannel *c, const char *name) {
    if (!c) return false;
    for (unsigned i = 0; i < QCOM_GLINK_NAME; ++i) {if (c->name[i] != name[i]) return false; if (!name[i]) return true;}
    return false;
}
inline void frames(Text &t, const QcomGlink &g, const uint8_t *bytes, unsigned n, unsigned tail, unsigned length) {
    unsigned at = 0, count = 0;
    while (at < n && count < 16) {
        if (n - at < 8) {t.add("frame status=partial-header offset=%u available=%u\n", (tail + at) % length, n - at); break;}
        const uint8_t *p = bytes + at;
        const unsigned cmd = atr16(p), cid = atr16(p + 2), value = atr32(p + 4);
        uint64_t size = 8;
        bool known = true;
        switch (cmd) {
        case QCOM_GLINK_OPEN: size += value & 0xffff; break;
        case QCOM_GLINK_INTENT: size += uint64_t(value) * 8; break;
        case QCOM_GLINK_TX_DATA: case QCOM_GLINK_TX_DATA_CONT:
            if (n - at < 16) {t.add("frame status=partial-data-header offset=%u\n", (tail + at) % length); return;}
            size = 16ull + atr32(p + 8); break;
        case 0: case 1: case 3: case 4: case 6: case 7: case 8: case 11: case 13: case 14: case 15: break;
        default: known = false;
        }
        const uint64_t padded = (size + 7) & ~7ull;
        const bool complete = known && padded <= n - at && padded <= length;
        const bool remote = cmd == QCOM_GLINK_OPEN_ACK || cmd == QCOM_GLINK_CLOSE_ACK;
        const QcomGlinkChannel *c = channel(g, cid, remote);
        t.add("frame index=%u offset=%u cmd=%u cid=%u value=%u channel=%.*s cid_valid=%u size=%llu padded_end=%u complete=%u status=%s raw_header=",
              count, (tail + at) % length, cmd, cid, value, c ? QCOM_GLINK_NAME : 7, c ? c->name : "unknown", c != nullptr,
              (unsigned long long)size, unsigned((tail + at + padded) % length), complete, !known ? "unknown-opcode" : complete ? "ok" : "incomplete");
        t.hex(p, cmd == 9 || cmd == 12 ? 16 : 8); t.add("\n");
        if (!complete) break;
        if (cmd == 9 || cmd == 12) {
            const unsigned chunk = atr32(p + 8), left = atr32(p + 12);
            bool iid = false;
            if (c) for (const auto &r : c->remote) iid |= r.valid && r.id == value;
            t.add("data iid=%u iid_valid=%u chunk=%u left=%u payload=omitted\n", value, iid, chunk, left);
            const uint8_t *d = p + 16;
            unsigned keep = 0;
            if (cmd == 9 && named(c, "fastrpcglink-apps-dsp") && chunk >= 40) {
                keep = 40;
                t.add("fastrpc pid=%u tid=%u ctx=%llu handle=%u sc=%#x addr=%#llx size=%llu\n", atr32(d), atr32(d+4),
                      (unsigned long long)le64(d+8), atr32(d+16), atr32(d+20), (unsigned long long)le64(d+24), (unsigned long long)le64(d+32));
            } else if (cmd == 9 && named(c, "adsp_apps") && chunk >= 24) {
                keep = 24;
                const unsigned w0 = atr32(d), domains = atr32(d+4), header = ((w0 >> 4) & 15) * 4, packetSize = w0 >> 8;
                t.add("gpr version=%u header_bytes=%u size=%u src_domain=%u dst_domain=%u src_port=%#x dst_port=%#x token=%#x opcode=%#x valid=%u\n",
                      w0 & 15, header, packetSize, (domains >> 8) & 255, domains & 255, atr32(d+8), atr32(d+12), atr32(d+16), atr32(d+20),
                      !(w0 & 15) && header >= 24 && packetSize >= header && packetSize <= chunk);
            } else if (cmd == 9 && named(c, "IPCRTR") && chunk >= 32) {
                keep = 32;
                const AdspTraceHeader h = atrHeader(d, chunk, true, false);
                if (h.version == 1 && h.type == 1 && h.size >= 7 && h.size <= chunk - 32) keep = 39;
                t.add("qrtr version=%u type=%u from=%u:%u to=%u:%u size=%u qmi_type=%d txn=%d msg=%d sns=%d\n",
                      h.version, h.type, h.src_node, h.src_port, h.dst_node, h.dst_port, h.size, h.qmi_type, h.txn, h.msg, h.sns);
            } else if (cmd == 9 && named(c, "PMIC_RTR_ADSP_APPS") && chunk >= 12) {
                keep = 12;
                t.add("pmic owner=%#x type=%u op=%#x\n", atr32(d), atr32(d+4), atr32(d+8));
            }
            t.add("raw_protocol_header="); t.hex(d, keep); t.add("\n");
        } else if (cmd == QCOM_GLINK_INTENT) {
            for (unsigned i = 0; i < value && i < 48; ++i)
                t.add("offer index=%u size=%u iid=%u\n", i, atr32(p+8+8*i), atr32(p+12+8*i));
            if (value > 48) t.add("offers_capped=1\n");
        }
        at += unsigned(padded); ++count;
    }
    t.add("frames=%u walked=%u pending=%u frame_cap=%u\n", count, at, n, at < n && count == 16);
}
inline void owner(Text &t, const QcomGlink &g) {
    t.add("owner broken=%u version=%u features=%#x retry_pending=%u reply_due=%u reply_cmd=%u reply_cid=%u reply_value=%u done_deferred=%u done_retired=%u reply_deferred=%u reply_retired=%u tx_blocked=%u rx_messages=%u tx_messages=%u\n",
          g.broken, g.version_agreed, g.features, g.retry_pending, g.reply_due, g.reply_cmd, g.reply_cid, g.reply_value,
          g.done_deferred, g.done_retried, g.reply_deferred, g.reply_retried, g.tx_blocked, g.rx_messages, g.tx_messages);
    t.add("sens-trace3 intent_totals grant_mode=%u dynamic_bytes=%u max_intent=%u max_total=%u reply_offer=%u requests=%u granted=%u refused=%u last_size=%u min_size=%u max_size=%u size_sum=%llu requests_tx=%u acks=%u ack_granted=%u ack_refused=%u\n",
          g.dynamic_intents, g.dynamic_bytes, QCOM_GLINK_DYNAMIC_INTENT_MAX, QCOM_GLINK_DYNAMIC_TOTAL_MAX, g.reply_offer,
          g.intents.requests, g.intents.granted, g.intents.refused, g.intents.last_size, g.intents.min_size, g.intents.max_size,
          (unsigned long long)g.intents.size_sum, g.intents.requests_tx, g.intents.acks, g.intents.ack_granted, g.intents.ack_refused);
    for (unsigned j = 0; j < QCOM_GLINK_MAX_CHANNELS; ++j) {
        const auto &c = g.channel[j];
        t.add("channel slot=%u name=%.*s used=%u lcid=%u rcid=%u up=%u open_sent=%u open_acked=%u remote_open=%u ack_due=%u intent_asked=%u size=%u count=%u\n",
              j, QCOM_GLINK_NAME, c.name, c.used, c.lcid, c.rcid, c.up, c.open_sent, c.open_acked, c.remote_open, c.ack_due, c.intent_asked, c.intent_size, c.intent_count);
        const auto &s = c.intents;
        t.add("intent_stats channel=%u requests=%u granted=%u refused=%u last_size=%u min_size=%u max_size=%u size_sum=%llu requests_tx=%u last_tx_size=%u acks=%u ack_granted=%u ack_refused=%u fixed_count=%u\n",
              j, s.requests, s.granted, s.refused, s.last_size, s.min_size, s.max_size, (unsigned long long)s.size_sum,
              s.requests_tx, s.last_tx_size, s.acks, s.ack_granted, s.ack_refused, c.fixed_intent_count);
        for (unsigned i = 0; i < QCOM_GLINK_MAX_LOCAL_INTENTS; ++i) {
            const auto &l = c.local[i];
            t.add("local channel=%u index=%u configured=%u size=%u dynamic=%u fill=%u advertised=%u busy=%u done_due=%u\n",
                  j, i, i < c.intent_count, l.size, i >= c.fixed_intent_count && i < c.intent_count, l.fill, l.advertised, l.busy, l.done_due);
        }
        for (unsigned i = 0; i < QCOM_GLINK_MAX_REMOTE_INTENTS; ++i) {
            const auto &r = c.remote[i];
            t.add("remote channel=%u index=%u id=%u size=%u valid=%u busy=%u\n", j, i, r.id, r.size, r.valid, r.busy);
        }
    }
}
}

namespace adspobserve {
struct Item {int status = -7; uint32_t offset = 0, size = 0;};
/* Only TOC and the selected APPS/ADSP or global partition. All arithmetic is checked before a load;
 * cap metadata traversal even if the remote publishes corrupt/cyclic allocation metadata. */
template <class Read> Item findItem(Read rd, uint32_t total, uint16_t host, uint16_t remote, uint16_t item) {
    Item out;
    if (total < 0x2000 || (total & 0xfff)) {out.status = -1; return out;}
    const uint32_t toc = total - 0x1000;
    if (rd(toc) != 0x434f5424 || rd(toc+4) != 1) {out.status = -1; return out;}
    const uint32_t count = rd(toc+8);
    if (count > (0x1000-32)/48) {out.status = -4; return out;}
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t e = toc + 32 + 48*i, p = rd(e), size = rd(e+4), hosts = rd(e+12);
        if (!((uint16_t(hosts)==host && uint16_t(hosts>>16)==remote) || (uint16_t(hosts)==remote && uint16_t(hosts>>16)==host))) continue;
        uint32_t line = rd(e+16); if (!line) line = 8;
        if (!p || size < 64 || uint64_t(p)+size > toc || (p&7) || line < 8 || line > size || (line&(line-1)) ||
            rd(p) != 0x54525024 || rd(p+4) != hosts || rd(p+8) != size) {out.status=-4; return out;}
        const uint32_t end = rd(p+12), cached = rd(p+16);
        if (end < 32 || end > cached || cached > size) {out.status=-4; return out;}
        uint32_t at = 32; unsigned steps = 0;
        while (at < end && steps++ < 4096) {
            if (end-at < 16) {out.status=-4; return out;}
            const uint32_t tag=rd(p+at), bytes=rd(p+at+4), pads=rd(p+at+8), header=pads>>16;
            if ((tag&0xffff)!=0xa5a5 || (pads&0xffff)>bytes || uint64_t(at)+16+header+bytes>end || ((at+16+header)&3)) {out.status=-4; return out;}
            if ((tag>>16)==item) {out.status=0; out.offset=p+at+16+header; out.size=bytes-(pads&0xffff); return out;}
            at += 16+header+bytes;
        }
        if (at != end) {out.status=-4; return out;}
        const uint32_t header=(16+line-1)&~(line-1);
        if (header>size) {out.status=-4; return out;}
        at=size-header;
        while (at>cached && steps++<4096) {
            if (uint64_t(at)+16>size || (at&3)) {out.status=-4; return out;}
            const uint32_t tag=rd(p+at), bytes=rd(p+at+4), pads=rd(p+at+8);
            if ((tag&0xffff)!=0xa5a5 || bytes>at || (pads&0xffff)>bytes || at-bytes<cached || ((at-bytes)&3)) {out.status=-4; return out;}
            if ((tag>>16)==item) {out.status=0; out.offset=p+at-bytes; out.size=bytes-(pads&0xffff); return out;}
            if (uint64_t(bytes)+header>at) {out.status=-4; return out;}
            at-=bytes+header;
        }
        if (steps>=4096) out.status=-4;
        return out;
    }
    out.status=-2; return out;
}
template <class Read> void smp(Text &t, Read rd, const Item &item, unsigned id) {
    t.add("smem item=%u status=%d bytes=%u\n", id, item.status, item.size);
    if (item.status || item.size<20) {t.add("smp status=unavailable-or-short\n"); return;}
    uint32_t h[5]; for(unsigned i=0;i<5;++i)h[i]=rd(item.offset+4*i);
    const unsigned valid=h[3]>>16;
    const bool layout=h[0]==0x504d5324 && (h[1]&255)==1 && (h[3]&0xffff)<=16 && valid<=(h[3]&0xffff) && 20+20*valid<=item.size;
    t.add("smp magic=%#x version=%u features=%#x local_pid=%u remote_pid=%u total=%u valid=%u flags=%#x layout_ok=%u\n",
          h[0], h[1]&255,h[1]>>8,h[2]&0xffff,h[2]>>16,h[3]&0xffff,valid,h[4],layout);
    bool slave=false;
    if (layout) for(unsigned i=0;i<valid;++i) {
        uint32_t words[4]; for(unsigned j=0;j<4;++j)words[j]=rd(item.offset+20+20*i+4*j);
        const char *names[2]={"slave-kernel","master-kernel"};
        for(unsigned k=0;k<2;++k) {
            bool same=true;
            for(unsigned j=0;j<16;++j) {const char c=char(words[j/4]>>(8*(j%4))); if(c!=names[k][j]){same=false;break;} if(!c)break;}
            if(same) {const uint32_t value=rd(item.offset+20+20*i+16); slave|=!k;
                t.add("entry name=%s index=%u value=%#x fatal=%u ready=%u handover=%u\n",names[k],i,value,value&1,(value>>1)&1,(value>>2)&1);}
        }
    }
    bool stable=true;for(unsigned i=0;i<5;++i)stable &= h[i]==rd(item.offset+4*i);
    t.add("smp header_stable=%u slave_found=%u\n",stable,slave);
}
}
