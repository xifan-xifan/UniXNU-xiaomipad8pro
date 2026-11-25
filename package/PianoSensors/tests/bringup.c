// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, Leo Zhang. All rights reserved.
// Host tests of the file service's bring-up after the tablet's first S2 (batch-2 adsp run, 2026-11-14): attach, remotectl open, register
// and close were answered 0, adsp_listener init2 (handle 3, method 3, no buffers) was answered AEE EBADPARM 0x8000040e, the service
// stopped, and 59 s after the attach the sensor PD's watchdog took the whole ADSP (sns_watchdog.c:84, qsh_process).
//
// Against a simulated DSP written from Linux's fastrpc.c as facts (no code shared with SensorsRpc.c): every invoke carries the metadata
// page (fastrpc_internal_invoke calls fastrpc_get_args for every invoke: n {pv, len}, n {num, pgidx}, n {addr, size}, 16 u64 fds, 64 u32
// crcs; the message's addr its page with the bank in bits 32 and up, size whole pages), ctx = id << 4 | PD in Linux's tree, the
// message's tid the calling task's, the release INIT_RELEASE = handle 1, method 1, one in buffer {client id}, client id 0 in the message.
// The DSP's init2 is scripted by what the message carries, which is how the three framings are told apart; the tablet decides which.
//
// Covered: the default framing byte for byte (register and init2 with a page of 384 zero bytes over junk in the arena, ctx id << 8,
// the tid the client id), the tablet's failure reproduced with PaganiSensors' framing and the session released, the ladder (pag ->
// meta, meta -> meta+ctx4 -> meta+ctx4+tid, the framing kept for next2), the ladder exhausted (released, no next2), the release withheld
// from a DSP the glue says may not be told, the deadline (an open never answered: released well inside the PD's minute), an abort by
// the glue, no release after a link loss, and the guard's file-service strike.
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "check.h"
#include "../SensorsArena.h"
#include "../SensorsGuard.h"
#include "../SensorsRpc.h"

#define ARENA_PA 0x9d200000ull
#define CLIENT 1u                     /* Linux's first client id: the kext's */
#define BANK 3u
#define EBADPARM ((int32_t)0x8000040e)   /* DSP_AEE_EOFFSET 0x80000400 + AEE_EBADPARM 14: the tablet's answer */

static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint64_t rd64(const uint8_t *p) {return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;}
static void wr32(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));}
static void wr64(uint8_t *p, uint64_t v) {wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));}

static uint8_t *arena;
static uint8_t *shadow;

/* The arena as the tablet has it, as far as a host can: unreadable (PROT_NONE) except inside SensorsArena.c's functions (SAR_CHECK's
 * enter/leave) and the simulated DSP's own turn, and every access SensorsArena.c makes is checked for its natural alignment. A direct
 * access to the arena anywhere else in the service, like s1b's merged get32, ends the test with a fault. */
static int arena_depth;
static unsigned long arena_accesses, arena_bytes[9];
static void arena_open(void)
{
    if (arena_depth++ == 0) CHECK(!mprotect(arena, SSR_ARENA_BYTES, PROT_READ | PROT_WRITE));
}
static void arena_close(void)
{
    CHECK(arena_depth > 0);
    if (--arena_depth == 0) CHECK(!mprotect(arena, SSR_ARENA_BYTES, PROT_NONE));
}
void sar_enter(void) {arena_open();}
void sar_leave(void) {arena_close();}
void sar_access(const volatile void *address, unsigned bytes)
{
    const uintptr_t a = (uintptr_t)address;
    CHECK(bytes == 1 || bytes == 4 || bytes == 8);
    CHECK(!(a & (bytes - 1)));                                            /* naturally aligned, or a byte */
    CHECK(a >= (uintptr_t)arena && a + bytes <= (uintptr_t)arena + SSR_ARENA_BYTES);
    CHECK(arena_depth > 0);
    arena_accesses++;
    arena_bytes[bytes]++;
}
static uint8_t bounce[SSR_BOUNCE];
static SsrCore core;
static uint64_t clock_ms;
static char logbuf[1 << 16];
static size_t logused;

