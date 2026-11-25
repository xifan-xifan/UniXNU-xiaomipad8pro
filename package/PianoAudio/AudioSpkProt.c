/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Speaker protection on the audio DSP: see AudioSpkProt.h. */
#include "AudioSpkProt.h"
#include "AudioSession.h"

#define PARAM_HEADER 16u
#define PARAM_SG_CONFIG 0x08001001u
#define PARAM_CONTAINER_CONFIG 0x08001000u
#define PARAM_MODULE_LIST 0x08001002u
#define PARAM_MODULE_PROP 0x08001003u
#define PARAM_MODULE_CONN 0x08001004u
#define PARAM_CTRL_LINK 0x08001061u
#define PROP_SG_PERF 0x0800100Eu
#define PROP_SG_DIRECTION 0x0800100Fu
#define PROP_SG_SCENARIO 0x08001010u
#define PROP_CONT_CAPS 0x08001011u
#define PROP_CONT_POS 0x08001012u
#define PROP_CONT_STACK 0x08001013u
#define PROP_CONT_DOMAIN 0x08001014u
#define PROP_CONT_HEAP 0x08001174u
#define PROP_CONT_PARENT 0x080010CBu
#define PROP_CONT_PEER_HEAP 0x0800124Du
#define PROP_PORT_INFO 0x08001015u
#define PROP_LINK_INTENTS 0x08001062u
#define PROP_LINK_0x136F 0x0800136Fu    /* unnamed in the public API; 1 in the stock link */
#define LINK_INTENT_FSADSP 0x08001231u  /* unnamed in the public API; the stock link's one intent */
#define PARAM_HW_EP_MF 0x08001017u
#define PARAM_FRAME_SIZE_FACTOR 0x08001018u
#define PARAM_TDM_INTF 0x0800101Bu
#define PARAM_MFC_OUTPUT 0x08001024u
#define PARAM_TDM_LANE 0x0800106Au
#define PARAM_SRC_CHANNEL_MAP 0x0800108Au
#define CONTAINER_GC 0x0B001001u
#define CONTAINER_SC 0x0B001000u        /* MIPLAY's container (the stock ACDB's capability) */
#define PERF_LOW_POWER 1u
#define PERF_LOW_LATENCY 2u
#define DIR_TX 1u
#define DIR_RX 2u
#define SCENARIO_ANY 0xffffffffu
#define POS_PER_STR_DEV 2u
#define POS_GLOBAL_DEV 4u
#define STACK 4096u
#define STACK_MIP 40960u
#define DOMAIN_ADSP 2u
#define CTRL_PORT 0x80000000u
#define CH_VI_FIRST 48u                 /* the V/I slots' channel types (the ACDB's source channel map: 48, 49, 50, 51) */

typedef struct {uint8_t *p; uint32_t n, cap; int bad;} W;
static void put8(W *w, uint32_t v) {if (w->n + 1 > w->cap) {w->bad = 1; return;} w->p[w->n++] = (uint8_t)v;}
static void put16(W *w, uint32_t v) {put8(w, v); put8(w, v >> 8);}
static void put32(W *w, uint32_t v) {put16(w, v); put16(w, v >> 16);}
static void pad_to(W *w, uint32_t end) {while (!w->bad && w->n < end) put8(w, 0);}
static uint32_t align8(uint32_t v) {return (v + 7u) & ~7u;}
static uint32_t align32(uint32_t v) {return (v + 31u) & ~31u;}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void set32(W *w, uint32_t at, uint32_t v)
{
    if (at + 4 <= w->n) {w->p[at] = (uint8_t)v; w->p[at + 1] = (uint8_t)(v >> 8); w->p[at + 2] = (uint8_t)(v >> 16); w->p[at + 3] = (uint8_t)(v >> 24);}
}
static void mem_zero(void *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) ((uint8_t *)p)[i] = 0;}

/* A buffer (an out-of-band payload) or a GPR packet (capped at GPR_MAX_PACKET; size patched in at the end). */
static void buf_begin(W *w, uint8_t *buf, uint32_t cap) {w->p = buf; w->n = 0; w->cap = cap; w->bad = 0;}
static void pkt_begin(W *w, uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, uint32_t opcode)
{
    buf_begin(w, out, cap < GPR_MAX_PACKET ? cap : GPR_MAX_PACKET);
    put32(w, 0); put32(w, GPR_DOMAIN_ADSP | GPR_DOMAIN_APPS << 8); put32(w, port); put32(w, AR_APM_IID); put32(w, token); put32(w, opcode);
}
static uint32_t pkt_finish(W *w)
{
    if (w->bad) return 0;
    set32(w, 0, (GPR_VERSION & 0xfu) | (GPR_HEADER_BYTES / 4) << 4 | w->n << 8);
    return w->n;
}
/* In-band APM command header; its payload size patched at the end. */
static uint32_t cmd_begin(W *w) {const uint32_t at = w->n; put32(w, 0); put32(w, 0); put32(w, 0); put32(w, 0); return at;}
static void cmd_end(W *w, uint32_t at) {set32(w, at + 12, w->n - at - 16u);}
/* One parameter block as the stock graph services lay them out: the header with the data's exact size, the data, zeros to 8. */
static uint32_t blk_begin(W *w, uint32_t iid, uint32_t pid) {const uint32_t at = w->n; put32(w, iid); put32(w, pid); put32(w, 0); put32(w, 0); return at;}
static void blk_end(W *w, uint32_t at)
{
    set32(w, at + 8, w->n - at - PARAM_HEADER);
    pad_to(w, at + align8(w->n - at));
}
static void prop(W *w, uint32_t id, uint32_t v) {put32(w, id); put32(w, 4); put32(w, v);}

/* ------------------------------------------------------------------------------------------------------- the private files */
typedef struct {uint32_t iid, pid, size; uint8_t want[16];} Expect;
/* The driver's own values (spk_endpoint_config): a file that carries these parameters must carry exactly them. */
static const Expect kExpect[] = {
    {SPK_IID_VI_TDM, PARAM_HW_EP_MF, 12, {0x80, 0xbb, 0, 0, 32, 0, 4, 0, 1, 0, 0, 0}},
    {SPK_IID_TDM, PARAM_HW_EP_MF, 12, {0x80, 0xbb, 0, 0, 32, 0, 4, 0, 1, 0, 0, 0}},
    {SPK_IID_VI_TDM, PARAM_TDM_LANE, 4, {SPK_LANE_VI, 0, 0, 0}},
    {SPK_IID_TDM, PARAM_TDM_LANE, 4, {SPK_LANE_RX, 0, 0, 0}},
    {SPK_IID_VI_TDM, PARAM_SRC_CHANNEL_MAP, 10, {4, 0, CH_VI_FIRST, 0, CH_VI_FIRST + 1, 0, CH_VI_FIRST + 2, 0, CH_VI_FIRST + 3, 0}},
    {SPK_IID_MFC, PARAM_MFC_OUTPUT, 16, {0x80, 0xbb, 0, 0, 32, 0, 4, 0, AR_CH_FL, 0, AR_CH_FR, 0, AR_CH_LS, 0, AR_CH_RS, 0}},
};

