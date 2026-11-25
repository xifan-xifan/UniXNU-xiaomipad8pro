/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* MiPPS charger authentication engine (ChargeMipps.h). Pure C: no I/O, no clock, no allocation. */
#include "ChargeMipps.h"
#include <string.h>

/* ---------------------------------------------------------------- SHA-256 / HMAC-SHA256 (FIPS 180-4, RFC 2104) */
typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t buf[64];
    uint32_t used;
} Sha256;

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void sha_block(Sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (unsigned i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (unsigned i = 16; i < 64; i++) {
        const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (unsigned i = 0; i < 64; i++) {
        const uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
    memset(w, 0, sizeof w);
}

static void sha_init(Sha256 *s)
{
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(s->h, iv, sizeof iv);
    s->bytes = 0;
    s->used = 0;
}

static void sha_update(Sha256 *s, const uint8_t *p, size_t n)
{
    s->bytes += n;
    while (n) {
        if (!s->used && n >= 64) { sha_block(s, p); p += 64; n -= 64; continue; }
        const uint32_t take = (uint32_t)(n < 64u - s->used ? n : 64u - s->used);
        memcpy(s->buf + s->used, p, take);
        s->used += take; p += take; n -= take;
        if (s->used == 64) { sha_block(s, s->buf); s->used = 0; }
    }
}

static void sha_final(Sha256 *s, uint8_t out[32])
{
    const uint64_t bits = s->bytes * 8u;
    const uint8_t one = 0x80, zero = 0;
    sha_update(s, &one, 1);
    while (s->used != 56) sha_update(s, &zero, 1);
    uint8_t len[8];
    for (unsigned i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (unsigned i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i];
    }
    memset(s, 0, sizeof *s);
}

void piano_mipps_sha256(const uint8_t *data, size_t bytes, uint8_t digest[32])
{
    Sha256 s;
    sha_init(&s);
    sha_update(&s, data, bytes);
    sha_final(&s, digest);
}

void piano_mipps_hmac_sha256(const uint8_t *key, size_t key_bytes, const uint8_t *msg, size_t msg_bytes, uint8_t digest[32])
{
    uint8_t k[64], pad[64], inner[32];
    memset(k, 0, sizeof k);
    if (key_bytes > 64) piano_mipps_sha256(key, key_bytes, k);
    else if (key_bytes) memcpy(k, key, key_bytes);
    Sha256 s;
    for (unsigned i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36u);
    sha_init(&s); sha_update(&s, pad, 64); sha_update(&s, msg, msg_bytes); sha_final(&s, inner);
    for (unsigned i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5cu);
    sha_init(&s); sha_update(&s, pad, 64); sha_update(&s, inner, 32); sha_final(&s, digest);
    memset(k, 0, sizeof k); memset(pad, 0, sizeof pad); memset(inner, 0, sizeof inner);
}

/* ---------------------------------------------------------------- the private key file */
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int piano_mipps_keys_parse(const uint8_t *data, size_t bytes, PianoMippsKeys *out)
{
    static const uint8_t magic[8] = {'P', 'M', 'I', 'P', 'K', 'E', 'Y', '1'};
    if (!out) return PIANO_MIPPS_E_FORMAT;
    memset(out, 0, sizeof *out);
    if (!data || !bytes) return PIANO_MIPPS_E_NOKEY;
    if (bytes < 16 + 48 + 32 || memcmp(data, magic, 8) != 0) return PIANO_MIPPS_E_FORMAT;
    const uint32_t count = le32(data + 8), entry = le32(data + 12);
    if (!count || count > PIANO_MIPPS_KEYS_MAX || entry != 48 || bytes != 16u + 48u * count + 32u) return PIANO_MIPPS_E_FORMAT;
    uint8_t digest[32];
    piano_mipps_sha256(data, 16u + 48u * count, digest);
    uint8_t diff = 0;
    for (unsigned i = 0; i < 32; i++) diff |= (uint8_t)(digest[i] ^ data[16u + 48u * count + i]);
    memset(digest, 0, sizeof digest);
    if (diff) return PIANO_MIPPS_E_FORMAT;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(out->k[i].seed, data + 16 + 48 * i, 16);
        memcpy(out->k[i].key, data + 16 + 48 * i + 16, 32);
    }
    out->count = count;
    return PIANO_MIPPS_OK;
}

void piano_mipps_keys_wipe(PianoMippsKeys *keys)
{
    if (!keys) return;
    volatile uint8_t *p = (volatile uint8_t *)keys;
    for (size_t i = 0; i < sizeof *keys; i++) p[i] = 0;
}

/* ---------------------------------------------------------------- the engine */
/* The program: one step per firmware action; VDM steps run the write / 10 ms / poll loop with one resend. */
enum {
    P_VERIFY1 = 1, P_VDM1, P_VERSION, P_UNVERIFY0, P_VDM2, P_VDM3, P_VDM4_SEED, P_VDM5_CHALLENGE, P_READ_AUTH, P_READ_ID,
    P_DIGEST, P_VDM7_OK, P_VDM6_OK, P_VBUS_FINAL, P_VERIFIED1, P_CONFIRM,
    F_UNVERIFY0, F_VDM7_FAIL, F_CLEAR, F_DONE,
};
/* VDM sub-states */
enum { V_WRITE = 0, V_SETTLE, V_VBUS, V_STATE, V_PAUSE };

static uint32_t vdm_timeout_ms(uint32_t cmd, uint32_t flag)
{
    static const uint32_t t[] = {0, 500, 500, 1000, 1500, 2500, 1000, 1000, 1000};
    if (cmd == 6 && flag == 1) return 500;
    return cmd < sizeof t / sizeof t[0] ? t[cmd] : 0;
}

static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* A 16-byte message on the wire: each 4-byte group, read big endian, stored as a little-endian word (and back): a byte swap per group. */
static void swap_groups(uint8_t out[16], const uint8_t in[16])
{
    for (unsigned g = 0; g < 4; g++)
        for (unsigned b = 0; b < 4; b++) out[4 * g + b] = in[4 * g + 3 - b];
}

static int vdm_step(uint32_t step) {
    return step == P_VDM1 || step == P_VDM2 || step == P_VDM3 || step == P_VDM4_SEED || step == P_VDM5_CHALLENGE ||
           step == P_VDM7_OK || step == P_VDM6_OK || step == F_VDM7_FAIL;
}

static void vdm_begin(PianoMipps *m, uint32_t step, uint32_t cmd, uint32_t flag)
{
    m->step = step; m->cmd = cmd; m->flag = flag; m->sub = V_WRITE; m->attempt = 0;
}

static void goto_finish(PianoMipps *m, int32_t error)
{
    if (m->pending_error == PIANO_MIPPS_OK) m->pending_error = error;
    m->error = m->pending_error;
    m->state = PIANO_MIPPS_CLEANUP;
    m->step = (m->error != PIANO_MIPPS_OK && m->discovered) ? F_UNVERIFY0 : F_CLEAR;
    m->sub = V_WRITE;
}

static void advance(PianoMipps *m)
{
    switch (m->step) {
    case P_VERIFY1: vdm_begin(m, P_VDM1, 1, 0); break;
    case P_VDM1: m->step = P_VERSION; m->sub = V_WRITE; break;
    case P_VERSION: m->step = P_UNVERIFY0; m->sub = V_WRITE; break;
    case P_UNVERIFY0: vdm_begin(m, P_VDM2, 2, 0); break;
    case P_VDM2: vdm_begin(m, P_VDM3, 3, 0); break;
    case P_VDM3: vdm_begin(m, P_VDM4_SEED, 4, 0); break;
    case P_VDM4_SEED: vdm_begin(m, P_VDM5_CHALLENGE, 5, 0); break;
    case P_VDM5_CHALLENGE: m->step = P_READ_AUTH; m->sub = V_WRITE; break;
    case P_READ_AUTH: m->step = P_READ_ID; m->sub = V_WRITE; break;
    case P_READ_ID: m->step = P_DIGEST; m->sub = V_WRITE; m->id_next = 0; break;
    case P_DIGEST: vdm_begin(m, P_VDM7_OK, 7, 1); break;
    case P_VDM7_OK: vdm_begin(m, P_VDM6_OK, 6, 1); break;
    case P_VDM6_OK: m->step = P_VBUS_FINAL; m->sub = V_WRITE; break;
    case P_VBUS_FINAL: m->step = P_VERIFIED1; m->sub = V_WRITE; break;
    case P_VERIFIED1: m->step = P_CONFIRM; m->sub = V_WRITE; break;
    case P_CONFIRM: m->verified = 1; goto_finish(m, PIANO_MIPPS_OK); break;
    case F_UNVERIFY0: vdm_begin(m, F_VDM7_FAIL, 7, 0); break;
    case F_VDM7_FAIL: m->step = F_CLEAR; m->sub = V_WRITE; break;
    case F_CLEAR: m->cleared = 1; m->step = F_DONE; m->state = PIANO_MIPPS_DONE; break;
    default: break;
    }
}

int piano_mipps_eligible(int enabled, const PianoMippsKeys *keys, int real_type, const uint32_t *pdo_max_mv, unsigned pdos,
                         int hv_off, int cp_off, int attempted_this_attach)
{
    if (!enabled || !keys || !keys->count || !hv_off || !cp_off || attempted_this_attach) return 0;
    if (real_type != 10 && real_type != 11 && real_type != 12) return 0;
    for (unsigned i = 0; pdo_max_mv && i < pdos; i++)
        if (pdo_max_mv[i] > 5000) return 1;
    return 0;
}

int piano_mipps_start(PianoMipps *m, const PianoMippsKeys *keys, uint32_t key_index, const uint8_t challenge[16], uint64_t now_ms)
{
    if (!m) return PIANO_MIPPS_E_FORMAT;
    if (m->state == PIANO_MIPPS_RUNNING || m->state == PIANO_MIPPS_CLEANUP) return PIANO_MIPPS_E_BUSY;
    memset(m, 0, sizeof *m);
    if (!keys || !keys->count || !challenge) { m->state = PIANO_MIPPS_DONE; m->error = PIANO_MIPPS_E_NOKEY; m->cleared = 1; return m->error; }
    m->keys = keys;
    m->key_index = key_index % keys->count;
    memcpy(m->challenge, challenge, 16);
    m->state = PIANO_MIPPS_RUNNING;
    m->step = P_VERIFY1;
    m->sub = V_WRITE;
    m->wait_until_ms = now_ms;
    return PIANO_MIPPS_OK;
}

static int digest_matches(const PianoMipps *m, uint32_t id)
{
    uint8_t msg[20], d[32];
    memcpy(msg, m->challenge, 16);
    msg[16] = (uint8_t)(id >> 24); msg[17] = (uint8_t)(id >> 16); msg[18] = (uint8_t)(id >> 8); msg[19] = (uint8_t)id;
    const PianoMippsKey *k = &m->keys->k[m->key_index];
    piano_mipps_hmac_sha256(k->key, sizeof k->key, msg, sizeof msg, d);
    uint8_t diff = 0;
    for (unsigned i = 0; i < PIANO_MIPPS_VDM_BYTES; i++) diff |= (uint8_t)(d[i] ^ m->response[i]);
    memset(d, 0, sizeof d); memset(msg, 0, sizeof msg);
    return diff == 0;
}

static void req_write_u32(PianoMippsRequest *r, uint32_t prop, uint32_t v)
{
    r->kind = PIANO_MIPPS_REQ_WRITE; r->prop = prop; r->len = 4; put_le32(r->data, v);
}
static void req_read(PianoMippsRequest *r, uint32_t prop, uint32_t len) { r->kind = PIANO_MIPPS_REQ_READ; r->prop = prop; r->len = len; }

void piano_mipps_next(PianoMipps *m, uint64_t now_ms, int attach_current, int service_current, PianoMippsRequest *req)
{
    memset(req, 0, sizeof *req);
    if (m->state != PIANO_MIPPS_RUNNING && m->state != PIANO_MIPPS_CLEANUP) return;   /* idle or done: wait_until 0 */
    if (m->awaiting) { req->wait_until_ms = now_ms + PIANO_MIPPS_POLL_MS; return; }   /* a reply is owed */
    if (!service_current) {
        /* a new firmware service starts with no authentication running: nothing to clean, nothing more may be written */
        m->verified = 0;
        m->adapter_id = 0;
        m->error = PIANO_MIPPS_E_GONE;
        m->pending_error = PIANO_MIPPS_E_GONE;
        m->cleared = 0;
        m->state = PIANO_MIPPS_DONE;
        m->step = F_DONE;
        return;
    }
    if (m->state == PIANO_MIPPS_RUNNING && !attach_current) { m->verified = 0; goto_finish(m, PIANO_MIPPS_E_GONE); }
    if (now_ms < m->wait_until_ms) { req->wait_until_ms = m->wait_until_ms; return; }
    if (m->state == PIANO_MIPPS_CLEANUP && m->step == F_VDM7_FAIL && !attach_current) { m->step = F_CLEAR; m->sub = V_WRITE; }

    if (vdm_step(m->step)) {
        switch (m->sub) {
        case V_WRITE:
            if (m->cmd == 8) { m->sent8 = 1; }   /* never reached: no step sends 8 */
            req->kind = PIANO_MIPPS_REQ_WRITE;
            req->prop = PIANO_MIPPS_PROP_VDM_BASE + m->cmd - 1;
            if (m->step == P_VDM4_SEED) { swap_groups(req->data, m->keys->k[m->key_index].seed); req->len = 16; }
            else if (m->step == P_VDM5_CHALLENGE) { swap_groups(req->data, m->challenge); req->len = 16; }
            else { put_le32(req->data, m->flag); req->len = 4; }
            m->last_cmd = m->cmd;
            m->awaiting = 1;
            return;
        case V_SETTLE: case V_PAUSE: case V_VBUS:
            m->sub = V_VBUS;
            req_read(req, PIANO_MIPPS_PROP_VBUS, 4); m->awaiting = 1; return;
        case V_STATE:
            req_read(req, PIANO_MIPPS_PROP_UVDM_STATE, 4); m->awaiting = 1; return;
        default: return;
        }
    }
    switch (m->step) {
    case P_VERIFY1: req_write_u32(req, PIANO_MIPPS_PROP_VERIFY_PROCESS, 1); break;
    case P_VERSION: req_read(req, PIANO_MIPPS_PROP_VERSION, 4); break;
    case P_UNVERIFY0: case F_UNVERIFY0: req_write_u32(req, PIANO_MIPPS_PROP_PD_VERIFIED, 0); break;
    case P_READ_AUTH: req_read(req, PIANO_MIPPS_PROP_AUTH, 16); break;
    case P_READ_ID: req_read(req, PIANO_MIPPS_PROP_ADAPTER_ID, 4); break;
    case P_VBUS_FINAL: req_read(req, PIANO_MIPPS_PROP_VBUS, 4); break;
    case P_VERIFIED1: req_write_u32(req, PIANO_MIPPS_PROP_PD_VERIFIED, 1); break;
    case F_CLEAR: req_write_u32(req, PIANO_MIPPS_PROP_VERIFY_PROCESS, 0); break;
    case P_CONFIRM:
        if (attach_current) advance(m);
        else { m->verified = 0; goto_finish(m, PIANO_MIPPS_E_GONE); }
        req->wait_until_ms = now_ms;
        return;
    case P_DIGEST: {
        /* the reported ID first, then IDs 0..65535 in chunks (the caller's thread yields between calls) */
        if (m->id_next == 0 && m->adapter_id && digest_matches(m, m->adapter_id)) { advance(m); req->wait_until_ms = now_ms; return; }
        const uint32_t end = m->id_next + PIANO_MIPPS_ID_CHUNK;
        for (; m->id_next < end && m->id_next < PIANO_MIPPS_ID_SEARCH; m->id_next++)
            if (digest_matches(m, m->id_next)) { m->adapter_id = m->id_next; advance(m); req->wait_until_ms = now_ms; return; }
        if (m->id_next >= PIANO_MIPPS_ID_SEARCH) { m->adapter_id = 0; goto_finish(m, PIANO_MIPPS_E_REJECTED); }
        req->wait_until_ms = now_ms;   /* the next chunk at once */
        return;
    }
    default: return;
    }
    m->awaiting = 1;
}

static int failed_hard(PianoMipps *m)
{
    /* a transport failure: in the main program it ends the run; in the cleanup it is retried (F_CLEAR) or skipped (the rest) */
    if (m->state == PIANO_MIPPS_RUNNING) { m->verified = 0; goto_finish(m, PIANO_MIPPS_E_IO); return 1; }
    return 0;
}

void piano_mipps_reply(PianoMipps *m, uint64_t now_ms, int status, const uint8_t *data, uint32_t len)
{
    if (!m->awaiting) return;   /* nothing was asked */
    m->awaiting = 0;
    m->wait_until_ms = now_ms;
    if (vdm_step(m->step)) {
        if (status) {
            if (!failed_hard(m)) { m->step = F_CLEAR; m->sub = V_WRITE; }   /* the failure branch's command 7: skip to the clear */
            return;
        }
        switch (m->sub) {
        case V_WRITE:
            m->sub = V_SETTLE;
            m->wait_until_ms = now_ms + PIANO_MIPPS_POLL_MS;
            m->deadline_ms = now_ms + PIANO_MIPPS_POLL_MS + vdm_timeout_ms(m->cmd, m->flag);
            return;
        case V_VBUS: {
            const uint32_t uv = (data && len >= 4) ? le32(data) : 0;
            if (uv < PIANO_MIPPS_VBUS_ONLINE_UV) {
                if (m->state == PIANO_MIPPS_RUNNING) { m->verified = 0; goto_finish(m, PIANO_MIPPS_E_GONE); }
                else { m->step = F_CLEAR; m->sub = V_WRITE; }
                return;
            }
            m->sub = V_STATE;
            return;
        }
        case V_STATE: {
            const uint32_t st = (data && len >= 4) ? le32(data) : 0;
            if (st == m->cmd) { advance(m); return; }
            if (now_ms < m->deadline_ms) { m->sub = V_PAUSE; m->wait_until_ms = now_ms + PIANO_MIPPS_POLL_MS; return; }
            if (++m->attempt < 2) { m->sub = V_WRITE; return; }   /* one resend */
            if (m->state == PIANO_MIPPS_RUNNING) { m->verified = 0; goto_finish(m, PIANO_MIPPS_E_TIMEOUT); }
            else { m->step = F_CLEAR; m->sub = V_WRITE; }
            return;
        }
        default: return;
        }
    }
    switch (m->step) {
    case P_VERIFY1: case P_UNVERIFY0: case P_VERIFIED1:
        if (status) { failed_hard(m); return; }
        advance(m);
        return;
    case P_VERSION: {
        if (status || !data || len < 4) { failed_hard(m); return; }
        const uint32_t v = le32(data);
        if (!v || v == 0xffffffffu) { goto_finish(m, PIANO_MIPPS_E_NOADAPTER); return; }
        m->version = v;
        m->discovered = 1;
        advance(m);
        return;
    }
    case P_READ_AUTH:
        if (status || !data || len < 16) { failed_hard(m); return; }
        swap_groups(m->response, data);
        advance(m);
        return;
    case P_READ_ID:
        if (status || !data || len < 4) { failed_hard(m); return; }
        m->adapter_id = le32(data);
        advance(m);
        return;
    case P_VBUS_FINAL: {
        if (status || !data || len < 4) { failed_hard(m); return; }
        if (le32(data) < PIANO_MIPPS_VBUS_ONLINE_UV) { m->verified = 0; goto_finish(m, PIANO_MIPPS_E_GONE); return; }
        advance(m);
        return;
    }
    case F_UNVERIFY0:
        advance(m);   /* best effort: on to command 7 with flag 0 */
        return;
    case F_CLEAR:
        if (status) { m->wait_until_ms = now_ms + 1000u; return; }   /* owed: retried every second while the service is the same */
        advance(m);
        return;
    default: return;
    }
}

int piano_mipps_cleanup_pending(const PianoMipps *m)
{
    return m && (m->state == PIANO_MIPPS_RUNNING || m->state == PIANO_MIPPS_CLEANUP);
}

uint32_t piano_mipps_budget_ma(const PianoMipps *m)
{
    return m && m->state == PIANO_MIPPS_DONE && m->verified && m->error == PIANO_MIPPS_OK ? PIANO_MIPPS_BUDGET_VERIFIED_MA
                                                                                         : PIANO_MIPPS_BUDGET_STANDARD_MA;
}

const char *piano_mipps_error_name(int error)
{
    switch (error) {
    case PIANO_MIPPS_OK: return "verified";
    case PIANO_MIPPS_E_NOKEY: return "no key file";
    case PIANO_MIPPS_E_FORMAT: return "key file malformed";
    case PIANO_MIPPS_E_IO: return "firmware transport failed";
    case PIANO_MIPPS_E_GONE: return "source gone (VBUS, detach or service)";
    case PIANO_MIPPS_E_TIMEOUT: return "command not answered";
    case PIANO_MIPPS_E_NOADAPTER: return "not a MiPPS adapter";
    case PIANO_MIPPS_E_REJECTED: return "answer matches no adapter ID";
    case PIANO_MIPPS_E_BUSY: return "busy";
    default: return "unknown";
    }
}

void piano_mipps_fill_cp_in(const PianoMipps *m, int will_run, struct piano_cp_in *in)
{
    _Static_assert(PIANO_MIPPS_BUDGET_VERIFIED_MA == PIANO_CP_BUDGET_VERIFIED_MA, "MiPPS budgets: ChargePump.h");
    _Static_assert(PIANO_MIPPS_BUDGET_STANDARD_MA == PIANO_CP_BUDGET_STANDARD_MA, "MiPPS budgets: ChargePump.h");
    if (!in) return;
    if (!will_run || !m) { in->mipps_done = 1; in->mipps_verified = 0; in->source_budget_ma = PIANO_CP_BUDGET_STANDARD_MA; return; }
    in->mipps_done = m->state == PIANO_MIPPS_DONE;
    in->mipps_verified = (uint8_t)(m->state == PIANO_MIPPS_DONE && m->verified && m->error == PIANO_MIPPS_OK);
    in->source_budget_ma = piano_mipps_budget_ma(m);
}
