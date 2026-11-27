/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026 Leo Zhang. */
/* Included in PianoAdsp's owner namespace, after all state declarations. */
struct ObserveCopy {QcomGlink glink; uint8_t bytes[kGlinkFifo];};
bool observeSpan(const volatile void *p, uint32_t bytes) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(gSmemW.va), at = reinterpret_cast<uintptr_t>(p);
    return base && at >= base && bytes <= kSmemSize && at - base <= kSmemSize - bytes;
}
bool observeFifo(const QcomGlinkFifo &f) {
    return f.length && f.length <= kGlinkFifo && !(f.length & 7) &&
           !(reinterpret_cast<uintptr_t>(f.head) & 3) && !(reinterpret_cast<uintptr_t>(f.tail) & 3) &&
           observeSpan(f.head, 4) && observeSpan(f.tail, 4) && observeSpan(f.data, f.length);
}
void observeGlink(pianoobserve::Text &t, bool fifo) {
    ObserveCopy *copy = static_cast<ObserveCopy *>(IOMallocZeroData(sizeof(ObserveCopy)));
    if (!copy) {t.add("status=no-memory\n"); return;}
    bool consistent = false;
    uint32_t epoch=0, th=0, tt=0, rh=0, rt=0, pending=0;
    for (unsigned attempt=0;attempt<3;++attempt) {
        epoch=__atomic_load_n(&gObserveEpoch,__ATOMIC_ACQUIRE);
        if (epoch&1) continue;
        /* Pointer fields are never dereferenced until checked against the permanent SMEM mapping. */
        const volatile uint8_t *src=reinterpret_cast<const volatile uint8_t *>(&gGlink);
        uint8_t *dst=reinterpret_cast<uint8_t *>(&copy->glink);
        for(unsigned i=0;i<sizeof(QcomGlink);++i)dst[i]=src[i];
        const auto &g=copy->glink;
        if (!observeFifo(g.tx) || !observeFifo(g.rx)) break;
        th=*g.tx.head;tt=*g.tx.tail;rh=*g.rx.head;rt=*g.rx.tail;
        if (th>=g.tx.length || tt>=g.tx.length || rh>=g.rx.length || rt>=g.rx.length || ((th|tt|rh|rt)&7)) break;
        __asm__ volatile("dmb oshld" ::: "memory"); // snapshot loads only; transport ordering is unchanged
        pending=atrDistance(tt,th,g.tx.length);
        if (fifo) for(unsigned i=0;i<pending;++i)copy->bytes[i]=g.tx.data[(tt+i)%g.tx.length];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        consistent=epoch==__atomic_load_n(&gObserveEpoch,__ATOMIC_ACQUIRE) &&
                   th==*g.tx.head && tt==*g.tx.tail && rh==*g.rx.head && rt==*g.rx.tail;
        if(consistent)break;
    }
    t.add("owner_consistent=%u epoch=%u glink_bytes=%u channel_bytes=%u fifo_bytes=%u channels=%u local_max=%u remote_max=%u\n",
          consistent,epoch,unsigned(sizeof(QcomGlink)),unsigned(sizeof(QcomGlinkChannel)),unsigned(sizeof(QcomGlinkFifo)),
          QCOM_GLINK_MAX_CHANNELS,QCOM_GLINK_MAX_LOCAL_INTENTS,QCOM_GLINK_MAX_REMOTE_INTENTS);
    if(consistent) {
        const auto &g=copy->glink;
        t.add("fifo status=ok tx_length=%u rx_length=%u th=%u tt=%u rh=%u rt=%u pending=%u\n",g.tx.length,g.rx.length,th,tt,rh,rt,pending);
        t.add("sens-trace3 requests=%u granted=%u refused=%u acks=%u ack_granted=%u ack_refused=%u grant_mode=%u dynamic_bytes=%u\n",
              g.intents.requests,g.intents.granted,g.intents.refused,g.intents.acks,g.intents.ack_granted,g.intents.ack_refused,
              g.dynamic_intents,g.dynamic_bytes);
        if(fifo)adspobserve::frames(t,g,copy->bytes,pending,tt,g.tx.length);
        else adspobserve::owner(t,g);
    } else t.add("status=unavailable-or-concurrent no_decode=1\n");
    IOFreeData(copy,sizeof(ObserveCopy));
}
void observeAdsp(pianoobserve::Text &t, int selector) {
    const uint64_t start=nowMs();
    t.add("sens-trace2 version=1 export=%d start_ms=%llu stage=%d mapped=%u\n",selector,(unsigned long long)start,gStage,gSmemW.va!=nullptr);
    if(selector==0 || selector==3)observeGlink(t,selector==0);
    if(selector==1 || selector==2) {
        if(!gSmemW.va) t.add("status=unmapped\n");
        else {
            auto read=[](uint32_t at){return rd(gSmemW,at);};
            if(selector==1) {
                const auto in=adspobserve::findItem(read,kSmemSize,0,kRemotePid,kItemIn);
                const auto out=adspobserve::findItem(read,kSmemSize,0,kRemotePid,kItemOut);
                adspobserve::smp(t,read,in,kItemIn);adspobserve::smp(t,read,out,kItemOut);
                t.add("cached slave=%d negotiated=%u ssr_ack_enabled=%u ssr_acked=%u fatal_reports=%u link_generation=%u cache_consistency=best-effort independent_wdog=unknown\n",
                      gSlaveBits,gP2p.negotiated,gP2p.ssr_ack_enabled,gP2p.ssr_acked,gRestarts,gLinkGeneration);
            } else {
                const auto crash=adspobserve::findItem(read,kSmemSize,QCOM_SMEM_HOST_GLOBAL,QCOM_SMEM_HOST_GLOBAL,kItemCrash);
                uint8_t text[80]={};unsigned n=0;bool terminated=false;
                if(!crash.status)for(;n<crash.size && n<sizeof text;++n) {
                    const uint32_t at=crash.offset+n;
                    const uint8_t byte=uint8_t(read(at&~3u)>>(8*(at&3)));
                    if(!byte){terminated=true;break;}text[n]=byte;
                }
                t.add("smem item=423 status=%s rc=%d item_bytes=%u copied=%u terminated=%u capped=%u raw_hex=",
                      crash.status==-7?"absent":crash.status?"read-error":!n?"empty":"present",crash.status,crash.size,n,terminated,!terminated && n==80);
                t.hex(text,n);t.add("\n");
            }
        }
    }
    if(selector==3) {
        t.add("ipcc config_status=%s",gIpcc.va?"ok":"unmapped");
        if(gIpcc.va)t.add(" config=%#x",rd(gIpcc,0x08));
        t.add(" poll_calls=%u recv_reads=%u recv_nonempty=%u last_recv=%#x clear_calls=%u last_clear=%#x send_calls=%u last_send=%#x recv_ms=%llu send_ms=%llu enable_requested=%#x apps_mask=unknown remote_mask=unknown remote_pending=unknown remote_ack=unknown\n",
              gObservePolls,gObserveReads,gObserveRecv,gObserveLastRecv,gObserveClears,gObserveLastClear,gObserveSends,gObserveLastSend,
              (unsigned long long)gObserveRecvMs,(unsigned long long)gObserveSendMs,gObserveEnabled);
    }
    t.add("end_ms=%llu truncated=%u\n",(unsigned long long)nowMs(),t.truncated);
}
int sysctlObserve(struct sysctl_oid *, void *, int selector, struct sysctl_req *req) {
    return pianoobserve::readOnly(req,[selector](pianoobserve::Text &t){observeAdsp(t,selector);});
}
void registerObserve() {
    static struct sysctl_oid oid[4];
    static const char *const names[]={"piano_glink_fifo","piano_smem_state","piano_smem_crash","piano_glink_owner"};
    for(unsigned i=0;i<4;++i)pianoobserve::registerReadOnly(oid[i],names[i],int(i),sysctlObserve);
    say("[sens-trace2] root-only passive exports v1 (no extra RECV_ID/SEND/ACK)");
}