int spk_iid_graph(uint32_t iid)
{
    switch (iid) {
    case SPK_IID_VI_TDM: case SPK_IID_VI_LOG: case SPK_IID_VI_SPLIT: case SPK_IID_SP_TX: return 1;
    case SPK_IID_MFC: case SPK_IID_SP_RX: case SPK_IID_SPLIT: case SPK_IID_LOG: case SPK_IID_TDM: return 2;
    default: return 0;
    }
}

static int fail(const char **why, const char *text) {if (why) *why = text; return -1;}

int spk_params_parse(SpkProt *p, const uint8_t *file, uint32_t bytes, const char **why)
{
    p->have_params = 0; p->count = 0; p->bytes = 0;
    if (!file || bytes < 16) return fail(why, "spk-prot.bin: missing or shorter than its header");
    if (le32(file) != SPK_PARAMS_MAGIC) return fail(why, "spk-prot.bin: not a PSPK file");
    if (le32(file + 4) != SPK_PARAMS_VERSION) return fail(why, "spk-prot.bin: another version");
    const uint32_t count = le32(file + 8);
    if (!count || count > SPK_MAX_ENTRIES || le32(file + 12)) return fail(why, "spk-prot.bin: bad entry count");
    uint32_t at = 16;
    int rx = 0, tx = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes - at < 12) return fail(why, "spk-prot.bin: truncated entry");
        SpkParam *e = &p->e[i];
        e->iid = le32(file + at); e->pid = le32(file + at + 4); e->size = le32(file + at + 8);
        at += 12;
        if (!e->size || e->size > SPK_MAX_PARAM || bytes - at < ((e->size + 3u) & ~3u)) return fail(why, "spk-prot.bin: bad parameter size");
        e->data = file + at;
        at += (e->size + 3u) & ~3u;
        if (!spk_iid_graph(e->iid)) return fail(why, "spk-prot.bin: a module that is not in the protection graphs");
        if (e->pid == PARAM_TDM_INTF) return fail(why, "spk-prot.bin: carries a TDM interface (the driver sends its own)");
        for (uint32_t k = 0; k < i; ++k)
            if (p->e[k].iid == e->iid && p->e[k].pid == e->pid) return fail(why, "spk-prot.bin: a parameter twice");
        for (unsigned k = 0; k < sizeof kExpect / sizeof kExpect[0]; ++k) {
            const Expect *x = &kExpect[k];
            if (x->iid != e->iid || x->pid != e->pid) continue;
            if (e->size != x->size) return fail(why, "spk-prot.bin: an endpoint/MFC parameter of another size than the driver's");
            for (uint32_t b = 0; b < x->size; ++b)
                if (e->data[b] != x->want[b]) return fail(why, "spk-prot.bin: an endpoint/MFC format or lane that is not the driver's");
        }
        rx |= e->iid == SPK_IID_SP_RX;
        tx |= e->iid == SPK_IID_SP_TX;
        p->bytes += e->size;
    }
    if (at != bytes) return fail(why, "spk-prot.bin: bytes after the last entry");
    if (!rx || !tx) return fail(why, "spk-prot.bin: no SP_RX or no SP_TX parameter (not the protection graphs' calibration)");
    p->count = count;
    p->have_params = 1;
    return 0;
}

int spk_re25_parse(SpkProt *p, const uint8_t *file, uint32_t bytes, const char **why)
{
    p->have_re25 = 0;
    if (!file || bytes != SPK_RE25_FILE_BYTES) return fail(why, "spk-re25.bin: missing or not 16 bytes");
    for (unsigned c = 0; c < 4; ++c) {
        const uint32_t r = le32(file + 4 * c);
        if (r < SPK_RE25_MIN || r > SPK_RE25_MAX) return fail(why, "spk-re25.bin: a channel's Re25 outside 2..16 ohm");
        p->re25[c] = r;
    }
    p->have_re25 = 1;
    return 0;
}

int spk_mip_iid(uint32_t iid)
{
    switch (iid) {
    case SPK_IID_MIP_SYNC: case SPK_IID_MIP_SAL: case SPK_IID_MIP_LOG_IN: case SPK_IID_MIP_MFC_IN: case SPK_IID_MIP_MI_A: case SPK_IID_DAP:
    case SPK_IID_MIP_MI_B: case SPK_IID_MIP_ROT: case SPK_IID_MIP_FX: case SPK_IID_MIP_LOG_OUT: case SPK_IID_MIP_VOL: case SPK_IID_MIP_MFC_OUT:
        return 1;
    default:
        return 0;
    }
}

int spk_mip_parse(SpkMip *m, const uint8_t *file, uint32_t bytes, const char **why)
{
    m->have = 0; m->count = m->dap = 0; m->bytes = 0; m->preg_pid = 0;
    if (!file || bytes < 16) return fail(why, "spk-dolby.bin: missing or shorter than its header");
    if (le32(file) != SPK_MIP_MAGIC) return fail(why, "spk-dolby.bin: not a PMIP file");
    if (le32(file + 4) != SPK_MIP_VERSION) return fail(why, "spk-dolby.bin: another version");
    const uint32_t count = le32(file + 8), preg_pid = le32(file + 12);
    if (!count || count > SPK_MIP_MAX_ENTRIES) return fail(why, "spk-dolby.bin: bad entry count");
    uint32_t at = 16;
    int preg = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes - at < 12) return fail(why, "spk-dolby.bin: truncated entry");
        SpkParam *e = &m->e[i];
        e->iid = le32(file + at); e->pid = le32(file + at + 4); e->size = le32(file + at + 8);
        at += 12;
        if (!e->size || e->size > SPK_MAX_PARAM || bytes - at < ((e->size + 3u) & ~3u)) return fail(why, "spk-dolby.bin: bad parameter size");
        e->data = file + at;
        at += (e->size + 3u) & ~3u;
        if (!spk_mip_iid(e->iid)) return fail(why, "spk-dolby.bin: a module that is not in the MIPLAY graph");
        for (uint32_t k = 0; k < i; ++k)
            if (m->e[k].iid == e->iid && m->e[k].pid == e->pid) return fail(why, "spk-dolby.bin: a parameter twice");
        m->dap += e->iid == SPK_IID_DAP;
        preg |= preg_pid && e->iid == SPK_IID_DAP && e->pid == preg_pid && e->size == 4;
        m->bytes += e->size;
    }
    if (at != bytes) return fail(why, "spk-dolby.bin: bytes after the last entry");
    if (!m->dap) return fail(why, "spk-dolby.bin: no parameter of the Dolby DAP 0x411f (not MIPLAY's calibration)");
    if (preg_pid && !preg) return fail(why, "spk-dolby.bin: its pregain parameter is not one of the DAP's 4-byte parameters");
    m->count = count;
    m->preg_pid = preg_pid;
    m->have = 1;
    return 0;
}

int spk_miplay_valid(int request)
{
    if (request == 0) return 1;
    if (request < 0 || request >= 128) return 0;
    const uint32_t r = (uint32_t)request;
    const uint32_t perf = r & SPK_MIP_PERF_MASK;
    return (perf == 1u || perf == 2u) && !(r & 8u) && (r >> SPK_MIP_ROT_SHIFT) <= 4u;
}
unsigned spk_miplay_perf(uint32_t request) {return request & SPK_MIP_PERF_MASK;}
unsigned spk_miplay_rotation(uint32_t request)
{
    const uint32_t k = request >> SPK_MIP_ROT_SHIFT;
    return k ? (unsigned)(k - 1u) & 3u : SPK_MIP_ROTATION_DEFAULT;
}

