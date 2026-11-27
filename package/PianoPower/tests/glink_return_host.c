/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, Leo Zhang. All rights reserved. */
/* Production GLINK receive-intent ownership under shared TX FIFO pressure. Synthetic bytes only. */
#include "../QcomGlink.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned checks, failures, deliveries, kicks;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static void put32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void kick(void *p) {(void)p; ++kicks;}
static void receive(void *p,QcomGlinkChannel *c,const uint8_t *data,uint32_t n)
{ (void)p; (void)c; CHECK(n==4 && !memcmp(data,"lux!",4)); ++deliveries; }
typedef struct {
    volatile uint32_t th,tt,rh,rt;
    uint8_t tx[512],rx[512],storage[16*4];
    QcomGlink g;
} Fixture;
static QcomGlinkChannel *setup(Fixture *f,int reuse,unsigned intents)
{
    memset(f,0,sizeof *f);
    const QcomGlinkFifo tx={&f->th,&f->tt,f->tx,sizeof f->tx},rx={&f->rh,&f->rt,f->rx,sizeof f->rx};
    qcom_glink_init(&f->g,tx,rx,kick,NULL);
    f->g.version_sent=f->g.version_agreed=1;
    f->g.features=reuse?QCOM_GLINK_FEATURE_INTENT_REUSE:0;
    const QcomGlinkClient client={NULL,receive};
    QcomGlinkChannel *c=qcom_glink_channel(&f->g,"IPCRTR",16,intents,f->storage,&client,NULL,1);
    CHECK(c!=NULL);
    c->open_sent=c->open_acked=c->remote_open=c->up=1;c->rcid=7;
    for(unsigned i=0;i<intents;++i)c->local[i].advertised=1;
    return c;
}
static void remote(Fixture *f,uint16_t command,uint16_t p1,uint32_t p2,const uint8_t *data,uint32_t n)
{
    uint8_t header[8]={(uint8_t)command,(uint8_t)(command>>8),(uint8_t)p1,(uint8_t)(p1>>8)};
    put32(header+4,p2);
    uint32_t h=f->rh;
    for(unsigned i=0;i<8;++i)f->rx[h++%sizeof f->rx]=header[i];
    for(unsigned i=0;i<n;++i)f->rx[h++%sizeof f->rx]=data[i];
    f->rh=((h+7)&~7u)%sizeof f->rx;
}
static void data(Fixture *f,uint16_t command,uint32_t id,const char *bytes,uint32_t n,uint32_t left)
{
    uint8_t payload[24];put32(payload,n);put32(payload+4,left);memcpy(payload+8,bytes,n);
    remote(f,command,7,id,payload,8+n);
}
static unsigned take(Fixture *f,uint16_t wanted,uint32_t wanted_p2)
{
    unsigned found=0;
    while(f->tt!=f->th){
        const uint32_t t=f->tt;
        uint8_t h[8];for(unsigned i=0;i<8;++i)h[i]=f->tx[(t+i)%sizeof f->tx];
        const uint16_t cmd=(uint16_t)(h[0]|h[1]<<8);
        const uint32_t p2=get32(h+4);
        if(cmd==wanted && p2==wanted_p2)++found;
        uint32_t n=8;
        if(cmd==QCOM_GLINK_INTENT)n+=8*p2;
        if(cmd==QCOM_GLINK_OPEN)n=(8+p2+7)&~7u;
        f->tt=(t+n)%sizeof f->tx;
    }
    return found;
}
static void completion(int reuse,int fragmented,unsigned intents)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,reuse,intents);
    const unsigned before=deliveries;
    /* No ordinary command fits; the physically reserved READ_NOTIF bytes still do. */
    f.th=sizeof f.tx-24;
    if(fragmented){data(&f,QCOM_GLINK_TX_DATA,1,"lu",2,2);qcom_glink_poll(&f.g);CHECK(deliveries==before);}
    data(&f,fragmented?QCOM_GLINK_TX_DATA_CONT:QCOM_GLINK_TX_DATA,1,fragmented?"x!":"lux!",fragmented?2:4,0);
    CHECK(qcom_glink_poll(&f.g)>=1 && deliveries==before+1 && f.rt==f.rh);
    /* A remote cannot re-use this intent until it sees DONE. No payload may be delivered twice on retry. */
    f.tt=f.th;
    qcom_glink_poll(&f.g);
    CHECK(take(&f,reuse?QCOM_GLINK_RX_DONE_W_REUSE:QCOM_GLINK_RX_DONE,1)==1);
    CHECK(deliveries==before+1 && c->local[0].busy==0);
    CHECK(f.g.done_deferred==1 && f.g.done_retried==1 && !c->local[0].done_due);
    CHECK(f.g.tx_blocked>0);
    qcom_glink_poll(&f.g);
    CHECK(take(&f,reuse?QCOM_GLINK_RX_DONE_W_REUSE:QCOM_GLINK_RX_DONE,1)==0 && deliveries==before+1);
    CHECK(!f.g.retry_pending);
}
static void intent_grant(void)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,0,1);
    /* The non-reuse DATA completion frees an intent during this poll. Its DONE and re-advertisement fit,
     * but the following grant ACK does not: replaying the request would turn grant=1 into refusal=0. */
    f.th=sizeof f.tx-48;
    data(&f,QCOM_GLINK_TX_DATA,1,"lux!",4,0);
    remote(&f,QCOM_GLINK_RX_INTENT_REQ,7,16,NULL,0);
    qcom_glink_poll(&f.g);
    CHECK(c->local[0].advertised && f.g.reply_due && f.g.reply_deferred==1 && f.rt==f.rh);
    f.tt=f.th;
    qcom_glink_poll(&f.g);
    CHECK(take(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,1)==1 && f.g.reply_retried==1 && !f.g.reply_due);
    qcom_glink_poll(&f.g);CHECK(take(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,1)==0 && !f.g.retry_pending);
}
static void multiple_returns(void)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,1,3);
    const unsigned before=deliveries;
    f.th=sizeof f.tx-24;
    for(unsigned i=1;i<=3;++i)data(&f,QCOM_GLINK_TX_DATA,i,"lux!",4,0);
    qcom_glink_poll(&f.g);
    CHECK(deliveries==before+3 && f.g.done_deferred==3 && f.g.retry_pending);
    for(unsigned i=0;i<3;++i)CHECK(c->local[i].done_due && c->local[i].busy);
    f.tt=f.th;qcom_glink_poll(&f.g);
    /* Take individually without consuming the other ids. */
    unsigned done=0;
    while(f.tt!=f.th){uint8_t h[8];for(unsigned i=0;i<8;++i)h[i]=f.tx[(f.tt+i)%sizeof f.tx];
        CHECK(h[0]==QCOM_GLINK_RX_DONE_W_REUSE && get32(h+4)==done+1);++done;f.tt=(f.tt+8)%sizeof f.tx;}
    CHECK(done==3 && f.g.done_retried==3 && deliveries==before+3);
    qcom_glink_poll(&f.g);CHECK(!f.g.retry_pending && deliveries==before+3);
}
static void close_reopen(void)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,1,1);
    const unsigned before=deliveries;
    f.th=sizeof f.tx-24;
    data(&f,QCOM_GLINK_TX_DATA,1,"lux!",4,0);qcom_glink_poll(&f.g);
    CHECK(c->local[0].done_due && deliveries==before+1);
    remote(&f,QCOM_GLINK_CLOSE,7,0,NULL,0);
    qcom_glink_poll(&f.g);
    CHECK(c->local[0].done_due && c->remote_open && f.rt!=f.rh);
    f.tt=f.th;qcom_glink_poll(&f.g);
    CHECK(take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1)==1);
    CHECK(!c->remote_open && !c->up && !c->local[0].done_due && !c->local[0].busy);
    const char name[]="IPCRTR";
    remote(&f,QCOM_GLINK_OPEN,8,sizeof name,(const uint8_t *)name,sizeof name);qcom_glink_poll(&f.g);
    CHECK(c->remote_open && c->rcid==8 && !c->up);
    take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1);
    remote(&f,QCOM_GLINK_OPEN_ACK,c->lcid,0,NULL,0);qcom_glink_poll(&f.g);
    CHECK(c->up && c->local[0].advertised);
    CHECK(take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1)==0);
    uint8_t payload[12];put32(payload,4);put32(payload+4,0);memcpy(payload+8,"lux!",4);
    remote(&f,QCOM_GLINK_TX_DATA,8,1,payload,sizeof payload);qcom_glink_poll(&f.g);
    CHECK(deliveries==before+2 && take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1)==1);
    qcom_glink_poll(&f.g);CHECK(deliveries==before+2 && take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1)==0);
}
static void shared_channel_returns(void)
{
    Fixture f;QcomGlinkChannel *channels[3];channels[0]=setup(&f,1,1);
    const QcomGlinkClient client={NULL,receive};
    const char *names[2]={"fastrpcglink-apps-dsp","adsp_apps"};
    for(unsigned i=1;i<3;++i){
        channels[i]=qcom_glink_channel(&f.g,names[i-1],16,1,f.storage+16*i,&client,NULL,1);
        CHECK(channels[i]!=NULL);
        channels[i]->open_sent=channels[i]->open_acked=channels[i]->remote_open=channels[i]->up=1;
        channels[i]->rcid=(uint16_t)(7+i);channels[i]->local[0].advertised=1;
    }
    const unsigned before=deliveries;
    f.th=sizeof f.tx-24;
    uint8_t payload[12];put32(payload,4);put32(payload+4,0);memcpy(payload+8,"lux!",4);
    for(unsigned i=0;i<3;++i)remote(&f,QCOM_GLINK_TX_DATA,channels[i]->rcid,1,payload,sizeof payload);
    qcom_glink_poll(&f.g);
    CHECK(deliveries==before+3 && f.g.done_deferred==3 && f.g.retry_pending);
    for(unsigned i=0;i<3;++i)CHECK(channels[i]->local[0].done_due && channels[i]->local[0].busy);
    f.tt=f.th;qcom_glink_poll(&f.g);
    unsigned mask=0,done=0;
    while(f.tt!=f.th){
        uint8_t h[8];for(unsigned i=0;i<8;++i)h[i]=f.tx[(f.tt+i)%sizeof f.tx];
        const uint16_t cid=(uint16_t)(h[2]|h[3]<<8);
        unsigned slot=3;for(unsigned i=0;i<3;++i)if(channels[i]->lcid==cid)slot=i;
        CHECK(h[0]==QCOM_GLINK_RX_DONE_W_REUSE && get32(h+4)==1 && slot<3);
        if(slot<3){CHECK(!(mask&(1u<<slot)));mask|=1u<<slot;}
        ++done;f.tt=(f.tt+8)%sizeof f.tx;
    }
    CHECK(done==3 && mask==7 && f.g.done_retried==3 && deliveries==before+3);
    for(unsigned i=0;i<3;++i)CHECK(!channels[i]->local[0].done_due && !channels[i]->local[0].busy);
    qcom_glink_poll(&f.g);
    CHECK(!f.g.retry_pending && f.tt==f.th && deliveries==before+3);
}
static void pmic_receive(void *p,QcomGlinkChannel *c,const uint8_t *bytes,uint32_t n)
{
    (void)c; unsigned *received=p;
    CHECK(n==64 && get32(bytes)==0x800b && get32(bytes+8)==0x11 && bytes[12]==0x10 && bytes[13]==1);
    ++*received;
}
/* PMIC and QRTR share the FIFO but not receive-intent ownership. A successful
 * UCSI READ payload must not lose its RX_DONE when QRTR fills the TX side. */