/* The DSP's script and record. */
enum {INIT_NEEDS_PAGE, INIT_NEEDS_TID, INIT_NEVER, INIT_ALWAYS};
static struct {
    int init_rule, silent_open, tell;
    int pending;
    uint8_t m[SSR_MESSAGE];
    unsigned messages, attaches, opens, registers, closes, inits, next2s, releases;
    uint64_t attach_at, release_at;
    uint32_t init_tid[4], init_ctx_low[4], init_page[4];
    uint64_t next2_ctx;
    uint32_t next2_tid;
    uint32_t order[16];
    unsigned mmaps;
} dsp;

/* The DSP's calls, delivered with next2 (the listener's blob: each in buffer a u32 length, aligned to 8 when not empty, its bytes; then
 * a u32 per out buffer, unaligned after the last in buffer), and what the answer must be (-999: any). */
typedef struct {
    uint32_t handle, method;                  /* method above 30: sent as 31 with the id as in buffer 0's first word */
    unsigned in_count, out_count;
    const uint8_t *in[5];
    uint32_t in_n[5], out_n[4];
    int32_t result;
    void (*check)(uint8_t *const out[4]);     /* the answer's out buffers (result 0) */
} DspCall;
static const DspCall *script;
static unsigned script_count, delivered, answered, fuzz_left;
static DspCall fuzz_call;
static uint8_t fuzz_bytes[5][48];
static uint32_t fuzz_seed = 12345;
static uint32_t last_length;
static const DspCall *in_flight;                 /* the call whose answer the next next2 brings */
static int session_ended;                        /* the DSP had no more calls: it took the link away */

static uint32_t pack_call(uint8_t *blob, uint32_t cap, const DspCall *k)
{
    uint32_t at = 0;
    for (unsigned i = 0; i < k->in_count; ++i) {
        CHECK(at + 4 <= cap);
        wr32(blob + at, k->in_n[i]);
        at += 4;
        if (!k->in_n[i]) continue;
        at = (at + 7) & ~7u;
        CHECK(at + k->in_n[i] <= cap);
        memcpy(blob + at, k->in[i], k->in_n[i]);
        at += k->in_n[i];
    }
    for (unsigned i = 0; i < k->out_count; ++i) {
        CHECK(at + 4 <= cap);
        wr32(blob + at, k->out_n[i]);
        at += 4;
    }
    return at;
}

/* The previous call's answer as next2 brings it back: its ctx, its result, its out buffers exactly as asked. */
static void check_answer(const DspCall *k, uint32_t ctx, int32_t result, const uint8_t *blob, uint32_t length)
{
    CHECK(ctx == 0x100 + answered);
    if (k->result != -999) CHECK(result == k->result);
    answered++;
    if (result) {
        CHECK(length == 0);
        return;
    }
    static uint8_t copy[0x10000];
    CHECK(length <= sizeof copy);
    memcpy(copy, blob, length);
    uint8_t *out[4] = {0};
    uint32_t at = 0;
    for (unsigned i = 0; i < k->out_count; ++i) {
        CHECK(at + 4 <= length && rd32(copy + at) == k->out_n[i]);
        at += 4;
        if (!k->out_n[i]) continue;
        at = (at + 7) & ~7u;
        out[i] = copy + at;
        at += k->out_n[i];
    }
    CHECK(at == length);
    if (k->check) k->check(out);
}

static uint32_t fuzz_next(void) {fuzz_seed = fuzz_seed * 1103515245u + 12345u; return fuzz_seed >> 8;}
static const DspCall *fuzz_make(void)
{
    static const uint32_t handles[4] = {0, 1, 2, 9};
    DspCall *k = &fuzz_call;
    memset(k, 0, sizeof *k);
    k->handle = handles[fuzz_next() % 4];
    k->method = fuzz_next() % 36;
    k->in_count = 1 + fuzz_next() % 4;
    k->out_count = fuzz_next() % 4;
    for (unsigned i = 0; i < k->in_count; ++i) {
        k->in_n[i] = fuzz_next() % 41;
        for (unsigned b = 0; b < sizeof fuzz_bytes[i]; ++b) fuzz_bytes[i][b] = (uint8_t)fuzz_next();
        if (k->in_n[i] && fuzz_next() % 2) fuzz_bytes[i][k->in_n[i] - 1] = 0;    /* often a string */
        k->in[i] = fuzz_bytes[i];
    }
    if (k->method > 30) wr32(fuzz_bytes[0], 31 + fuzz_next() % 3);
    for (unsigned i = 0; i < k->out_count; ++i) k->out_n[i] = fuzz_next() % 300;
    k->result = -999;
    return k;
}