static unsigned put_text(char *buf, unsigned n, unsigned cap, const char *t) {for (; *t && n + 1 < cap; ++t) buf[n++] = *t; return n;}
static unsigned put_number(char *buf, unsigned n, unsigned cap, int32_t v)
{
    char d[12];
    unsigned k = 0;
    uint32_t u = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    do {d[k++] = (char)('0' + u % 10u); u /= 10u;} while (u && k < sizeof d);
    if (v < 0 && n + 1 < cap) buf[n++] = '-';
    while (k && n + 1 < cap) buf[n++] = d[--k];
    return n;
}
const char *spk_miplay_text(uint32_t request, char *buf, unsigned cap)
{
    if (!buf || !cap) return "";
    unsigned n = 0;
    if (!request) {
        n = put_text(buf, n, cap, "off");
    } else {
        n = put_text(buf, n, cap, "perf ");
        n = put_number(buf, n, cap, (int32_t)spk_miplay_perf(request));
        n = put_text(buf, n, cap, (request & SPK_MIP_PREG_TRACKED) ? ", pregain following the master volume" : ", pregain fixed 0 dB");
        n = put_text(buf, n, cap, ", rotation ");
        n = put_number(buf, n, cap, (int32_t)spk_miplay_rotation(request));
        if (!(request >> SPK_MIP_ROT_SHIFT)) n = put_text(buf, n, cap, " (Android's)");
    }
    buf[n] = 0;
    return buf;
}

/* log2(x) in Q16 for 1 <= x < 2^15 (the integer part by the top bit, the fraction by repeated squaring) */
static int32_t log2_q16(uint32_t x)
{
    int32_t n = 0;
    while ((x >> (n + 1)) && n < 31) ++n;
    uint64_t m = ((uint64_t)x << 16) >> n;   /* [1, 2) in Q16 */
    int32_t frac = 0;
    for (int i = 15; i >= 0; --i) {
        m = (m * m) >> 16;
        if (m >= (2u << 16)) {m >>= 1; frac |= 1 << i;}
    }
    return n * 65536 + frac;
}
int32_t spk_preg_q4(int32_t q15)
{
    if (q15 >= 32767) return 0;
    if (q15 <= 0) return SPK_PREG_MUTE;
    /* 20 log10(q15 / 32768) x 16 = (log2 q15 - 15) x 320 / log2 10; 320 / log2 10 = 96.3296 = 6313029 / 65536 */
    const int64_t v = (int64_t)(log2_q16((uint32_t)q15) - 15 * 65536) * 6313029;   /* Q32 */
    const int32_t r = (int32_t)((v + ((int64_t)1 << 31)) >> 32);
    return r < SPK_PREG_MUTE ? SPK_PREG_MUTE : r > 0 ? 0 : r;
}

/* ------------------------------------------------------------------------------------------------------------ the graphs */
typedef struct {uint32_t id, caps, position, stack, peer_heap;} Cont;
typedef struct {uint32_t module, iid, container, inputs, outputs;} Mod;
typedef struct {uint32_t from, from_port, to, to_port;} Link;
typedef struct {uint32_t sg, direction; const Cont *c; unsigned containers; const Mod *m; unsigned modules; const Link *l; unsigned links;
                int ctrl;} Graph;

static const Cont kViCont[] = {{SPK_CONT_VI, CONTAINER_GC, POS_GLOBAL_DEV, STACK, 1}};
static const Mod kViMods[] = {
    {AR_MOD_LOG, SPK_IID_VI_LOG, SPK_CONT_VI, 1, 1}, {SPK_MOD_SPLITTER, SPK_IID_VI_SPLIT, SPK_CONT_VI, 1, 2},
    {SPK_MOD_TDM_SOURCE, SPK_IID_VI_TDM, SPK_CONT_VI, 0, 1}, {SPK_MOD_SP_TX, SPK_IID_SP_TX, SPK_CONT_VI, 1, 1},
};
static const Link kViLinks[] = {{SPK_IID_VI_TDM, 1, SPK_IID_VI_LOG, 2}, {SPK_IID_VI_LOG, 1, SPK_IID_VI_SPLIT, 2}, {SPK_IID_VI_SPLIT, 1, SPK_IID_SP_TX, 2}};
static const Cont kDevCont[] = {{SPK_CONT_MFC, CONTAINER_GC, POS_GLOBAL_DEV, STACK, 2}, {SPK_CONT_RX, CONTAINER_GC, POS_GLOBAL_DEV, STACK, 2},
                                {SPK_CONT_TAIL, CONTAINER_GC, POS_GLOBAL_DEV, STACK, 2}};
static const Mod kDevMods[] = {
    {AR_MOD_MFC, SPK_IID_MFC, SPK_CONT_MFC, 1, 1}, {SPK_MOD_SP_RX, SPK_IID_SP_RX, SPK_CONT_RX, 1, 1},
    {SPK_MOD_SPLITTER, SPK_IID_SPLIT, SPK_CONT_TAIL, 1, 24}, {AR_MOD_LOG, SPK_IID_LOG, SPK_CONT_TAIL, 1, 1},
    {AR_MOD_TDM_SINK, SPK_IID_TDM, SPK_CONT_TAIL, 1, 0},
};
static const Link kDevLinks[] = {{SPK_IID_MFC, 1, SPK_IID_SP_RX, 2}, {SPK_IID_SP_RX, 1, SPK_IID_SPLIT, 2}, {SPK_IID_SPLIT, 1, SPK_IID_LOG, 2},
                                 {SPK_IID_LOG, 1, SPK_IID_TDM, 2}};