static void pmic_channel_returns(int reuse)
{
    Fixture f;QcomGlinkChannel *qrtr=setup(&f,reuse,1);
    uint8_t storage[64], payload[72]={0};unsigned received=0;
    const QcomGlinkClient client={NULL,pmic_receive};
    QcomGlinkChannel *pmic=qcom_glink_channel(&f.g,"PMIC_RTR_ADSP_APPS",64,1,storage,&client,&received,1);
    CHECK(pmic!=NULL);pmic->open_sent=pmic->open_acked=pmic->remote_open=pmic->up=1;
    pmic->rcid=9;pmic->local[0].advertised=1;
    put32(payload,64);put32(payload+8,0x800b);put32(payload+16,0x11);payload[20]=0x10;payload[21]=1;
    const unsigned before=deliveries;
    f.th=sizeof f.tx-24;
    data(&f,QCOM_GLINK_TX_DATA,1,"lux!",4,0);
    remote(&f,QCOM_GLINK_TX_DATA,9,1,payload,sizeof payload);
    qcom_glink_poll(&f.g);
    CHECK(received==1 && deliveries==before+1 && f.g.done_deferred==2);
    CHECK(qrtr->local[0].done_due && pmic->local[0].done_due && f.g.retry_pending);
    f.tt=f.th;qcom_glink_poll(&f.g);
    unsigned mask=0;
    while(f.tt!=f.th){
        uint8_t h[8];for(unsigned i=0;i<8;++i)h[i]=f.tx[(f.tt+i)%sizeof f.tx];
        const unsigned cmd=h[0],cid=h[2]|(unsigned)h[3]<<8;
        if(cmd==(reuse?QCOM_GLINK_RX_DONE_W_REUSE:QCOM_GLINK_RX_DONE)){
            CHECK(get32(h+4)==1 && (cid==qrtr->lcid || cid==pmic->lcid));
            const unsigned bit=cid==pmic->lcid?2:1;CHECK(!(mask&bit));mask|=bit;
        }
        f.tt=(f.tt+8+(cmd==QCOM_GLINK_INTENT?8*get32(h+4):0))%sizeof f.tx;
    }
    CHECK(mask==3 && f.g.done_retried==2 && received==1);
    qcom_glink_poll(&f.g);
    CHECK(!f.g.retry_pending && !pmic->local[0].busy && received==1 && deliveries==before+1);
}
static unsigned allocations, freed, events, fail_alloc;
static const char *last_reason;
static uint8_t *intent_alloc(uint32_t size) {if(fail_alloc)return NULL; ++allocations;return malloc(size);}
static void intent_free(uint8_t *bytes,uint32_t size) {(void)size;++freed;free(bytes);}
static void intent_event(void *ctx,const QcomGlinkChannel *c,uint16_t cmd,uint16_t rcid,uint32_t size,uint32_t granted,const char *reason)
{(void)ctx;(void)c;(void)cmd;(void)rcid;(void)size;(void)granted;++events;last_reason=reason;}
static void enable_intents(Fixture *f)
{f->g.dynamic_intents=1;f->g.intent_alloc=intent_alloc;f->g.intent_free=intent_free;f->g.intent_event=intent_event;}
static void ask(Fixture *f,uint32_t size)
{remote(f,QCOM_GLINK_RX_INTENT_REQ,7,size,NULL,0);qcom_glink_poll(&f->g);}
static void close_intents(Fixture *f)
{f->tt=f->th;remote(f,QCOM_GLINK_CLOSE,7,0,NULL,0);qcom_glink_poll(&f->g);}
static void intent_counts(void)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,1,1);f.g.intent_event=intent_event;
    const unsigned before=events;
    ask(&f,16);CHECK(take(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,0)==1 && !strcmp(last_reason,"no-idle-unadvertised"));
    ask(&f,32);CHECK(take(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,0)==1 && !strcmp(last_reason,"exceeds-fixed-size"));
    remote(&f,QCOM_GLINK_RX_INTENT_REQ,99,64,NULL,0);qcom_glink_poll(&f.g);
    CHECK(take(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,0)==1 && !strcmp(last_reason,"unknown-channel"));
    CHECK(c->intents.requests==2 && c->intents.refused==2 && !c->intents.granted);
    CHECK(c->intents.last_size==32 && c->intents.min_size==16 && c->intents.max_size==32 && c->intents.size_sum==48);
    CHECK(f.g.intents.requests==3 && f.g.intents.size_sum==112 && events==before+3);
    CHECK(qcom_glink_send(&f.g,c,"lux!",4)==QcomGlinkAgain);
    CHECK(c->intents.requests_tx==1 && c->intents.last_tx_size==4);
    remote(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,7,1,NULL,0);
    remote(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,7,0,NULL,0);
    remote(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,99,0,NULL,0);qcom_glink_poll(&f.g);
    CHECK(c->intents.acks==2 && c->intents.ack_granted==1 && c->intents.ack_refused==1 && !c->intent_asked);
    CHECK(f.g.intents.acks==3 && f.g.intents.ack_refused==2 && events==before+6);
}
static void dynamic_pressure(unsigned room)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,1,1);enable_intents(&f);
    const unsigned before=allocations,old_events=events;
    f.th=sizeof f.tx-24-room;
    ask(&f,64);
    CHECK(c->intents.requests==1 && c->intents.granted==1 && allocations==before+1 && events==old_events+1);
    CHECK(c->intent_count==2 && f.g.dynamic_bytes==64 && f.g.reply_due && f.rt==f.rh);
    CHECK((f.g.reply_offer!=0)==(room==0));
    qcom_glink_poll(&f.g);qcom_glink_poll(&f.g);
    CHECK(c->intents.requests==1 && allocations==before+1 && events==old_events+1);
    f.tt=f.th;qcom_glink_poll(&f.g);
    /* room=0: deferred offer must precede ACK=1, even when it wraps the TX FIFO. */
    if(room==0){uint8_t h[16];for(unsigned i=0;i<16;++i)h[i]=f.tx[(f.tt+i)%sizeof f.tx];
        CHECK(h[0]==QCOM_GLINK_INTENT && get32(h+8)==64 && get32(h+12)==2);}
    CHECK(take(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,1)==1 && !f.g.reply_due && !f.g.reply_offer);
    CHECK(c->local[1].advertised && c->local[1].size==64 && c->local[1].data!=f.storage);
    const unsigned old_deliveries=deliveries;
    data(&f,QCOM_GLINK_TX_DATA,2,"lu",2,2);data(&f,QCOM_GLINK_TX_DATA_CONT,2,"x!",2,0);qcom_glink_poll(&f.g);
    CHECK(deliveries==old_deliveries+1 && take(&f,QCOM_GLINK_RX_DONE_W_REUSE,2)==1);
    CHECK(c->local[1].advertised && !c->local[1].busy);
    const unsigned old_freed=freed;close_intents(&f);
    CHECK(freed==old_freed+1 && !f.g.dynamic_bytes && c->intent_count==1 && !c->local[1].data);
    CHECK(c->intents.requests==1); /* lifetime counters survive reset */
}
static void large_receive(void *ctx,QcomGlinkChannel *c,const uint8_t *bytes,uint32_t n)
{(void)c;CHECK(n==48);for(unsigned i=0;i<n;++i)CHECK(bytes[i]==(uint8_t)i);++*(unsigned *)ctx;}
static void dynamic_capacity(int reuse)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,reuse,1);enable_intents(&f);
    ask(&f,64);CHECK(take(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,1)==1);
    unsigned received=0;c->client.receive=large_receive;c->context=&received;
    uint8_t payload[56];put32(payload,48);put32(payload+4,0);for(unsigned i=0;i<48;++i)payload[i+8]=(uint8_t)i;
    remote(&f,QCOM_GLINK_TX_DATA,7,2,payload,sizeof payload);qcom_glink_poll(&f.g);
    CHECK(received==1 && !f.g.dropped && take(&f,reuse?QCOM_GLINK_RX_DONE_W_REUSE:QCOM_GLINK_RX_DONE,2)==1);
    CHECK(c->local[1].advertised); /* non-reuse: progress re-advertises its real 64B capacity */
    close_intents(&f);CHECK(!f.g.dynamic_bytes);
}
static void dynamic_limits(void)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,1,1);enable_intents(&f);
    const unsigned before=allocations;
    ask(&f,0);CHECK(!strcmp(last_reason,"zero-size"));take(&f,8,0);
    ask(&f,QCOM_GLINK_DYNAMIC_INTENT_MAX+1);CHECK(!strcmp(last_reason,"size-cap"));take(&f,8,0);
    ask(&f,0xffffffffu);CHECK(!strcmp(last_reason,"size-cap"));take(&f,8,0);
    fail_alloc=1;ask(&f,64);CHECK(!strcmp(last_reason,"allocation-failed"));take(&f,8,0);fail_alloc=0;
    CHECK(allocations==before && !f.g.dynamic_bytes && c->intent_count==1);
    for(unsigned i=0;i<4;++i){ask(&f,QCOM_GLINK_DYNAMIC_INTENT_MAX);CHECK(take(&f,8,1)==1);}
    CHECK(f.g.dynamic_bytes==QCOM_GLINK_DYNAMIC_TOTAL_MAX);
    ask(&f,1);CHECK(take(&f,8,0)==1 && !strcmp(last_reason,"total-cap"));
    CHECK(c->intents.requests==9 && c->intents.granted==4 && c->intents.refused==5);
    close_intents(&f);CHECK(!f.g.dynamic_bytes);
    c=setup(&f,1,0);enable_intents(&f);
    for(unsigned i=0;i<QCOM_GLINK_MAX_LOCAL_INTENTS;++i){ask(&f,4);CHECK(take(&f,8,1)==1);}
    ask(&f,4);CHECK(take(&f,8,0)==1 && !strcmp(last_reason,"slot-cap"));
    CHECK(c->intent_count==QCOM_GLINK_MAX_LOCAL_INTENTS && f.g.dynamic_bytes==4*QCOM_GLINK_MAX_LOCAL_INTENTS);
    close_intents(&f);CHECK(!f.g.dynamic_bytes && !c->intent_count);
    CHECK(allocations==freed);
}
static void framing(void)
{
    Fixture f;
    QcomGlinkChannel *c = setup(&f, 1, 1);
    const unsigned before = deliveries;
    f.rt = f.rh = sizeof f.rx - 16;
    data(&f, QCOM_GLINK_TX_DATA, 1, "lux!", 4, 0);
    const uint32_t padded_head = f.rh;
    f.rh -= 4;
    const uint32_t tail = f.rt;
    CHECK(qcom_glink_poll(&f.g) == 0 && f.rt == tail && deliveries == before);
    f.rh = padded_head;
    CHECK(qcom_glink_poll(&f.g) == 1 && f.rt == f.rh && deliveries == before + 1);

    c = setup(&f, 1, 1);
    const char name[] = "IPCRTR";
    remote(&f, QCOM_GLINK_OPEN, 8, sizeof name, (const uint8_t *)name, sizeof name);
    --f.rh;
    CHECK(qcom_glink_poll(&f.g) == 0 && f.rt == 0 && c->rcid == 7);
    ++f.rh;
    CHECK(qcom_glink_poll(&f.g) == 1 && f.rt == f.rh && c->rcid == 8);

    c = setup(&f, 1, 1);
    uint8_t payload[8] = {0};
    put32(payload, sizeof f.rx - 16);
    remote(&f, QCOM_GLINK_TX_DATA, 7, 1, payload, sizeof payload);
    CHECK(qcom_glink_poll(&f.g) == QcomGlinkCorrupt && f.rt == 0);

    c = setup(&f, 1, 1);
    CHECK(qcom_glink_send(&f.g, c, "lux!", UINT32_MAX) == QcomGlinkTooBig);
    CHECK(f.th == 0 && !c->intent_asked);
}