static uint64_t op_now(void *c) {(void)c; return clock_ms;}
static void op_trace(void *c, const char *line)
{
    (void)c;
    const size_t n = strlen(line);
    if (logused + n + 2 < sizeof logbuf) {
        memcpy(logbuf + logused, line, n);
        logused += n;
        logbuf[logused++] = '\n';
        logbuf[logused] = 0;
    }
}
static int logged(const char *text) {return strstr(logbuf, text) != NULL;}
static int op_send(void *c, const uint8_t *m, uint32_t n) {(void)c; CHECK(n == SSR_MESSAGE && !dsp.pending); memcpy(dsp.m, m, n); dsp.pending = 1; return 0;}
static void op_publish(void *c) {(void)c;}
static int op_prepare(void *c) {(void)c; return 0;}
static int op_pack_size(void *c, uint64_t *size) {(void)c; *size = 0; return SsrPackAbsent;}
static int op_pack_read(void *c, uint64_t o, void *to, uint32_t n) {(void)c; (void)o; (void)to; (void)n; return 1;}
static void *op_alloc(void *c, uint32_t n) {(void)c; return calloc(1, n);}
static int op_admit(void *c) {(void)c; return 1;}
static int op_tellable(void *c) {(void)c; return dsp.tell;}

/* The metadata the way Linux lays it out; the bytes of buffer i (bank bits dropped), NULL when empty. */
static const uint8_t *metadata_of(const uint8_t *m, uint32_t *n)
{
    const uint32_t sc = rd32(m + 20);
    const uint64_t addr = rd64(m + 24), size = rd64(m + 32);
    *n = (sc >> 16 & 0xff) + (sc >> 8 & 0xff);
    if (!addr && !size) return NULL;                  /* PaganiSensors' framing of an invoke without buffers */
    CHECK(addr >> 32 == BANK && !(addr & 0xfff) && size == 0x1000);
    const uint64_t pa = addr & 0xffffffffull;
    CHECK(pa >= ARENA_PA && pa + size <= ARENA_PA + SSR_ARENA_BYTES);
    const uint8_t *meta = arena + (pa - ARENA_PA);
    for (unsigned i = 0; i < 16 * 8 + 64 * 4; ++i) CHECK(meta[*n * 40 + i] == 0);   /* fds and crcs zero (the arena was junk) */
    return meta;
}
static uint8_t *buffer(const uint8_t *meta, uint32_t n, uint32_t i, uint64_t *len)
{
    CHECK(meta && i < n);
    const uint8_t *list = meta + n * 16, *pages = list + n * 8;
    const uint64_t pv = rd64(meta + i * 16);
    *len = rd64(meta + i * 16 + 8);
    if (!*len) return NULL;
    const uint64_t page = rd64(pages + i * 16);
    CHECK(page >> 32 == BANK && rd32(list + i * 8) == 1);
    return arena + ((page & 0xffffffffull) - ARENA_PA) + (pv & 0xfff);
}

static void respond(int32_t value)
{
    uint8_t r[16] = {0};
    wr64(r, rd64(dsp.m + 8));
    wr32(r + 8, (uint32_t)value);
    dsp.pending = 0;
    ssr_core_receive(&core, r, sizeof r);
}