/* MIPLAY 0xb000019f as the stock ACDB holds it (modules, ports and links in its order), plus the link into our speaker subgraph */
static const Cont kMipCont[] = {{SPK_CONT_MIP, CONTAINER_SC, POS_PER_STR_DEV, STACK_MIP, 2}};
static const Mod kMipMods[] = {
    {AR_MOD_LOG, SPK_IID_MIP_LOG_IN, SPK_CONT_MIP, 1, 1}, {AR_MOD_MFC, SPK_IID_MIP_MFC_IN, SPK_CONT_MIP, 1, 1},
    {AR_MOD_LOG, SPK_IID_MIP_LOG_OUT, SPK_CONT_MIP, 1, 1}, {SPK_MOD_SAL, SPK_IID_MIP_SAL, SPK_CONT_MIP, 4, 1},
    {SPK_MOD_SYNC, SPK_IID_MIP_SYNC, SPK_CONT_MIP, 3, 4}, {AR_MOD_MFC, SPK_IID_MIP_MFC_OUT, SPK_CONT_MIP, 1, 1},
    {SPK_MOD_VOL, SPK_IID_MIP_VOL, SPK_CONT_MIP, 1, 1}, {SPK_MOD_MI_FX, SPK_IID_MIP_FX, SPK_CONT_MIP, 1, 1},
    {SPK_MOD_DAP, SPK_IID_DAP, SPK_CONT_MIP, 1, 1}, {SPK_MOD_MI_ROT, SPK_IID_MIP_ROT, SPK_CONT_MIP, 1, 1},
    {SPK_MOD_MI_B, SPK_IID_MIP_MI_B, SPK_CONT_MIP, 1, 1}, {SPK_MOD_MI_A, SPK_IID_MIP_MI_A, SPK_CONT_MIP, 1, 1},
};
static const Link kMipLinks[] = {
    {SPK_IID_MIP_LOG_IN, 1, SPK_IID_MIP_MFC_IN, 2}, {SPK_IID_MIP_SYNC, 5, SPK_IID_MIP_SAL, 2}, {SPK_IID_MIP_SYNC, 7, SPK_IID_MIP_SAL, 4},
    {SPK_IID_MIP_SYNC, 9, SPK_IID_MIP_SAL, 6}, {SPK_IID_MIP_SAL, 1, SPK_IID_MIP_LOG_IN, 2}, {SPK_IID_MIP_SYNC, 0xb, SPK_IID_MIP_SAL, 8},
    {SPK_IID_MIP_LOG_OUT, 1, SPK_IID_MIP_VOL, 2}, {SPK_IID_MIP_VOL, 1, SPK_IID_MIP_MFC_OUT, 2}, {SPK_IID_MIP_FX, 1, SPK_IID_MIP_LOG_OUT, 2},
    {SPK_IID_MIP_ROT, 1, SPK_IID_MIP_FX, 2}, {SPK_IID_DAP, 1, SPK_IID_MIP_MI_B, 2}, {SPK_IID_MIP_MI_B, 1, SPK_IID_MIP_ROT, 2},
    {SPK_IID_MIP_MFC_IN, 1, SPK_IID_MIP_MI_A, 2}, {SPK_IID_MIP_MI_A, 1, SPK_IID_DAP, 2},
    {SPK_IID_MIP_MFC_OUT, 1, SPK_IID_MFC, 2},   /* the deep-buffer graph's 4377:1 -> SAL 4012:0x24, here to our MFC */
};
#define N(a) (sizeof a / sizeof a[0])
static const Graph kVi = {SPK_SG_VI, DIR_TX, kViCont, N(kViCont), kViMods, N(kViMods), kViLinks, N(kViLinks), 1};
static const Graph kDev = {SPK_SG_DEV, DIR_RX, kDevCont, N(kDevCont), kDevMods, N(kDevMods), kDevLinks, N(kDevLinks), 1};
static const Graph kMip = {SPK_SG_MIP, DIR_RX, kMipCont, N(kMipCont), kMipMods, N(kMipMods), kMipLinks, N(kMipLinks), 0};

static uint32_t open_payload(uint8_t *buf, uint32_t cap, const Graph *g, uint32_t perf)
{
    W w;
    buf_begin(&w, buf, cap);
    uint32_t at = blk_begin(&w, AR_APM_IID, PARAM_SG_CONFIG);
    put32(&w, 1);
    put32(&w, g->sg); put32(&w, 3);
    prop(&w, PROP_SG_PERF, perf); prop(&w, PROP_SG_DIRECTION, g->direction); prop(&w, PROP_SG_SCENARIO, SCENARIO_ANY);
    blk_end(&w, at);
    at = blk_begin(&w, AR_APM_IID, PARAM_CONTAINER_CONFIG);
    put32(&w, g->containers);
    for (unsigned i = 0; i < g->containers; ++i) {
        put32(&w, g->c[i].id); put32(&w, 7);
        put32(&w, PROP_CONT_CAPS); put32(&w, 8); put32(&w, 1); put32(&w, g->c[i].caps);
        prop(&w, PROP_CONT_POS, g->c[i].position); prop(&w, PROP_CONT_STACK, g->c[i].stack); prop(&w, PROP_CONT_DOMAIN, DOMAIN_ADSP);
        prop(&w, PROP_CONT_HEAP, 1); prop(&w, PROP_CONT_PARENT, 0xffffffffu); prop(&w, PROP_CONT_PEER_HEAP, g->c[i].peer_heap);
    }
    blk_end(&w, at);
    at = blk_begin(&w, AR_APM_IID, PARAM_MODULE_LIST);
    put32(&w, g->containers);                 /* every container holds a module */
    for (unsigned i = 0; i < g->containers; ++i) {
        unsigned n = 0;
        for (unsigned k = 0; k < g->modules; ++k) n += g->m[k].container == g->c[i].id;
        put32(&w, g->sg); put32(&w, g->c[i].id); put32(&w, n);
        for (unsigned k = 0; k < g->modules; ++k) if (g->m[k].container == g->c[i].id) {put32(&w, g->m[k].module); put32(&w, g->m[k].iid);}
    }
    blk_end(&w, at);
    at = blk_begin(&w, AR_APM_IID, PARAM_MODULE_PROP);
    put32(&w, g->modules);
    for (unsigned k = 0; k < g->modules; ++k) {
        put32(&w, g->m[k].iid); put32(&w, 1);
        put32(&w, PROP_PORT_INFO); put32(&w, 8); put32(&w, g->m[k].inputs); put32(&w, g->m[k].outputs);
    }
    blk_end(&w, at);
    at = blk_begin(&w, AR_APM_IID, PARAM_MODULE_CONN);
    put32(&w, g->links);
    for (unsigned k = 0; k < g->links; ++k) {put32(&w, g->l[k].from); put32(&w, g->l[k].from_port); put32(&w, g->l[k].to); put32(&w, g->l[k].to_port);}
    blk_end(&w, at);
    if (g->ctrl) {
        at = blk_begin(&w, AR_APM_IID, PARAM_CTRL_LINK);
        put32(&w, 1);
        put32(&w, SPK_IID_SP_TX); put32(&w, CTRL_PORT); put32(&w, SPK_IID_SP_RX); put32(&w, CTRL_PORT); put32(&w, 2);
        put32(&w, PROP_LINK_INTENTS); put32(&w, 8); put32(&w, 1); put32(&w, LINK_INTENT_FSADSP);
        prop(&w, PROP_LINK_0x136F, 1);
        blk_end(&w, at);
    }
    return w.bad ? 0 : w.n;
}

uint32_t spk_open_payload(uint8_t *buf, uint32_t cap, int vi) {return open_payload(buf, cap, vi ? &kVi : &kDev, PERF_LOW_LATENCY);}

uint32_t spk_mip_open_payload(uint8_t *buf, uint32_t cap, unsigned perf)
{
    if (perf != PERF_LOW_POWER && perf != PERF_LOW_LATENCY) return 0;
    return open_payload(buf, cap, &kMip, perf);
}

uint32_t spk_oob(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, uint32_t opcode, uint64_t address, uint32_t handle,
                 uint32_t bytes)
{
    if (!handle || !bytes || (bytes & 3u) || (address & 31u)) return 0;   /* the APM's rules for an out-of-band payload */
    W w;
    pkt_begin(&w, out, cap, port, token, opcode);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, handle); put32(&w, bytes);
    return pkt_finish(&w);
}

static void tdm_block(W *w, uint32_t iid, const ArTdm *t)
{
    const uint32_t at = blk_begin(w, iid, PARAM_TDM_INTF);
    put32(w, t->lpaif_type); put32(w, t->intf_idx); put16(w, t->sync_src); put16(w, t->data_out_enable);
    put32(w, t->slot_mask); put16(w, t->slots); put16(w, t->slot_width); put16(w, t->sync_mode); put16(w, t->invert_sync);
    put16(w, t->data_delay); put16(w, 0);
    blk_end(w, at);
}
static void word_block(W *w, uint32_t iid, uint32_t pid, uint32_t v) {const uint32_t at = blk_begin(w, iid, pid); put32(w, v); blk_end(w, at);}

