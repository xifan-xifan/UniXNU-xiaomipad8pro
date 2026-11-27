/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Copyright (c) 2026 Leo Zhang.
 * Explicit root-only PianoAudio engineering diagnostic, not a HAL recorder.
 * Uses the published mic-delivery1 ABI; never requests or changes TCC access.
 * Build: clang -Wall -Wextra -Werror -O2 piano-mic-diag.c -framework IOKit
 *        -framework CoreFoundation -o piano-mic-diag
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../package/PianoAudio/AudioTiming.h"
#include "audio-cli.h"
#include "audio-wav.h"

enum {
    RATE = PIANO_AUDIO_RATE,
    RING = PIANO_AUDIO_RING_FRAMES,
    BLOCK = PIANO_AUDIO_PERIOD_FRAMES,
    LAG = 2 * PIANO_AUDIO_SAFETY_FRAMES,
    MAX_AGE = RING - 2 * PIANO_AUDIO_SAFETY_FRAMES
};
enum ReadState { READ_WAIT, READ_READY, ANCHOR_AHEAD, ANCHOR_STALE, READER_LAG };
static const char *const read_failures[] = {
    [ANCHOR_AHEAD] = "anchor_ahead",
    [ANCHOR_STALE] = "anchor_stale",
    [READER_LAG] = "reader_lag"
};
typedef struct { uint64_t sample, host, seed, reserved, end; } Status;
static volatile sig_atomic_t interrupted;
static void stop_signal(int sig) { (void)sig; interrupted=1; }
static uint64_t load64(const uint64_t *p) { return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static mach_timebase_info_data_t tb;
static __uint128_t ns(uint64_t ticks) { return (__uint128_t)ticks*tb.numer/tb.denom; }
static uint64_t frame_clock(uint64_t ticks) { return (uint64_t)(ns(ticks)*RATE/1000000000); }
static uint64_t distance(uint64_t a,uint64_t b) { return a>b ? a-b : b-a; }

/* The engine publishes a completed wrap. Keep the block end two input safety
 * offsets behind its clock and reserve another two before overwrite. Kernel
 * wake and FIFO diagnostics independently check producer timing. */
static enum ReadState readable(uint64_t clock, uint64_t anchor, uint64_t next)
{
    if (anchor > clock) return ANCHOR_AHEAD;
    if (clock - anchor > RING + LAG) return ANCHOR_STALE;
    if (clock < next || clock - next < BLOCK + LAG) return READ_WAIT;
    if (clock - next > MAX_AGE) return READER_LAG;
    return READ_READY;
}
static int selftest(void)
{
    int16_t ring[RING], block[BLOCK];
    uint64_t next=0, producer=0;
    for (uint64_t clock=0;clock<=5u*RATE+LAG;clock+=96) {
        /* The producer keeps running during a 100 ms pause in reader polls. */
        if (clock == 2u * RATE) clock += RATE / 10;
        uint64_t complete=clock/PIANO_AUDIO_LEGACY_PERIOD_FRAMES*PIANO_AUDIO_LEGACY_PERIOD_FRAMES;
        for (;producer<complete;++producer) ring[producer%RING]=(int16_t)(producer*13+7);
        while (readable(clock,complete/RING*RING,next)==READ_READY) {
            if (clock + RATE / 500 > next + MAX_AGE) return 1;
            piano_audio_copy_from_ring(block,ring,RING,(uint32_t)(next%RING),BLOCK,1);
            for (unsigned j=0;j<BLOCK;++j) if (block[j]!=(int16_t)((next+j)*13+7)) return 1;
            next+=BLOCK;
        }
    }
    if (next != 5u * RATE ||
        readable(RING + LAG + 1, 0, 0) != ANCHOR_STALE ||
        readable(MAX_AGE + 1, 0, 0) != READER_LAG ||
        readable(MAX_AGE, 0, 0) != READ_READY ||
        readable(1000, 2000, 0) != ANCHOR_AHEAD ||
        readable(UINT64_MAX, UINT64_MAX, UINT64_MAX - 1) != READ_WAIT ||
        readable(BLOCK + LAG - 1, 0, 0) != READ_WAIT) return 1;
    puts("SELFTEST_PASS chronological 5s PCM across wraps and a 100ms reader stall, stale/overrun/future/early bounds");
    return 0;
}
static CFTypeRef prop(io_service_t s,CFStringRef key)
{ return IORegistryEntryCreateCFProperty(s,key,kCFAllocatorDefault,0); }
static int number(io_service_t s,CFStringRef key,int *v)
{
    CFTypeRef p=prop(s,key); int ok=p && CFGetTypeID(p)==CFNumberGetTypeID() && CFNumberGetValue(p,kCFNumberIntType,v);
    if (p) CFRelease(p);
    return ok;
}
static int equal(io_service_t s,CFStringRef key,CFStringRef value)
{ CFTypeRef p=prop(s,key); int ok=p && CFEqual(p,value); if (p) CFRelease(p); return ok; }
static int boolean(io_service_t s,CFStringRef key,int *v)
{
    CFTypeRef p=prop(s,key); int ok=p && CFGetTypeID(p)==CFBooleanGetTypeID();
    if (ok) *v=CFBooleanGetValue(p);
    if (p) CFRelease(p);
    return ok;
}
static io_service_t find_mic(void)
{
    io_iterator_t it=0; io_service_t found=0,s; unsigned matches=0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("PianoAudioDevice"),&it)) return 0;
    while ((s=IOIteratorNext(it))) {
        if (equal(s,CFSTR("device UID"),CFSTR("Mic")) &&
            equal(s,CFSTR("piano-audio-input-contract"),CFSTR("mic-delivery1:inpt/main:mono:db-linear:pre-HAL"))) {
            ++matches; if (!found) {found=s; continue;}
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (matches!=1) { if (found) IOObjectRelease(found); return 0; }
    return found;
}
static int control(io_connect_t c,io_service_t s,int level,int mute)
{
    uint64_t args[2]={1,(uint64_t)level};
    kern_return_t a=IOConnectCallScalarMethod(c,2,args,2,NULL,NULL);
    args[0]=2; args[1]=(uint64_t)mute;
    kern_return_t b=IOConnectCallScalarMethod(c,2,args,2,NULL,NULL);
    int l=-1,m=-1;
    int ok=number(s,CFSTR("piano-audio-input-level"),&l) && boolean(s,CFSTR("piano-audio-input-mute"),&m);
    printf("CONTROL level=%d mute=%d level_rc=0x%x mute_rc=0x%x read_level=%d read_mute=%d %s\n",
           level,mute,a,b,l,m,!a&&!b&&ok&&l==level&&m==mute?"PASS":"FAIL");
    return a||b||!ok||l!=level||m!=mute;
}
static int capture(io_connect_t c,io_service_t service,const char *prefix,unsigned seconds)
{
    char wav[1024],ready[1024],stop[1024],done[1024];
    if (snprintf(wav,sizeof wav,"%s.wav",prefix)>=(int)sizeof wav ||
        snprintf(ready,sizeof ready,"%s.ready",prefix)>=(int)sizeof ready ||
        snprintf(stop,sizeof stop,"%s.stop",prefix)>=(int)sizeof stop ||
        snprintf(done,sizeof done,"%s.done",prefix)>=(int)sizeof done) return 1;
    int running=1,frames=0;
    if (!boolean(service,CFSTR("is running"),&running) || running) {
        puts("HARNESS_FAIL check=engine_idle");return 1;
    }
    if (!number(service,CFSTR("io buffer frame size"),&frames) || frames!=RING) {
        puts("HARNESS_FAIL check=ring_abi");return 1;
    }
    if (access(ready,F_OK)==0 || access(stop,F_OK)==0 || access(done,F_OK)==0) {
        puts("HARNESS_FAIL check=unused_prefix");return 1;
    }
    FILE *f=fopen(wav,"wbx"); if (!f) {perror("exclusive wav");return 1;}
    const char *failure = audio_wav_write_header(f,RATE,0) ? "wav_header" : NULL;
    int started=0;
    mach_vm_address_t sa=0,ra=0; mach_vm_size_t sl=0,rl=0;
    uint64_t next=0,seed=0,previous_clock=0,blocks=0,anchors=0,last_anchor=0;
    uint64_t start=mach_absolute_time(),origin=0;
    uint64_t sample=0,host=0,now=0,clock=0,expected=0,observed_seed=0;
    uint64_t gap=0,max_gap=0,check_clock=0,torn_pairs=0;
    if (!failure && IOConnectMapMemory64(c,0,mach_task_self(),&sa,&sl,kIOMapAnywhere|kIOMapReadOnly)) failure="map_status";
    if (!failure && IOConnectMapMemory64(c,0x10000001,mach_task_self(),&ra,&rl,kIOMapAnywhere|kIOMapReadOnly)) failure="map_ring";
    if (!failure && (sl<sizeof(Status) || rl<RING*2)) failure="map_size";
    if (!failure) {
        if (IOConnectCallScalarMethod(c,0,NULL,0,NULL,NULL)) failure="start_io";
        else started=1;
    }
    const Status *s=(const Status *)(uintptr_t)sa;
    const int16_t *ring=(const int16_t *)(uintptr_t)ra;
    int16_t block[BLOCK],again[BLOCK];
    if (!failure) {
        observed_seed=seed=load64(&s->seed);
        host=origin=load64(&s->host);
        sample=load64(&s->sample);
        if (!seed || !origin || origin>mach_absolute_time() || sample) failure="initial_anchor";
    }
    int announced=0,stopped=0;
    while (!failure && !interrupted && ns(mach_absolute_time()-start)<(uint64_t)seconds*1000000000) {
        if (access(stop,F_OK)==0) {stopped=1;break;}
        sample=load64(&s->sample);
        host=load64(&s->host);
        now=mach_absolute_time();
        observed_seed=load64(&s->seed);
        check_clock=expected=0;
        clock=frame_clock(now-origin);
        gap=previous_clock ? clock-previous_clock : 0;
        if (gap>max_gap) max_gap=gap;
        if (observed_seed!=seed) {failure="seed_change";break;}
        if (host>now || host<origin) {failure="host_time";break;}
        if (sample%RING) {failure="anchor_alignment";break;}
        /* Host is stored before sample. Only retry a split snapshot while
         * the unread block is still inside the ring's safe window. */
        expected=frame_clock(host-origin);
        if (distance(expected,sample)>2) {
            ++torn_pairs;
            if (clock>next+MAX_AGE) {failure="torn_pair";break;}
            usleep(1000);
            continue;
        }
        if (sample!=last_anchor) {++anchors;last_anchor=sample;}
        previous_clock=clock;
        /* A coherent host tick may round down by a frame in conversion. */
        enum ReadState state=readable(clock<sample ? sample : clock,sample,next);
        if (read_failures[state]) {failure=read_failures[state];break;}
        if (state==READ_READY) {
            piano_audio_copy_from_ring(block,ring,RING,(uint32_t)(next%RING),BLOCK,1);
            usleep(2000);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            piano_audio_copy_from_ring(again,ring,RING,(uint32_t)(next%RING),BLOCK,1);
            check_clock=frame_clock(mach_absolute_time()-origin);
            observed_seed=load64(&s->seed);
            if (observed_seed!=seed) {failure="reread_seed_change";break;}
            if (check_clock>next+MAX_AGE) {failure="reread_lag";break;}
            if (memcmp(block,again,sizeof block)) {failure="reread_mismatch";break;}
            if (next>AUDIO_WAV_MAX_MONO_FRAMES-BLOCK) {failure="wav_size";break;}
            if (fwrite(block,1,sizeof block,f)!=sizeof block) {failure="wav_write";break;}
            next+=BLOCK;++blocks;
            if (!announced && next>=2u*RATE) {
                FILE *r=fopen(ready,"wx");
                if (!r) {failure="ready_open";break;}
                int written=fprintf(r,"READY seed=%llu frames=%llu root=1 direct-driver=1\n",(unsigned long long)seed,(unsigned long long)next);
                if (fclose(r) || written<0) {failure="ready_write";break;}
                announced=1;
            }
        } else usleep(1000);
    }
    if (!failure && interrupted) failure="interrupted";
    if (!failure && !stopped) failure="stop_timeout";
    if (!failure && !announced) failure="stop_before_ready";
    kern_return_t sr=started?IOConnectCallScalarMethod(c,1,NULL,0,NULL,NULL):KERN_SUCCESS;
    if (!failure && sr) failure="stop_io";
    kern_return_t ru=ra?IOConnectUnmapMemory64(c,0x10000001,mach_task_self(),ra):KERN_SUCCESS;
    kern_return_t su=sa?IOConnectUnmapMemory64(c,0,mach_task_self(),sa):KERN_SUCCESS;
    int header_rc=audio_wav_write_header(f,RATE,next);
    int close_rc=fclose(f);
    if (!failure && (ru || su)) failure=ru ? "unmap_ring" : "unmap_status";
    if (!failure && header_rc) failure="wav_finalize";
    if (!failure && close_rc) failure="wav_close";
    if (!failure) {
        FILE *d=fopen(done,"wx");
        if (!d) failure="done_open";
        else {
            int written=fprintf(d,"CAPTURE_DONE frames=%llu seed=%llu\n",
                                (unsigned long long)next,(unsigned long long)seed);
            if (fclose(d) || written<0) {
                failure="done_write";
                unlink(done);
            }
        }
    }
    if (failure) {
        printf("CAPTURE_CHECK_FAIL check=%s seed=%llu observed_seed=%llu next=%llu sample=%llu expected=%llu "
               "clock=%llu gap=%llu check_clock=%llu host=%llu origin=%llu now=%llu\n",
               failure,(unsigned long long)seed,(unsigned long long)observed_seed,(unsigned long long)next,
               (unsigned long long)sample,(unsigned long long)expected,(unsigned long long)clock,
               (unsigned long long)gap,(unsigned long long)check_clock,(unsigned long long)host,
               (unsigned long long)origin,(unsigned long long)now);
    }
    printf("CAPTURE_TIMING max_gap_frames=%llu torn_pairs=%llu\n",
           (unsigned long long)max_gap,(unsigned long long)torn_pairs);
    printf("CAPTURE %s seed=%llu frames=%llu blocks=%llu anchors=%llu stop_rc=0x%x unmap_ring=0x%x unmap_status=0x%x\n",
        failure?"FAIL":"PASS",(unsigned long long)seed,(unsigned long long)next,(unsigned long long)blocks,(unsigned long long)anchors,sr,ru,su);
    return failure!=NULL;
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--selftest")) return selftest();
    int ctl=argc==4&&!strcmp(argv[1],"--control"),rec=argc==4&&!strcmp(argv[1],"--capture");
    if ((!ctl&&!rec)||geteuid()!=0) {fputs("usage (root): --control LEVEL MUTE | --capture UNIQUE_PREFIX MAX_SECONDS; --selftest needs no device\n",stderr);return 2;}
    umask(0077);
    long seconds=0,level=0,mute=0;
    if (ctl) {
        if (!audio_parse_long(argv[2],0,100,&level) || !audio_parse_long(argv[3],0,1,&mute)) return 2;
    } else if (!argv[2][0] || !audio_parse_long(argv[3],5,180,&seconds)) return 2;
    if (rec && (mach_timebase_info(&tb)!=KERN_SUCCESS || !tb.numer || !tb.denom)) return 1;
    io_service_t s=find_mic();io_connect_t c=0;
    if (!s) {puts("HARNESS_FAIL unique Mic/mic-delivery1 contract missing");return 2;}
    kern_return_t kr=IOServiceOpen(s,mach_task_self(),0,&c);
    if (kr) {printf("HARNESS_FAIL IOServiceOpen 0x%x\n",kr);IOObjectRelease(s);return 2;}
    signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);signal(SIGHUP,stop_signal);
    int rc=ctl?control(c,s,(int)level,(int)mute):capture(c,s,argv[2],(unsigned)seconds);
    kr=IOServiceClose(c);if (kr)rc=1;IOObjectRelease(s);
    printf("CONNECTION_CLOSED rc=0x%x result=%d\n",kr,rc);
    return rc;
}