/* One message of the service, answered (or not) as the script says. */
static void dsp_turn(void)
{
    const uint8_t *m = dsp.m;
    const uint32_t pid = rd32(m), tid = rd32(m + 4), handle = rd32(m + 16), sc = rd32(m + 20), method = sc >> 24 & 0x1f;
    const uint64_t ctx = rd64(m + 8);
    uint32_t n = 0;
    uint64_t len = 0;
    const uint8_t *meta = metadata_of(m, &n);
    CHECK((ctx & 0xf) == SSR_PD_SENSORS && !(sc & 0xff) && !(sc >> 29));
    if (n) CHECK(meta);                               /* buffers always come with their metadata */
    if (dsp.messages < 16) dsp.order[dsp.messages] = handle << 8 | method;
    dsp.messages++;
    if (handle == 1 && method == 0) {                 /* attach */
        CHECK(pid == 0 && sc == 0x00010000u && rd32(buffer(meta, n, 0, &len)) == CLIENT && len == 4);
        dsp.attaches++;
        dsp.attach_at = clock_ms;
        respond(0);
        return;
    }
    if (handle == 1 && method == 1) {                 /* release: Linux's INIT_RELEASE */
        CHECK(pid == 0 && sc == 0x01010000u && meta && rd32(buffer(meta, n, 0, &len)) == CLIENT && len == 4);
        dsp.releases++;
        dsp.release_at = clock_ms;
        respond(0);
        return;
    }
    if (handle == 1 && method == 4) {                 /* apps_mem's mmap (a fuzzed map64): the address the DSP gives the pages */
        CHECK(pid == 0 && sc == 0x04020100u);
        wr64(buffer(meta, n, 2, &len), 0xe0000000ull);
        dsp.mmaps++;
        respond(0);
        return;
    }
    CHECK(pid == CLIENT);
    if (handle == 0 && method == 0) {                 /* remotectl open */
        CHECK(sc == 0x00020200u);
        dsp.opens++;
        if (dsp.silent_open) {dsp.pending = 0; return;}    /* never answered */
        uint8_t *out = buffer(meta, n, 2, &len);
        wr32(out, 0x77);
        wr32(out + 4, 0);
        respond(0);
        return;
    }
    if (handle == 0x77) {                             /* register: no buffers, so a page of 384 zero bytes under the default */
        CHECK(sc == 0 && n == 0);
        dsp.registers++;
        respond(0);
        return;
    }
    if (handle == 0 && method == 1) {dsp.closes++; respond(0); return;}
    CHECK(handle == 3);
    if (method == 3) {                                /* init2: no buffers */
        CHECK(sc == 0x03000000u && n == 0);
        const unsigned i = dsp.inits++;
        if (i < 4) {
            dsp.init_tid[i] = tid;
            dsp.init_ctx_low[i] = (uint32_t)(ctx & 0xff);
            dsp.init_page[i] = meta != NULL;
        }
        int ok = 0;
        switch (dsp.init_rule) {
        case INIT_NEEDS_PAGE: ok = meta != NULL; break;
        case INIT_NEEDS_TID: ok = meta != NULL && tid != pid; break;
        case INIT_ALWAYS: ok = 1; break;
        default: ok = 0; break;
        }
        respond(ok ? 0 : EBADPARM);
        return;
    }
    CHECK(method == 4 && sc == 0x04020200u);          /* next2: the previous answer, then the next call (or the session ends) */
    dsp.next2s++;
    dsp.next2_ctx = ctx;
    dsp.next2_tid = tid;
    uint64_t prim_n = 0, prev_n = 0, out_n = 0, cap = 0;
    const uint8_t *prim = buffer(meta, n, 0, &prim_n);
    const uint8_t *prev = buffer(meta, n, 1, &prev_n);
    uint8_t *out = buffer(meta, n, 2, &out_n), *blob = buffer(meta, n, 3, &cap);
    CHECK(prim_n == 16 && rd32(prim + 8) == prev_n && rd32(prim + 12) == cap && out_n == 16);
    if (in_flight) check_answer(in_flight, rd32(prim), (int32_t)rd32(prim + 4), prev, (uint32_t)prev_n);
    else CHECK(rd32(prim) == 0 && rd32(prim + 4) == 0xffffffffu && prev_n == 0);   /* the first next2 */
    const DspCall *k = script && delivered < script_count ? &script[delivered] : fuzz_left ? fuzz_make() : NULL;
    if (!k) {
        session_ended = 1;
        dsp.pending = 0;
        ssr_core_channel(&core, 0);                    /* the test ends the session by taking the link away */
        return;
    }
    if (!(script && delivered < script_count)) fuzz_left--;
    const uint32_t length = pack_call(blob, (uint32_t)cap, k);
    last_length = length;
    const uint32_t m31 = k->method > 30 ? 31 : k->method;
    wr32(out, 0x100 + delivered);
    wr32(out + 4, k->handle);
    wr32(out + 8, m31 << 24 | k->in_count << 16 | k->out_count << 8);
    wr32(out + 12, length);
    delivered++;
    in_flight = k;
    respond(0);
}

/* The DSP answers within a millisecond; with nothing to answer the wait runs its whole time. */
static void op_wait(void *c, uint32_t ms)
{
    (void)c;
    if (dsp.pending) {
        clock_ms += 1;
        arena_open();                                  /* the DSP's own turn: it reads and writes the arena as it likes */
        dsp_turn();
        arena_close();
        return;
    }
    clock_ms += ms ? ms : 1;
}

static const SsrOps ops = {NULL, op_now, op_send, op_wait, op_trace, op_publish, op_prepare, op_pack_size, op_pack_read, op_alloc,
                           op_admit, op_tellable};