uint32_t spk_endpoint_config(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, int vi, const ArTdm *t, const ArFormat *f)
{
    if (!t || !f || f->channels != 4 || t->slots != 4 || f->bits != 32 || t->slot_width != 32) return 0;
    const uint32_t iid = vi ? SPK_IID_VI_TDM : SPK_IID_TDM;
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    tdm_block(&w, iid, t);
    uint32_t at = blk_begin(&w, iid, PARAM_HW_EP_MF);
    put32(&w, f->rate); put16(&w, f->bits); put16(&w, f->channels); put32(&w, 1);
    blk_end(&w, at);
    word_block(&w, iid, PARAM_FRAME_SIZE_FACTOR, 1);
    word_block(&w, iid, PARAM_TDM_LANE, vi ? SPK_LANE_VI : SPK_LANE_RX);
    if (vi) {
        at = blk_begin(&w, iid, PARAM_SRC_CHANNEL_MAP);
        put16(&w, 4);
        for (unsigned c = 0; c < 4; ++c) put16(&w, CH_VI_FIRST + c);
        blk_end(&w, at);
    } else {
        uint8_t map[4] = {0};
        (void)ar_channel_map(4, map);
        at = blk_begin(&w, SPK_IID_MFC, PARAM_MFC_OUTPUT);
        put32(&w, f->rate); put16(&w, f->bits); put16(&w, f->channels);
        for (unsigned c = 0; c < 4; ++c) put16(&w, map[c]);
        blk_end(&w, at);
    }
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}

static void data_block(W *w, uint32_t iid, uint32_t pid, const uint8_t *data, uint32_t size)
{
    const uint32_t at = blk_begin(w, iid, pid);
    for (uint32_t i = 0; i < size && !w->bad; ++i) put8(w, data[i]);
    blk_end(w, at);
}

uint32_t spk_param_block(uint8_t *buf, uint32_t cap, uint32_t iid, uint32_t pid, const uint8_t *data, uint32_t size)
{
    W w;
    buf_begin(&w, buf, cap);
    data_block(&w, iid, pid, data, size);
    return w.bad ? 0 : w.n;
}

uint32_t spk_set_entries(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, const SpkParam *e, unsigned n, const uint8_t *order,
                         unsigned first, unsigned count)
{
    if (!e || !count) return 0;
    for (unsigned i = first; i < first + count; ++i) if (order[i] >= n) return 0;
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    for (unsigned i = first; i < first + count; ++i) data_block(&w, e[order[i]].iid, e[order[i]].pid, e[order[i]].data, e[order[i]].size);
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}

uint32_t spk_set_params(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, const SpkProt *p, const uint8_t *order,
                        unsigned first, unsigned count)
{
    if (!p || !count || first + count > p->count) return 0;
    return spk_set_entries(out, cap, port, token, p->e, p->count, order, first, count);
}

/* MIPLAY's MFC 0x4351: our stream is four channels FL FR LS RS carrying L R L R (audio_stereo_to_quad), the DAP takes two. The
 * mixer's single table takes FL to FL and RS to FR (Q14 unity): its coefficients read the same as [output][input] and as
 * [input][output], so either layout gives L and R. */
static const uint16_t kMipMixer[] = {1, 0, 2, 4, AR_CH_FL, AR_CH_FR, AR_CH_FL, AR_CH_FR, AR_CH_LS, AR_CH_RS, 16384, 0, 0, 0, 0, 0, 0, 16384};
static void preg_block(W *w, uint32_t preg_pid, int32_t preg)
{
    const uint32_t at = blk_begin(w, SPK_IID_DAP, preg_pid);
    put32(w, (uint32_t)preg);
    blk_end(w, at);
}
uint32_t spk_mip_config(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, unsigned rotation, uint32_t preg_pid, int32_t preg)
{
    if (rotation > 3u || preg > 0 || preg < SPK_PREG_MUTE) return 0;
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    uint32_t at = blk_begin(&w, SPK_IID_MIP_MFC_IN, PARAM_MFC_OUTPUT);
    put32(&w, 48000); put16(&w, 24); put16(&w, 2); put16(&w, AR_CH_FL); put16(&w, AR_CH_FR);   /* PAL's payloadMFCConfig */
    blk_end(&w, at);
    at = blk_begin(&w, SPK_IID_MIP_MFC_IN, SPK_PID_CHMIXER);
    for (unsigned i = 0; i < sizeof kMipMixer / sizeof kMipMixer[0]; ++i) put16(&w, kMipMixer[i]);
    blk_end(&w, at);
    word_block(&w, SPK_IID_MIP_ROT, SPK_PID_MIP_ROTATION, rotation);
    if (preg_pid) preg_block(&w, preg_pid, preg);
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}
uint32_t spk_preg(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, uint32_t preg_pid, int32_t preg)
{
    if (!preg_pid || preg > 0 || preg < SPK_PREG_MUTE) return 0;
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    preg_block(&w, preg_pid, preg);
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}

uint32_t spk_re25(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, const uint32_t re25[4])
{
    for (unsigned c = 0; c < 4; ++c) if (re25[c] < SPK_RE25_MIN || re25[c] > SPK_RE25_MAX) return 0;
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    const uint32_t at = blk_begin(&w, SPK_IID_SP_RX, SPK_PID_RE25);
    put16(&w, 0); put16(&w, 4);
    for (unsigned c = 0; c < 4; ++c) {put32(&w, 0); put32(&w, re25[c]);}
    blk_end(&w, at);
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}