static void ownership_under_pressure(void)
{
    Fixture f;
    QcomGlinkChannel *c = setup(&f, 1, 1);
    const unsigned before = deliveries;
    f.th = sizeof f.tx - 24;
    data(&f, QCOM_GLINK_TX_DATA, 1, "lux!", 4, 0);
    data(&f, QCOM_GLINK_TX_DATA, 1, "lux!", 4, 0);
    CHECK(qcom_glink_poll(&f.g) == 2 && deliveries == before + 1 && f.g.dropped == 1);
    CHECK(c->local[0].busy && c->local[0].done_due && f.g.done_deferred == 1);
    f.tt = f.th;
    qcom_glink_poll(&f.g);
    CHECK(take(&f, QCOM_GLINK_RX_DONE_W_REUSE, 1) == 1);

    c = setup(&f, 1, 1);
    f.th = sizeof f.tx - 32;  /* Only one ordinary command fits. */
    remote(&f, QCOM_GLINK_CLOSE, 7, 0, NULL, 0);
    CHECK(qcom_glink_poll(&f.g) == 0 && c->up && f.rt == 0);
    f.tt = sizeof f.tx - 32;
    CHECK(take(&f, QCOM_GLINK_CLOSE_ACK, 0) == 0);
    CHECK(qcom_glink_poll(&f.g) == 1 && !c->up && f.rt == f.rh);
    CHECK(take(&f, QCOM_GLINK_CLOSE_ACK, 0) == 1);
}

int main(void)
{
    for(int reuse=0;reuse<=1;++reuse)for(int frag=0;frag<=1;++frag)completion(reuse,frag,1);
    intent_grant();
    multiple_returns();
    close_reopen();
    shared_channel_returns();
    pmic_channel_returns(0);pmic_channel_returns(1);
    intent_counts();dynamic_pressure(0);dynamic_pressure(16);
    dynamic_capacity(0);dynamic_capacity(1);dynamic_limits();
    framing();
    ownership_under_pressure();
    printf("glink_return_host: %u checks, %u failures, %u delivered, %u kicks\n",checks,failures,deliveries,kicks);
    return failures?1:0;
}