static void start(uint32_t frame, uint32_t no_ladder, int rule)
{
    memset(&dsp, 0, sizeof dsp);
    dsp.init_rule = rule;
    dsp.tell = 1;
    arena_open();
    memset(arena, 0x5a, SSR_ARENA_BYTES);             /* junk: the page's fds and crcs must be cleared by the service */
    arena_close();
    script = NULL;
    script_count = delivered = answered = fuzz_left = 0;
    in_flight = NULL;
    session_ended = 0;
    logused = 0;
    logbuf[0] = 0;
    clock_ms = 1000;
    const SsrConfig cfg = {arena, ARENA_PA, SSR_ARENA_BYTES, bounce, CLIENT, BANK, 2, frame, no_ladder, 0, shadow};
    ssr_core_init(&core, &ops, &cfg);
    ssr_core_channel(&core, 1);
}

/* Steps until the session serves (a next2 is out), stops, or the step budget ends; the service's thread in miniature. */
static int run(unsigned budget)
{
    for (unsigned i = 0; i < budget; ++i) {
        if (!ssr_core_step(&core)) return 0;
        if (session_ended) return 1;
    }
    return 1;
}

static void default_framing(void)
{
    start(0, 0, INIT_NEEDS_PAGE);
    CHECK(run(100) && dsp.next2s == 1);
    CHECK(dsp.attaches == 1 && dsp.opens == 1 && dsp.registers == 1 && dsp.closes == 1 && dsp.inits == 1 && dsp.releases == 0);
    CHECK(dsp.order[0] == (1u << 8 | 0) && dsp.order[1] == 0 && dsp.order[2] == (0x77u << 8) && dsp.order[3] == 1 && dsp.order[4] == (3u << 8 | 3));
    CHECK(dsp.init_page[0] && dsp.init_tid[0] == CLIENT && dsp.init_ctx_low[0] == SSR_PD_SENSORS);   /* id << 8: bits 4..7 clear */
    CHECK(core.status.frame == SSR_FRAME_META && core.status.inits == 1 && core.status.stage == SsrStListen);
    CHECK((dsp.next2_ctx & 0xff) == SSR_PD_SENSORS && dsp.next2_ctx >> 8 && dsp.next2_tid == CLIENT);
    CHECK(logged("attached to the sensors PD") && logged("frame=meta deadline=15000") && logged("listening frame=meta inits=1"));
    CHECK(!strcmp(ssr_frame_name(SSR_FRAME_DEFAULT), "meta") && !strcmp(ssr_frame_name(0), "pag") &&
          !strcmp(ssr_frame_name(7), "meta+ctx4+tid"));
    printf("PASS default framing: register and init2 carry Linux's metadata page (384 zero bytes over junk, bank 3), ctx id << 8, tid "
           "the client id; init2 answered at once, next2 follows\n");
}

static void tablet_failure(void)
{
    /* PaganiSensors' framing, no ladder: the tablet's run, and now the session is released at once */
    start(SSR_FRAME_PAG, 1, INIT_NEEDS_PAGE);
    CHECK(!run(100));
    CHECK(dsp.inits == 1 && !dsp.init_page[0] && dsp.next2s == 0);
    CHECK(logged("FAIL st=-6 err=-2147482610 adsp_listener init2 failed"));             /* the tablet's line, byte for byte */
    CHECK(dsp.releases == 1 && dsp.release_at - dsp.attach_at < 1000 && core.status.release == SSR_RELEASE_DONE);
    CHECK(logged("released the sensors PD's session rc=0 value=0") && core.status.stage == -SsrStListen);
    CHECK(dsp.order[dsp.messages - 1] == (1u << 8 | 1));                                 /* the release is the last message */
    CHECK(!ssr_core_step(&core) && dsp.messages == 6);                                   /* stopped: nothing more */

    /* PaganiSensors' framing with the ladder (piano-sensors-rpc-frame=0): the control, then Linux's page in the same session */
    start(SSR_FRAME_PAG, 0, INIT_NEEDS_PAGE);
    CHECK(run(100) && dsp.next2s == 1 && dsp.inits == 2 && !dsp.init_page[0] && dsp.init_page[1] && dsp.releases == 0);
    CHECK(logged("init2 frame=pag answered rc=0 value=-2147482610: next frame=meta") && logged("listening frame=meta inits=2"));
    CHECK(core.status.frame == SSR_FRAME_META);
    printf("PASS the tablet's failure: PaganiSensors' page-less init2 answered EBADPARM, the session released at once (the last "
           "message); with the ladder the same session listens with Linux's page\n");
}