uint32_t spk_vbat(uint8_t *out, uint32_t cap, uint32_t port, uint32_t token, uint32_t microvolts)
{
    if (microvolts < 2500000u || microvolts > 5000000u) return 0;   /* one cell, as the gauges read it */
    W w;
    pkt_begin(&w, out, cap, port, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_begin(&w);
    const uint32_t at = blk_begin(&w, SPK_IID_SP_RX, SPK_PID_VBAT);
    put32(&w, 1); put32(&w, microvolts); put32(&w, 0); put32(&w, 0);
    blk_end(&w, at);
    cmd_end(&w, cmd);
    return pkt_finish(&w);
}

void spk_live_stage(uint8_t slot[SPK_LIVE_SLOT])
{
    W w;
    buf_begin(&w, slot, SPK_LIVE_SLOT);
    put32(&w, SPK_IID_SP_RX); put32(&w, SPK_PID_LIVE); put32(&w, SPK_LIVE_BYTES); put32(&w, 0);
    pad_to(&w, SPK_LIVE_SLOT);
}

int spk_live_parse(const uint8_t slot[SPK_LIVE_SLOT], SpkLive *out)
{
    if (le32(slot) != SPK_IID_SP_RX || le32(slot + 4) != SPK_PID_LIVE || le32(slot + 8) != SPK_LIVE_BYTES || le32(slot + 12)) return -1;
    const uint8_t *d = slot + PARAM_HEADER;
    for (unsigned c = 0; c < 4; ++c, d += 24) {
        out->re[c] = le32(d); out->temp[c] = le32(d + 4); out->w2[c] = le32(d + 8);
        out->f0[c] = le32(d + 12); out->q[c] = le32(d + 16); out->w5[c] = le32(d + 20);
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------- the plan */
static const SpkPlan *plan(const ArSeq *s) {return (const SpkPlan *)s->context;}

uint64_t spk_plan_address(const SpkPlan *p, uint32_t offset)
{
    uint64_t base;
    if (ar_dsp_address(p->base.buffer_phys, p->base.stream_id, &base)) return 0;
    return base + p->param_offset + offset;
}

/* the CPU's bit-clock request: attribute 1 (couple no), or 4 (invert, couple no) with +4, as the ACDB's HW_INTF_CLK_CFG asks */
static uint32_t clock_attribute(const ArSeq *s) {return (((const SpkPlan *)s->context)->variant & SPK_VAR_INVERT_CLOCK) ? 4u : 1u;}
static uint32_t clock_on(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    return ar_prm_clock_attr(o, c, s->prm_port, t, AR_CLK_SEC_TDM_IBIT, AR_TDM_BIT_CLOCK, clock_attribute(s));
}
static uint32_t clock_off(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    return ar_prm_clock_attr(o, c, s->prm_port, t, AR_CLK_SEC_TDM_IBIT, 0, clock_attribute(s));
}
static uint32_t map_region(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    uint64_t address;
    if (p->base.buffer_phys % 4096u || ar_dsp_address(p->base.buffer_phys, p->base.stream_id, &address)) return 0;
    return ar_map(o, c, s->port, t, address, p->region_bytes);
}
static uint32_t unmap_region(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return s->handle ? ar_unmap(o, c, s->port, t, s->handle) : 0;}
static uint32_t open_vi(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    return spk_oob(o, c, s->port, t, AR_OP_GRAPH_OPEN, spk_plan_address(p, p->vi_open), s->handle, p->vi_open_bytes);
}
static uint32_t open_dev(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    return spk_oob(o, c, s->port, t, AR_OP_GRAPH_OPEN, spk_plan_address(p, p->dev_open), s->handle, p->dev_open_bytes);
}
#define GRAPH_CMD(name, op, sg) static uint32_t name(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_graph_command(o, c, s->port, t, op, sg);}
GRAPH_CMD(close_vi, AR_OP_GRAPH_CLOSE, SPK_SG_VI)
GRAPH_CMD(prep_vi, AR_OP_GRAPH_PREPARE, SPK_SG_VI)
GRAPH_CMD(start_vi, AR_OP_GRAPH_START, SPK_SG_VI)
GRAPH_CMD(stop_vi, AR_OP_GRAPH_STOP, SPK_SG_VI)
GRAPH_CMD(close_dev, AR_OP_GRAPH_CLOSE, SPK_SG_DEV)
GRAPH_CMD(prep_dev, AR_OP_GRAPH_PREPARE, SPK_SG_DEV)
GRAPH_CMD(start_dev, AR_OP_GRAPH_START, SPK_SG_DEV)
GRAPH_CMD(stop_dev, AR_OP_GRAPH_STOP, SPK_SG_DEV)
GRAPH_CMD(close_str, AR_OP_GRAPH_CLOSE, AR_SG_PLAY)
GRAPH_CMD(prep_str, AR_OP_GRAPH_PREPARE, AR_SG_PLAY)
GRAPH_CMD(start_str, AR_OP_GRAPH_START, AR_SG_PLAY)
GRAPH_CMD(stop_str, AR_OP_GRAPH_STOP, AR_SG_PLAY)
GRAPH_CMD(close_mip, AR_OP_GRAPH_CLOSE, SPK_SG_MIP)
GRAPH_CMD(prep_mip, AR_OP_GRAPH_PREPARE, SPK_SG_MIP)
GRAPH_CMD(start_mip, AR_OP_GRAPH_START, SPK_SG_MIP)
GRAPH_CMD(stop_mip, AR_OP_GRAPH_STOP, SPK_SG_MIP)
static uint32_t open_mip(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    return spk_oob(o, c, s->port, t, AR_OP_GRAPH_OPEN, spk_plan_address(p, p->mip_open), s->handle, p->mip_open_bytes);
}
static uint32_t mip_cfg(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    return spk_mip_config(o, c, s->port, t, spk_miplay_rotation(p->miplay), p->mip ? p->mip->preg_pid : 0, p->preg);
}
static uint32_t vi_endpoint(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return spk_endpoint_config(o, c, s->port, t, 1, &plan(s)->base.tdm, &plan(s)->base.slots);}
static uint32_t dev_endpoint(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return spk_endpoint_config(o, c, s->port, t, 0, &plan(s)->base.tdm, &plan(s)->base.slots);}
/* A parameter chunk: which one is the step being built (ar_seq_next builds step s->done). */
static uint32_t chunk_cmd(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    const SpkPlan *p = plan(s);
    if (s->done >= p->count || p->step_chunk[s->done] < 0) return 0;
    const SpkChunk *k = &p->chunk[p->step_chunk[s->done]];
    if (k->oob) return spk_oob(o, c, s->port, t, AR_OP_SET_CFG, spk_plan_address(p, k->offset), s->handle, k->bytes);
    if (k->graph == 3) return p->mip ? spk_set_entries(o, c, s->port, t, p->mip->e, p->mip->count, p->order, k->first, k->count) : 0;
    return spk_set_params(o, c, s->port, t, p->prot, p->order, k->first, k->count);
}
static uint32_t re25_cmd(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return spk_re25(o, c, s->port, t, plan(s)->prot->re25);}
static uint32_t open_str(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    if (plan(s)->miplay) return ar_open_playback_stream_to_port(o, c, s->port, t, SPK_IID_MIP_SYNC, SPK_MIP_STREAM_PORT);
    return ar_open_playback_stream_to(o, c, s->port, t, SPK_IID_MFC);
}
static uint32_t str_shmem(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_playback_shmem(o, c, s->port, t, &plan(s)->base.stream);}
static uint32_t str_pcm(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_playback_stream(o, c, s->port, t, &plan(s)->base.stream);}
static uint32_t str_log(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return ar_config_playback_logger(o, c, s->port, t);}
static int amps_on(const ArSeq *s) {const SpkPlan *p = plan(s); return p->base.amps_on ? p->base.amps_on(p->base.owner) : -1;}
static int amps_off(const ArSeq *s) {const SpkPlan *p = plan(s); return p->base.amps_off ? p->base.amps_off(p->base.owner) : -1;}

static int add(SpkPlan *p, const char *name, ArBuild build, uint32_t opcode, int answer, ArLocal local, ArBuild undo, uint32_t undo_opcode,
               int undo_answer, ArLocal undo_local, int prm, int chunk)
{
    if (p->count >= SPK_MAX_STEPS) return -1;
    ArStep *st = &p->steps[p->count];
    st->name = name; st->build = build; st->opcode = opcode; st->answer = answer; st->local = local;
    st->undo = undo; st->undo_opcode = undo_opcode; st->undo_answer = undo_answer; st->undo_local = undo_local; st->prm = prm;
    p->step_chunk[p->count++] = (int8_t)chunk;
    return 0;
}
#define CMD(p, name, build, op) add(p, name, build, op, ArBasicResult, 0, 0, 0, 0, 0, 0, -1)
#define CMD_UNDO(p, name, build, op, undo, uop) add(p, name, build, op, ArBasicResult, 0, undo, uop, ArBasicResult, 0, 0, -1)

static const char *const kChunkNames[4][2] = {{"", ""}, {"V/I parameters (in band)", "V/I parameter (out of band)"},
                                              {"speaker parameters (in band)", "speaker parameter (out of band)"},
                                              {"MIPLAY parameters (in band)", "MIPLAY parameter (out of band)"}};

int spk_request_valid(int request)
{
    if (request == 0) return 1;
    if (request < 0 || (request & 1) == 0 || ((uint32_t)request & ~SPK_VAR_ALL)) return 0;
    return ((uint32_t)request & (SPK_VAR_ENDPOINT_CLOCK | SPK_VAR_INVERT_CLOCK)) != (SPK_VAR_ENDPOINT_CLOCK | SPK_VAR_INVERT_CLOCK);
}

const char *spk_variant_text(uint32_t variant, char *buf, unsigned cap)
{
    const char *order = (variant & SPK_VAR_VI_FIRST) ? "round-1 order (V/I first)" : "Android order";
    const char *clock = (variant & SPK_VAR_ENDPOINT_CLOCK) ? "endpoint-owned clock (no CPU request)"
                        : (variant & SPK_VAR_INVERT_CLOCK) ? "CPU clock attribute 4 (inverted)" : "CPU clock attribute 1";
    const char *cfg = (variant & SPK_VAR_NO_CLK_CFG) ? "HW_INTF_CLK_CFG not sent" : "HW_INTF_CLK_CFG sent";
    unsigned n = 0;
    const char *part[3] = {order, clock, cfg};
    if (!buf || !cap) return "";
    for (unsigned k = 0; k < 3; ++k) {
        if (k && n + 2 < cap) {buf[n++] = ','; buf[n++] = ' ';}
        for (const char *c = part[k]; *c && n + 1 < cap; ++c) buf[n++] = *c;
    }
    buf[n] = 0;
    return buf;
}

/* a parameter this variant does not send: the endpoints' HW_INTF_CLK_CFG with +8 */
static int skipped(const SpkPlan *p, const SpkParam *e) {return (p->variant & SPK_VAR_NO_CLK_CFG) && e->pid == SPK_PID_HW_INTF_CLK;}

/* entry i of graph g (1, 2: the protection file; 3: MIPLAY's) */
static const SpkParam *entry(const SpkPlan *p, int g, unsigned i) {return g == 3 ? &p->mip->e[i] : &p->prot->e[i];}

int spk_plan_init(SpkPlan *p, const SpkProt *prot, uint8_t *area, uint32_t area_bytes, const char **why)
{
    p->prot = prot;
    p->skipped = 0;
    p->region_bytes = AUDIO_DMA_PARAM + AUDIO_DMA_PARAM_BYTES;
    p->param_offset = AUDIO_DMA_PARAM;
    p->chunks = p->vi_chunks = p->oob_chunks = p->mip_chunks = p->count = 0;
    p->mip_open = p->mip_open_bytes = 0;
    uint64_t address;
    if (!prot || !prot->have_params || !prot->have_re25) return fail(why, "the private files are not both valid");
    if (!area || area_bytes < AUDIO_DMA_PARAM_BYTES) return fail(why, "no parameter area");
    if (p->base.buffer_bytes > p->param_offset || p->base.buffer_phys % 4096u || ar_dsp_address(p->base.buffer_phys, p->base.stream_id, &address) ||
        (uint64_t)(uint32_t)address + p->region_bytes > 0x100000000ull)
        return fail(why, "the region is not DSP-addressable");
    if (!spk_request_valid((int)(p->variant | 1u))) return fail(why, "an invalid variant");
    const int with_mip = p->miplay != 0;
    if (with_mip && (!spk_miplay_valid((int)p->miplay) || !p->mip || !p->mip->have)) return fail(why, "MIPLAY asked for without a valid MIPLAY file");
    if (with_mip && (p->preg > 0 || p->preg < SPK_PREG_MUTE)) return fail(why, "a MIPLAY pregain out of range");
    /* the order: per graph (V/I, speaker, MIPLAY), the parameters that fit in band, then those that go out of band */
    unsigned n = 0;
    for (unsigned i = 0; i < prot->count; ++i) p->skipped += (unsigned)skipped(p, &prot->e[i]);
    for (int g = 1; g <= 2; ++g)
        for (int big = 0; big < 2; ++big)
            for (unsigned i = 0; i < prot->count; ++i)
                if (!skipped(p, &prot->e[i]) && spk_iid_graph(prot->e[i].iid) == g &&
                    (PARAM_HEADER + align8(prot->e[i].size) > SPK_INBAND_MAX) == big) p->order[n++] = (uint8_t)i;
    if (n + p->skipped != prot->count) return fail(why, "a parameter of no graph");
    const unsigned prot_n = n;
    if (with_mip)
        for (int big = 0; big < 2; ++big)
            for (unsigned i = 0; i < p->mip->count; ++i)
                if ((PARAM_HEADER + align8(p->mip->e[i].size) > SPK_INBAND_MAX) == big) p->order[n++] = (uint8_t)i;
    /* the parameter area: the opens, the live slot, then every out-of-band parameter */
    uint32_t at = 0;
    p->vi_open = at;
    p->vi_open_bytes = spk_open_payload(area + at, area_bytes - at, 1);
    at = align32(at + p->vi_open_bytes);
    p->dev_open = at;
    p->dev_open_bytes = at < area_bytes ? spk_open_payload(area + at, area_bytes - at, 0) : 0;
    at = align32(at + p->dev_open_bytes);
    if (with_mip) {
        p->mip_open = at;
        p->mip_open_bytes = at < area_bytes ? spk_mip_open_payload(area + at, area_bytes - at, spk_miplay_perf(p->miplay)) : 0;
        if (!p->mip_open_bytes) return fail(why, "the opens do not fit the parameter area");
        at = align32(at + p->mip_open_bytes);
    }
    if (!p->vi_open_bytes || !p->dev_open_bytes || at + SPK_LIVE_SLOT > area_bytes) return fail(why, "the opens do not fit the parameter area");
    p->live_slot = at;
    spk_live_stage(area + at);
    at = align32(at + SPK_LIVE_SLOT);
    uint32_t sum = 0;
    for (unsigned i = 0; i < n; ++i) {
        const int g = i < prot_n ? spk_iid_graph(prot->e[p->order[i]].iid) : 3;
        const SpkParam *e = entry(p, g, p->order[i]);
        const uint32_t block = PARAM_HEADER + align8(e->size);
        SpkChunk *last = p->chunks ? &p->chunk[p->chunks - 1] : 0;
        if (block <= SPK_INBAND_MAX && last && !last->oob && last->graph == g && sum + block <= SPK_INBAND_MAX) {
            last->count++;
            sum += block;
            continue;
        }
        if (p->chunks >= SPK_MAX_CHUNKS) return fail(why, "too many parameter chunks");
        SpkChunk *k = &p->chunk[p->chunks++];
        mem_zero(k, sizeof *k);
        k->graph = (uint8_t)g;
        k->first = (uint16_t)i; k->count = 1;
        if (block > SPK_INBAND_MAX) {
            k->oob = 1;
            k->offset = at;
            k->bytes = at < area_bytes ? spk_param_block(area + at, area_bytes - at, e->iid, e->pid, e->data, e->size) : 0;
            if (!k->bytes) return fail(why, "the out-of-band parameters do not fit the parameter area");
            at = align32(at + k->bytes);
            p->oob_chunks++;
        }
        sum = block;
        p->vi_chunks += (unsigned)(g == 1);
        p->mip_chunks += (unsigned)(g == 3);
    }
    p->staged = at;
    /* the steps */
    int r = 0;
    if (!(p->variant & SPK_VAR_ENDPOINT_CLOCK))
        r |= add(p, "tdm bit clock 6.144 MHz", clock_on, AR_OP_PRM_REQUEST, ArPrmDone, 0, clock_off, AR_OP_PRM_RELEASE, ArPrmDone, 0, 1, -1);
    r |= add(p, "map the periods and the parameter area", map_region, AR_OP_MEM_MAP, ArMapped, 0, unmap_region, AR_OP_MEM_UNMAP,
             ArBasicResult, 0, 0, -1);
    const int vi_first = (p->variant & SPK_VAR_VI_FIRST) != 0;
    for (int round = 0; round < 2; ++round) {
        if (round == vi_first) {   /* Android's order: the speaker graph (SP_RX, the IMCL peer) and the stream opened first */
            r |= CMD_UNDO(p, "open speaker graph b0000002 (out of band)", open_dev, AR_OP_GRAPH_OPEN, close_dev, AR_OP_GRAPH_CLOSE);
            r |= CMD(p, "speaker TDM sink: interface, format, lane 1; MFC 4 x 32-bit", dev_endpoint, AR_OP_SET_CFG);
            for (unsigned k = 0; k < p->chunks; ++k)
                if (p->chunk[k].graph == 2) r |= add(p, kChunkNames[2][p->chunk[k].oob], chunk_cmd, AR_OP_SET_CFG, ArBasicResult, 0, 0, 0, 0, 0, 0, (int)k);
            r |= CMD(p, "Re25 to SP_RX", re25_cmd, AR_OP_SET_CFG);
            if (with_mip) {   /* MIPLAY between the stream and the speaker graph's MFC: opened after the graph it links to */
                r |= CMD_UNDO(p, "open MIPLAY graph b000019f (out of band)", open_mip, AR_OP_GRAPH_OPEN, close_mip, AR_OP_GRAPH_CLOSE);
                for (unsigned k = 0; k < p->chunks; ++k)
                    if (p->chunk[k].graph == 3) r |= add(p, kChunkNames[3][p->chunk[k].oob], chunk_cmd, AR_OP_SET_CFG, ArBasicResult, 0, 0, 0, 0, 0, 0, (int)k);
                r |= CMD(p, "MIPLAY: MFC 0x4351 48 kHz / 24 bit / 2 ch, rotation, pregain", mip_cfg, AR_OP_SET_CFG);
            }
            r |= CMD_UNDO(p, with_mip ? "open playback stream graph (to MIPLAY's SYNC)" : "open playback stream graph (to the MFC)", open_str,
                          AR_OP_GRAPH_OPEN, close_str, AR_OP_GRAPH_CLOSE);
            r |= CMD(p, "stream shared memory format", str_shmem, AR_OP_SET_CFG);
            r |= CMD(p, "stream decoder and converter", str_pcm, AR_OP_SET_CFG);
            r |= CMD(p, "stream logger", str_log, AR_OP_SET_CFG);
        } else {
            r |= CMD_UNDO(p, "open V/I graph b00000d5 (out of band)", open_vi, AR_OP_GRAPH_OPEN, close_vi, AR_OP_GRAPH_CLOSE);
            r |= CMD(p, "V/I TDM source: interface, format, lane 0, V/I channels", vi_endpoint, AR_OP_SET_CFG);
            for (unsigned k = 0; k < p->chunks; ++k)
                if (p->chunk[k].graph == 1) r |= add(p, kChunkNames[1][p->chunk[k].oob], chunk_cmd, AR_OP_SET_CFG, ArBasicResult, 0, 0, 0, 0, 0, 0, (int)k);
            r |= CMD(p, "prepare V/I", prep_vi, AR_OP_GRAPH_PREPARE);
            r |= CMD_UNDO(p, "start V/I (before the speaker)", start_vi, AR_OP_GRAPH_START, stop_vi, AR_OP_GRAPH_STOP);
        }
    }
    r |= CMD(p, "prepare speaker", prep_dev, AR_OP_GRAPH_PREPARE);
    if (with_mip) r |= CMD(p, "prepare MIPLAY", prep_mip, AR_OP_GRAPH_PREPARE);
    r |= CMD(p, "prepare stream", prep_str, AR_OP_GRAPH_PREPARE);
    r |= CMD_UNDO(p, "start speaker (bit clock runs)", start_dev, AR_OP_GRAPH_START, stop_dev, AR_OP_GRAPH_STOP);
    if (with_mip) r |= CMD_UNDO(p, "start MIPLAY", start_mip, AR_OP_GRAPH_START, stop_mip, AR_OP_GRAPH_STOP);
    r |= CMD_UNDO(p, "start stream", start_str, AR_OP_GRAPH_START, stop_str, AR_OP_GRAPH_STOP);
    r |= add(p, "amplifiers on", 0, 0, 0, amps_on, 0, 0, 0, amps_off, 0, -1);
    if (r) return fail(why, "too many steps");
    return 0;
}

/* --------------------------------------------------------------------------------------------------------- the monitor */
static const SpkMonitor *mon(const ArSeq *s) {return (const SpkMonitor *)s->context;}
static uint32_t vbat_cmd(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return spk_vbat(o, c, s->port, t, mon(s)->vbat_uv);}
static uint32_t live_cmd(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t)
{
    return spk_oob(o, c, s->port, t, AR_OP_GET_CFG, mon(s)->live_address, mon(s)->handle, SPK_LIVE_SLOT);
}
static uint32_t preg_cmd(const ArSeq *s, uint8_t *o, uint32_t c, uint32_t t) {return spk_preg(o, c, s->port, t, mon(s)->preg_pid, mon(s)->preg);}
/* {pregain, battery, live} and {pregain, live}: every combination is a run of one of the two */
static const ArStep kMonitor[] = {
    {"Dolby pregain to the DAP (MIPLAY)", preg_cmd, AR_OP_SET_CFG, ArBasicResult, 0, 0, 0, 0, 0, 0},
    {"battery voltage to SP_RX", vbat_cmd, AR_OP_SET_CFG, ArBasicResult, 0, 0, 0, 0, 0, 0},
    {"live data from SP_RX", live_cmd, AR_OP_GET_CFG, ArGetCfgDone, 0, 0, 0, 0, 0, 0},
};
static const ArStep kMonitorNoVbat[] = {
    {"Dolby pregain to the DAP (MIPLAY)", preg_cmd, AR_OP_SET_CFG, ArBasicResult, 0, 0, 0, 0, 0, 0},
    {"live data from SP_RX", live_cmd, AR_OP_GET_CFG, ArGetCfgDone, 0, 0, 0, 0, 0, 0},
};

const ArStep *spk_monitor_plan(int with_vbat, int with_preg, unsigned *count)
{
    if (with_preg && !with_vbat) {*count = 2u; return kMonitorNoVbat;}
    *count = 1u + (unsigned)(with_vbat != 0) + (unsigned)(with_preg != 0);
    return kMonitor + (3u - *count);
}

const ArStep *spk_monitor_steps(int with_vbat, unsigned *count) {return spk_monitor_plan(with_vbat, 0, count);}