static void ladder(void)
{
    /* a DSP that also wants a thread id of its own: meta, meta+ctx4, meta+ctx4+tid; the framing stays for next2 */
    start(0, 0, INIT_NEEDS_TID);
    CHECK(run(100) && dsp.next2s == 1 && dsp.inits == 3 && dsp.releases == 0);
    CHECK(dsp.init_page[0] && dsp.init_page[1] && dsp.init_page[2]);
    CHECK(dsp.init_tid[0] == CLIENT && dsp.init_tid[1] == CLIENT && dsp.init_tid[2] == CLIENT + SSR_TID_OFFSET);
    CHECK(core.status.frame == (SSR_FRAME_META | SSR_FRAME_CTX4 | SSR_FRAME_TID));
    /* the context numbers 1..8 in order (attach, open, register, close, three init2, next2): id << 8 first, id << 4 from ctx4 on */
    CHECK(dsp.init_ctx_low[0] == 0x02 && dsp.init_ctx_low[1] == 0x62 && dsp.init_ctx_low[2] == 0x72);
    CHECK(dsp.next2_tid == CLIENT + SSR_TID_OFFSET && dsp.next2_ctx == 0x82);
    CHECK(logged("next frame=meta+ctx4") && logged("next frame=meta+ctx4+tid") && logged("listening frame=meta+ctx4+tid inits=3"));

    /* the ladder exhausted: released, no next2, stopped */
    start(0, 0, INIT_NEVER);
    CHECK(!run(100) && dsp.inits == 3 && dsp.next2s == 0 && dsp.releases == 1 && core.status.release == SSR_RELEASE_DONE);
    CHECK(dsp.release_at - dsp.attach_at < 1000 && logged("FAIL st=-6 err=-2147482610 adsp_listener init2 failed"));

    /* no ladder: one init2 only */
    start(0, 1, INIT_NEVER);
    CHECK(!run(100) && dsp.inits == 1 && dsp.releases == 1);
    printf("PASS ladder: an init2 answered with an error goes again with + ctx4, then + tid; the framing that is answered 0 serves "
           "next2; exhausted: released at once, nothing more\n");
}

static void withheld_and_deadline(void)
{
    /* the glue says the DSP may not be told (a fatal sign): no release, said */
    start(0, 0, INIT_NEVER);
    dsp.tell = 0;
    CHECK(!run(100) && dsp.releases == 0 && core.status.release == SSR_RELEASE_WITHHELD && logged("release withheld"));

    /* an open never answered: 5 s per try, 1 s apart; the deadline (15 s after the attach) releases the session */
    start(0, 0, INIT_ALWAYS);
    dsp.silent_open = 1;
    CHECK(!run(1000));
    CHECK(dsp.opens == 3 && dsp.releases == 1 && dsp.inits == 0 && core.status.release == SSR_RELEASE_DONE);
    CHECK(dsp.release_at - dsp.attach_at >= SSR_DEADLINE_MS && dsp.release_at - dsp.attach_at < SSR_DEADLINE_MS + 7000);   /* < 59 s */
    CHECK(logged("the listener was not up within the deadline") && core.status.stage == -SsrStRegister);

    /* the glue's abort while the bring-up is held (the guard struck, or quiet ran past the deadline) */
    start(0, 0, INIT_ALWAYS);
    dsp.silent_open = 1;
    for (unsigned i = 0; i < 50 && !dsp.opens; ++i) CHECK(ssr_core_step(&core));
    CHECK(ssr_core_bringing_up(&core) && !ssr_core_overdue(&core));
    clock_ms += SSR_DEADLINE_MS;
    CHECK(ssr_core_overdue(&core));
    ssr_core_abort(&core, "the guard is off before the listening");
    CHECK(dsp.releases == 1 && !ssr_core_bringing_up(&core) && !ssr_core_step(&core));
    ssr_core_abort(&core, "twice");                                                       /* stopped: nothing */
    CHECK(dsp.releases == 1 && logged("the guard is off before the listening"));

    /* before the attach nothing is released; after a link loss the DSP's side is gone: nothing either */
    start(0, 0, INIT_ALWAYS);
    ssr_core_abort(&core, "before the attach");
    CHECK(dsp.messages == 0 && core.status.release == SSR_RELEASE_NONE);
    start(0, 0, INIT_ALWAYS);
    dsp.silent_open = 1;
    for (unsigned i = 0; i < 50 && !dsp.opens; ++i) CHECK(ssr_core_step(&core));
    ssr_core_channel(&core, 0);
    for (unsigned i = 0; i < 20; ++i) CHECK(ssr_core_step(&core));
    CHECK(!ssr_core_bringing_up(&core) && dsp.releases == 0 && logged("channel went away"));
    printf("PASS release: withheld from a DSP that may not be told; the deadline (an open never answered) releases inside 22 s of the "
           "attach; the glue's abort; nothing before an attach or after a link loss\n");
}

/* The DSP's calls at odd offsets, starting with s1b's first: remotectl open "apps_std" (handle 0, sc 0x20200, a 0x29-byte blob with the
 * out-buffer lengths at 0x21 and 0x25). Everything the service reads or writes in the arena goes through SensorsArena.c (checked); the
 * answers come back exactly as asked; then 400 fuzzed calls of odd shapes. */
static uint32_t lengths[16];
static void open_std(uint8_t *const out[4]) {CHECK(rd32(out[0]) == 1 && rd32(out[0] + 4) == 0); for (int i = 0; i < 256; ++i) CHECK(!out[1][i]);}
static void open_mem(uint8_t *const out[4]) {CHECK(rd32(out[0]) == 2 && rd32(out[0] + 4) == 0);}
static void getenv_path(uint8_t *const out[4])
{
    const uint32_t n = (uint32_t)strlen(SSR_LIBRARY_PATH);
    CHECK(rd32(out[0]) == n + 1 && n + 1 <= 97 && !memcmp(out[1], SSR_LIBRARY_PATH, n) && out[1][n] == 0);
}
static void exists_no(uint8_t *const out[4]) {CHECK(out[0][0] == 0);}

static void odd_offsets(void)
{
    static uint8_t p_std[8], p_mem[8], p_msgd[8], p_fopen[8], p_getenv[8], p_print[8], p_exists[8], p_stat[8], p_close[8];
    static const char std[] = "apps_std", mem[] = "apps_mem", msgd[] = "adspmsgd_apps", path[] = "/vendor/etc/sensors/x.json", mode[] = "r",
                      env[] = "ADSP_LIBRARY_PATH", text[] = "hello, piano", odm[] = "/odm/etc/sensors/a.json", persist[] = "/persist/x";
    wr32(p_std, sizeof std); wr32(p_std + 4, 256);
    wr32(p_mem, sizeof mem); wr32(p_mem + 4, 256);
    wr32(p_msgd, sizeof msgd); wr32(p_msgd + 4, 255);
    wr32(p_fopen, sizeof path); wr32(p_fopen + 4, sizeof mode);
    wr32(p_getenv, sizeof env); wr32(p_getenv + 4, 97);
    wr32(p_print, sizeof text); wr32(p_print + 4, 0);
    wr32(p_exists, sizeof odm); wr32(p_exists + 4, 1);
    wr32(p_stat, 31); wr32(p_stat + 4, sizeof persist);
    wr32(p_close, 1); wr32(p_close + 4, 0);
    const DspCall calls[] = {
        {0, 0, 2, 2, {p_std, (const uint8_t *)std}, {8, sizeof std}, {8, 256}, 0, open_std},          /* s1b's first call */
        {0, 0, 2, 2, {p_mem, (const uint8_t *)mem}, {8, sizeof mem}, {8, 256}, 0, open_mem},
        {0, 0, 2, 2, {p_msgd, (const uint8_t *)msgd}, {8, sizeof msgd}, {8, 255}, -5, NULL},         /* not here: -5, nothing back */
        {1, 0, 3, 1, {p_fopen, (const uint8_t *)path, (const uint8_t *)mode}, {8, sizeof path, sizeof mode}, {4}, SSR_EFAILED, NULL},
        {1, 16, 2, 2, {p_getenv, (const uint8_t *)env}, {8, sizeof env}, {4, 97}, 0, getenv_path},
        {1, 15, 2, 0, {p_print, (const uint8_t *)text}, {8, sizeof text}, {0}, 0, NULL},
        {1, 22, 2, 1, {p_exists, (const uint8_t *)odm}, {8, sizeof odm}, {1}, 0, exists_no},           /* a 1-byte answer */
        {1, 31, 2, 1, {p_stat, (const uint8_t *)persist}, {8, sizeof persist}, {96}, SSR_EFAILED, NULL},
        {0, 1, 1, 2, {p_close}, {8}, {4, 0}, 0, NULL},
    };
    start(0, 0, INIT_ALWAYS);
    script = calls;
    script_count = sizeof calls / sizeof calls[0];
    const unsigned long before = arena_accesses;
    CHECK(run(200) && dsp.next2s == script_count + 1 && answered == script_count);
    CHECK(core.status.calls == script_count && core.status.missing == 3);
    CHECK(logged("first call of the DSP handle=0 sc=0x20200") && logged("[sensors] dsp: hello, piano") &&
          logged("missing /vendor/etc/sensors/x.json") && logged("adspmsgd_apps: no such interface here"));
    /* s1b's blob, byte for byte: the out lengths at 0x21 and 0x25, 0x29 in all (the service read them through SensorsArena.c) */
    DspCall first = calls[0];
    uint8_t blob[64];
    CHECK(pack_call(blob, sizeof blob, &first) == 0x29 && rd32(blob + 0x21) == 8 && rd32(blob + 0x25) == 256);
    CHECK(arena_accesses > before && arena_bytes[1] && arena_bytes[4] && arena_bytes[8]);
    (void)lengths;

    /* fuzzed calls of odd shapes: every one answered, every answer exactly as asked when it succeeds, no access outside the accessors */
    start(0, 0, INIT_ALWAYS);
    fuzz_left = 400;
    CHECK(run(2000) && answered == 400 && dsp.next2s == 401);
    printf("PASS odd offsets: s1b's first call (remotectl open \"apps_std\", out lengths at 0x21) and eight more of odd lengths, then "
           "400 fuzzed calls; the arena unreadable outside SensorsArena.c, all %lu of its accesses aligned (%lu bytes, %lu words, %lu "
           "doublewords)\n", arena_accesses, arena_bytes[1], arena_bytes[4], arena_bytes[8]);
}

/* The checks themselves catch what they are for: a direct read of the arena faults, an unaligned word is refused. */
static int child_fails(void (*body)(void))
{
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        FILE *quiet = freopen("/dev/null", "w", stderr);
        (void)quiet;
        body();
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    return !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void raw_read(void) {const volatile uint8_t *p = arena + 0x1021; (void)*p;}
static void unaligned_word(void) {arena_open(); sar_access(arena + 0x1021, 4);}
static void aligned_word(void) {arena_open(); sar_access(arena + 0x1020, 4); sar_access(arena + 0x1021, 1); arena_close();}
static void controls(void)
{
    CHECK(child_fails(raw_read) && child_fails(unaligned_word) && !child_fails(aligned_word));
    printf("PASS controls: a direct read of the arena faults, an unaligned word is refused, aligned words and bytes pass\n");
}

static void guard_strike(void)
{
    SgGuard g;
    sg_init(&g, 0);
    sg_strike(&g, SG_REASON_FILES);
    CHECK(g.state == SG_OFF && g.reason == SG_REASON_FILES && !sg_serve_files(&g) && !sg_may_request(&g, 1));
    CHECK(!strcmp(sg_reason_name(SG_REASON_FILES), "the file service failed after the attach"));
    sg_init(&g, 0);
    const SgDsp fatal = {1, 1, 0, SG_CHARGER_UP, 1, 0, 0};
    CHECK(sg_observe(&g, &fatal, 1) == SG_OFF && g.reason == SG_REASON_FATAL);
    sg_strike(&g, SG_REASON_FILES);
    CHECK(g.reason == SG_REASON_FATAL);                                                  /* the first reason stays */
    printf("PASS guard: the file service's strike, off for the boot; an earlier reason kept\n");
}

int main(void)
{
    void *map = mmap(NULL, SSR_ARENA_BYTES, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(map != MAP_FAILED);
    arena = map;
    shadow = malloc(SSR_SHADOW_BYTES);
    CHECK(shadow);
    controls();
    default_framing();
    tablet_failure();
    ladder();
    withheld_and_deadline();
    odd_offsets();
    guard_strike();
    CHECK(arena_depth == 0);
    munmap(map, SSR_ARENA_BYTES);
    free(shadow);
    printf("PASS PianoSensors bring-up tests: %u checks\n", checks);
    return 0;
}
