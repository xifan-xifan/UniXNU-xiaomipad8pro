// SPDX-License-Identifier: BSD-2-Clause-Patent
// MiPPS engine (ChargeMipps.c) against a model of the charger firmware and a MiPPS adapter. TEST keys only: never the real ones.
#include "ChargeMipps.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <random>

static int failures = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string hex(const uint8_t *p, size_t n) { std::string s; char b[3]; for (size_t i = 0; i < n; i++) { std::snprintf(b, 3, "%02x", p[i]); s += b; } return s; }

static void test_vectors()
{
    uint8_t d[32];
    piano_mipps_sha256((const uint8_t *)"abc", 3, d);
    CHECK(hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    piano_mipps_sha256(nullptr, 0, d);
    CHECK(hex(d, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
    const char *m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    piano_mipps_sha256((const uint8_t *)m56, std::strlen(m56), d);
    CHECK(hex(d, 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 448 bits");
    uint8_t k1[20]; std::memset(k1, 0x0b, 20);
    piano_mipps_hmac_sha256(k1, 20, (const uint8_t *)"Hi There", 8, d);
    CHECK(hex(d, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "rfc4231 1");
    piano_mipps_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, d);
    CHECK(hex(d, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "rfc4231 2");
    uint8_t k4[25]; for (int i = 0; i < 25; i++) k4[i] = (uint8_t)(i + 1);
    uint8_t m4[50]; std::memset(m4, 0xcd, 50);
    piano_mipps_hmac_sha256(k4, 25, m4, 50, d);
    CHECK(hex(d, 32) == "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", "rfc4231 4");
    uint8_t k6[131]; std::memset(k6, 0xaa, 131);
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    piano_mipps_hmac_sha256(k6, 131, (const uint8_t *)m6, std::strlen(m6), d);
    CHECK(hex(d, 32) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "rfc4231 6");
    const char *m7 = "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed "
                     "before being used by the HMAC algorithm.";
    piano_mipps_hmac_sha256(k6, 131, (const uint8_t *)m7, std::strlen(m7), d);
    CHECK(hex(d, 32) == "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", "rfc4231 7");
}

// ---- TEST key material (made up here; the real table lives only in the private file)
static std::vector<uint8_t> key_file(const std::vector<PianoMippsKey> &keys)
{
    std::vector<uint8_t> f = {'P', 'M', 'I', 'P', 'K', 'E', 'Y', '1'};
    auto le = [&](uint32_t v) { for (int i = 0; i < 4; i++) f.push_back((uint8_t)(v >> (8 * i))); };
    le((uint32_t)keys.size()); le(48);
    for (auto &k : keys) { f.insert(f.end(), k.seed, k.seed + 16); f.insert(f.end(), k.key, k.key + 32); }
    uint8_t d[32]; piano_mipps_sha256(f.data(), f.size(), d);
    f.insert(f.end(), d, d + 32);
    return f;
}
static std::vector<PianoMippsKey> test_keys(uint8_t salt)
{
    std::vector<PianoMippsKey> v(3);
    for (unsigned j = 0; j < 3; j++) {
        for (unsigned i = 0; i < 16; i++) v[j].seed[i] = (uint8_t)(0x10 * j + i + salt);
        for (unsigned i = 0; i < 32; i++) v[j].key[i] = (uint8_t)(0x5a ^ (j * 31 + i * 7 + salt));
    }
    return v;
}

static void swap4(uint8_t out[16], const uint8_t in[16]) { for (int g = 0; g < 4; g++) for (int b = 0; b < 4; b++) out[4 * g + b] = in[4 * g + 3 - b]; }
static uint32_t rd(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

// ---- the firmware + adapter model
struct Model {
    bool mipps = true;
    uint32_t version = 0x00010003;
    uint32_t real_id = 0x00001234, reported_id = 0x00001234;
    std::vector<PianoMippsKey> adapter_keys = test_keys(0);
    uint32_t latency[9] = {0, 30, 30, 60, 90, 300, 40, 40, 40};
    bool never[9] = {};
    int answer_on[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};   // answer from this attempt on
    int attempts[9] = {};
    uint32_t pending = 0, uvdm = 0;
    uint64_t answer_at = 0;
    uint64_t vbus_drop_at = UINT64_MAX;
    uint32_t verify_process = 0, pd_verified = 0;
    std::vector<uint32_t> cmds;
    std::vector<std::pair<uint32_t, uint32_t>> flag_writes;   // (cmd, flag)
    std::vector<std::pair<uint32_t, uint32_t>> u32_writes;    // (prop, value)
    int seed_key = -1;
    uint8_t answer[16] = {};
    bool sent8 = false;
    uint32_t fail_prop = 0; int fail_times = 0;               // transport failures on one property
    int handle(const PianoMippsRequest &r, uint64_t now, uint8_t *out, uint32_t *len)
    {
        *len = 0;
        if (fail_times > 0 && r.prop == fail_prop) { fail_times--; return -1; }
        if (r.kind == PIANO_MIPPS_REQ_WRITE) {
            if (r.prop >= PIANO_MIPPS_PROP_VDM_BASE && r.prop <= PIANO_MIPPS_PROP_VDM_BASE + 7) {
                const uint32_t cmd = r.prop - PIANO_MIPPS_PROP_VDM_BASE + 1;
                cmds.push_back(cmd);
                if (cmd == 8) { sent8 = true; return 0; }   // stalls: never answered
                attempts[cmd]++;
                if (r.len == 4 && (cmd == 6 || cmd == 7)) flag_writes.push_back({cmd, rd(r.data)});
                if (cmd == 4 && r.len == 16) {
                    uint8_t s[16]; swap4(s, r.data); seed_key = -1;
                    for (size_t i = 0; i < adapter_keys.size(); i++) if (!std::memcmp(adapter_keys[i].seed, s, 16)) seed_key = (int)i;
                }
                if (cmd == 5 && r.len == 16) {
                    uint8_t c[16]; swap4(c, r.data);
                    uint8_t msg[20]; std::memcpy(msg, c, 16);
                    msg[16] = (uint8_t)(real_id >> 24); msg[17] = (uint8_t)(real_id >> 16); msg[18] = (uint8_t)(real_id >> 8); msg[19] = (uint8_t)real_id;
                    uint8_t d[32];
                    if (seed_key >= 0) piano_mipps_hmac_sha256(adapter_keys[seed_key].key, 32, msg, 20, d);
                    else { uint8_t other[32]; std::memset(other, 0x77, 32); piano_mipps_hmac_sha256(other, 32, msg, 20, d); }
                    std::memcpy(answer, d, 16);
                }
                if (mipps && !never[cmd] && attempts[cmd] >= answer_on[cmd]) { pending = cmd; answer_at = now + latency[cmd]; }
                return 0;
            }
            if (r.len != 4) return -2;
            u32_writes.push_back({r.prop, rd(r.data)});
            if (r.prop == PIANO_MIPPS_PROP_VERIFY_PROCESS) verify_process = rd(r.data);
            else if (r.prop == PIANO_MIPPS_PROP_PD_VERIFIED) pd_verified = rd(r.data);
            else return -3;
            return 0;
        }
        if (r.kind == PIANO_MIPPS_REQ_READ) {
            switch (r.prop) {
            case PIANO_MIPPS_PROP_VBUS: wr(out, now >= vbus_drop_at ? 0 : 9000000); *len = 4; return 0;
            case PIANO_MIPPS_PROP_UVDM_STATE: if (pending && now >= answer_at) { uvdm = pending; pending = 0; } wr(out, uvdm); *len = 4; return 0;
            case PIANO_MIPPS_PROP_VERSION: wr(out, mipps ? version : 0); *len = 4; return 0;
            case PIANO_MIPPS_PROP_AUTH: swap4(out, answer); *len = 16; return 0;
            case PIANO_MIPPS_PROP_ADAPTER_ID: wr(out, reported_id); *len = 4; return 0;
            default: return -4;
            }
        }
        return -5;
    }
};

struct Run { PianoMipps m; uint64_t end_ms; int requests; };
static Run run(Model &mo, const PianoMippsKeys &keys, uint32_t key_index, uint64_t detach_at = UINT64_MAX,
               uint64_t service_lost_at = UINT64_MAX, uint64_t limit = 120000)
{
    Run r{}; uint8_t ch[16]; for (int i = 0; i < 16; i++) ch[i] = (uint8_t)(0xc0 + i * 3);
    piano_mipps_start(&r.m, &keys, key_index, ch, 0);
    uint64_t now = 0;
    while (r.m.state != PIANO_MIPPS_DONE && now < limit) {
        PianoMippsRequest q;
        piano_mipps_next(&r.m, now, now < detach_at, now < service_lost_at, &q);
        if (q.kind == PIANO_MIPPS_REQ_NONE) {
            if (r.m.state == PIANO_MIPPS_DONE) break;
            now = q.wait_until_ms > now ? q.wait_until_ms : now + 1;
            continue;
        }
        r.requests++;
        uint8_t buf[16] = {}; uint32_t len = 0;
        const int st = mo.handle(q, now, buf, &len);
        now += 1;
        piano_mipps_reply(&r.m, now, st, buf, len);
    }
    r.end_ms = now;
    return r;
}
static PianoMippsKeys parsed(const std::vector<PianoMippsKey> &v)
{
    PianoMippsKeys k; auto f = key_file(v);
    CHECK(piano_mipps_keys_parse(f.data(), f.size(), &k) == PIANO_MIPPS_OK, "parse test keys");
    return k;
}
static bool last_verify_is_zero(const Model &mo)
{
    for (auto it = mo.u32_writes.rbegin(); it != mo.u32_writes.rend(); ++it)
        if (it->first == PIANO_MIPPS_PROP_VERIFY_PROCESS) return it->second == 0;
    return false;
}
static void common_invariants(const Model &mo, const Run &r, const char *what)
{
    CHECK(!mo.sent8 && !r.m.sent8, "%s: command 8 sent", what);
    for (auto c : mo.cmds) CHECK(c >= 1 && c <= 7, "%s: command %u", what, c);
    CHECK(r.m.state == PIANO_MIPPS_DONE, "%s: not done (state %u step %u)", what, r.m.state, r.m.step);
    if (r.m.cleared) CHECK(last_verify_is_zero(mo) && mo.verify_process == 0, "%s: 0x21002 not cleared last", what);
    CHECK(piano_mipps_budget_ma(&r.m) == (r.m.verified ? 6000u : 5000u), "%s: budget", what);
    if (r.m.verified) CHECK(mo.pd_verified == 1 && r.m.error == PIANO_MIPPS_OK, "%s: verified without PD_VERIFIED 1", what);
}

static void test_verified()
{
    Model mo; auto k = parsed(test_keys(0));
    Run r = run(mo, k, 1);
    common_invariants(mo, r, "verified");
    CHECK(r.m.verified && r.m.adapter_id == 0x1234 && r.m.cleared, "verified: v=%u id=%x err=%d", r.m.verified, r.m.adapter_id, r.m.error);
    CHECK(mo.u32_writes.front().first == PIANO_MIPPS_PROP_VERIFY_PROCESS && mo.u32_writes.front().second == 1, "first write 0x21002 = 1");
    const std::vector<uint32_t> want = {1, 2, 3, 4, 5, 7, 6};
    CHECK(mo.cmds == want, "command order");
    CHECK(mo.flag_writes.size() == 2 && mo.flag_writes[0] == std::make_pair(7u, 1u) && mo.flag_writes[1] == std::make_pair(6u, 1u), "7 then 6, flag 1");
    struct piano_cp_in in{}; piano_mipps_fill_cp_in(&r.m, 1, &in);
    CHECK(in.mipps_done == 1 && in.mipps_verified == 1 && in.source_budget_ma == 6000, "cp_in verified");
}
static void test_search()
{
    Model mo; mo.reported_id = 0; mo.real_id = 0x0000beef; auto k = parsed(test_keys(0));
    Run r = run(mo, k, 0);
    common_invariants(mo, r, "search");
    CHECK(r.m.verified && r.m.adapter_id == 0xbeef, "search found id %x", r.m.adapter_id);
    Model mo2; mo2.reported_id = 0x9999; mo2.real_id = 0x0042;   // a stale reported ID
    Run r2 = run(mo2, k, 2);
    common_invariants(mo2, r2, "stale id");
    CHECK(r2.m.verified && r2.m.adapter_id == 0x42, "stale id recovered %x", r2.m.adapter_id);
}
static void test_wrong()
{
    Model mo; mo.adapter_keys = test_keys(0x40);   // the adapter knows other keys: no seed of ours
    auto k = parsed(test_keys(0));
    Run r = run(mo, k, 0);
    common_invariants(mo, r, "wrong");
    CHECK(!r.m.verified && r.m.error == PIANO_MIPPS_E_REJECTED && r.m.cleared, "wrong: err %d", r.m.error);
    CHECK(mo.pd_verified == 0, "wrong: PD_VERIFIED 0");
    CHECK(!mo.flag_writes.empty() && mo.flag_writes.back() == std::make_pair(7u, 0u), "wrong: command 7 flag 0 at the end");
    struct piano_cp_in in{}; piano_mipps_fill_cp_in(&r.m, 1, &in);
    CHECK(in.mipps_done == 1 && in.mipps_verified == 0 && in.source_budget_ma == 5000, "cp_in rejected");
}
static void test_timeouts()
{
    auto k = parsed(test_keys(0));
    for (uint32_t c = 1; c <= 7; c++) {
        Model mo; mo.never[c] = true;
        Run r = run(mo, k, 0);
        char w[32]; std::snprintf(w, sizeof w, "timeout cmd %u", c);
        common_invariants(mo, r, w);
        CHECK(!r.m.verified && r.m.cleared, "%s: verified %u cleared %u", w, r.m.verified, r.m.cleared);
        if (c != 7) CHECK(r.m.error == PIANO_MIPPS_E_TIMEOUT, "%s: err %d", w, r.m.error);
        CHECK(mo.attempts[c] >= 2, "%s: one resend (%d attempts)", w, mo.attempts[c]);
        if (c >= 2) CHECK(mo.pd_verified == 0, "%s: PD_VERIFIED 0", w);
        if (c == 1) CHECK(!r.m.discovered && mo.flag_writes.empty(), "%s: nothing after an undiscovered adapter", w);
    }
}
static void test_retry()
{
    Model mo; mo.answer_on[3] = 2; auto k = parsed(test_keys(0));
    Run r = run(mo, k, 0);
    common_invariants(mo, r, "retry");
    CHECK(r.m.verified && mo.attempts[3] == 2, "retry: verified %u attempts %d", r.m.verified, mo.attempts[3]);
}
static void test_keys_file()
{
    PianoMippsKeys k;
    CHECK(piano_mipps_keys_parse(nullptr, 0, &k) == PIANO_MIPPS_E_NOKEY && k.count == 0, "no file");
    auto f = key_file(test_keys(0));
    auto bad = f; bad[0] ^= 1; CHECK(piano_mipps_keys_parse(bad.data(), bad.size(), &k) == PIANO_MIPPS_E_FORMAT, "magic");
    bad = f; bad[40] ^= 1; CHECK(piano_mipps_keys_parse(bad.data(), bad.size(), &k) == PIANO_MIPPS_E_FORMAT, "sha");
    bad = f; bad.pop_back(); CHECK(piano_mipps_keys_parse(bad.data(), bad.size(), &k) == PIANO_MIPPS_E_FORMAT, "truncated");
    bad = f; bad[8] = 17; CHECK(piano_mipps_keys_parse(bad.data(), bad.size(), &k) == PIANO_MIPPS_E_FORMAT, "count");
    CHECK(piano_mipps_keys_parse(f.data(), f.size(), &k) == PIANO_MIPPS_OK && k.count == 3, "good file");
    piano_mipps_keys_wipe(&k); CHECK(k.count == 0 && k.k[0].key[0] == 0, "wipe");
    // no key: never runs, 5 A
    PianoMippsKeys none{}; const uint32_t pdo[2] = {5000, 9000};
    CHECK(!piano_mipps_eligible(1, &none, 10, pdo, 2, 1, 1, 0), "not eligible without a key");
    PianoMipps m{}; uint8_t ch[16] = {};
    CHECK(piano_mipps_start(&m, &none, 0, ch, 0) == PIANO_MIPPS_E_NOKEY && m.state == PIANO_MIPPS_DONE && piano_mipps_budget_ma(&m) == 5000, "start without key");
    struct piano_cp_in in{}; piano_mipps_fill_cp_in(nullptr, 0, &in);
    CHECK(in.mipps_done == 1 && in.mipps_verified == 0 && in.source_budget_ma == 5000, "cp_in not eligible");
    auto kk = parsed(test_keys(0));
    CHECK(piano_mipps_eligible(1, &kk, 10, pdo, 2, 1, 1, 0), "eligible PD 9V");
    const uint32_t pdo5[1] = {5000};
    CHECK(!piano_mipps_eligible(1, &kk, 10, pdo5, 1, 1, 1, 0), "5 V only: not eligible");
    CHECK(!piano_mipps_eligible(1, &kk, 4, pdo, 2, 1, 1, 0), "not a PD type");
    CHECK(!piano_mipps_eligible(1, &kk, 10, pdo, 2, 0, 1, 0), "after a voltage step");
    CHECK(!piano_mipps_eligible(1, &kk, 10, pdo, 2, 1, 0, 0), "with the pump on");
    CHECK(!piano_mipps_eligible(1, &kk, 10, pdo, 2, 1, 1, 1), "tried on this attachment");
    CHECK(!piano_mipps_eligible(0, &kk, 10, pdo, 2, 1, 1, 0), "MiPPS off");
}
static void test_gone()
{
    auto k = parsed(test_keys(0));
    Model mo; mo.vbus_drop_at = 400;   // VBUS lost during command 4/5
    Run r = run(mo, k, 0);
    common_invariants(mo, r, "vbus drop");
    CHECK(!r.m.verified && r.m.error == PIANO_MIPPS_E_GONE && r.m.cleared, "vbus drop: err %d cleared %u", r.m.error, r.m.cleared);
    Model mo2; Run r2 = run(mo2, k, 0, 200);   // detach
    common_invariants(mo2, r2, "detach");
    CHECK(!r2.m.verified && r2.m.error == PIANO_MIPPS_E_GONE && r2.m.cleared, "detach: err %d", r2.m.error);
    Model mo3; Run r3 = run(mo3, k, 0, UINT64_MAX, 250);   // a new firmware service: nothing more written, not owed
    CHECK(!r3.m.verified && r3.m.error == PIANO_MIPPS_E_GONE && !piano_mipps_cleanup_pending(&r3.m) && !mo3.sent8, "service lost");
    // detach right after PD_VERIFIED 1 (the confirm step, as the stock order checks the attachment after that write)
    Model mo4; PianoMipps m{}; uint8_t ch[16] = {7};
    piano_mipps_start(&m, &k, 0, ch, 0);
    uint64_t now = 0; bool attached = true;
    while (m.state != PIANO_MIPPS_DONE && now < 120000) {
        PianoMippsRequest q; piano_mipps_next(&m, now, attached, 1, &q);
        if (q.kind == PIANO_MIPPS_REQ_NONE) { now = q.wait_until_ms > now ? q.wait_until_ms : now + 1; continue; }
        uint8_t buf[16] = {}; uint32_t len = 0;
        const int st = mo4.handle(q, now, buf, &len);
        if (q.kind == PIANO_MIPPS_REQ_WRITE && q.prop == PIANO_MIPPS_PROP_PD_VERIFIED && rd(q.data) == 1) attached = false;
        now += 1;
        piano_mipps_reply(&m, now, st, buf, len);
    }
    CHECK(m.state == PIANO_MIPPS_DONE && !m.verified && m.error == PIANO_MIPPS_E_GONE && mo4.pd_verified == 0 && m.cleared,
          "detach after PD_VERIFIED 1: verified %u err %d pd_verified %u", m.verified, m.error, mo4.pd_verified);
}
static void test_noadapter()
{
    auto k = parsed(test_keys(0));
    Model mo; mo.version = 0;
    Run r = run(mo, k, 0);
    common_invariants(mo, r, "version 0");
    CHECK(r.m.error == PIANO_MIPPS_E_NOADAPTER && !r.m.discovered && mo.flag_writes.empty() && r.m.cleared, "version 0");
    Model mo2; mo2.version = 0xffffffffu;
    Run r2 = run(mo2, k, 0);
    CHECK(r2.m.error == PIANO_MIPPS_E_NOADAPTER && r2.m.cleared, "version all ones");
}
static void test_transport()
{
    auto k = parsed(test_keys(0));
    Model mo; mo.fail_prop = PIANO_MIPPS_PROP_AUTH; mo.fail_times = 1;
    Run r = run(mo, k, 0);
    common_invariants(mo, r, "read failure");
    CHECK(r.m.error == PIANO_MIPPS_E_IO && !r.m.verified && r.m.cleared, "read failure: err %d", r.m.error);
    // the clear itself fails twice: owed, retried, then acknowledged; no voltage step meanwhile
    Model mo2; mo2.never[2] = true;   // a failing run, then the clear fails
    PianoMipps m{}; uint8_t ch[16] = {1};
    piano_mipps_start(&m, &k, 0, ch, 0);
    uint64_t now = 0; int clear_fail = 2; int pending_seen = 0;
    while (m.state != PIANO_MIPPS_DONE && now < 200000) {
        PianoMippsRequest q; piano_mipps_next(&m, now, 1, 1, &q);
        if (q.kind == PIANO_MIPPS_REQ_NONE) { now = q.wait_until_ms > now ? q.wait_until_ms : now + 1; continue; }
        uint8_t buf[16] = {}; uint32_t len = 0; int st;
        if (q.kind == PIANO_MIPPS_REQ_WRITE && q.prop == PIANO_MIPPS_PROP_VERIFY_PROCESS && rd(q.data) == 0 && clear_fail > 0) { clear_fail--; st = -1; }
        else st = mo2.handle(q, now, buf, &len);
        now += 1;
        piano_mipps_reply(&m, now, st, buf, len);
        if (piano_mipps_cleanup_pending(&m)) pending_seen++;
    }
    CHECK(m.state == PIANO_MIPPS_DONE && m.cleared && clear_fail == 0 && mo2.verify_process == 0 && pending_seen > 0, "clear retried until acknowledged");
}
static void test_fuzz()
{
    auto k = parsed(test_keys(0));
    std::mt19937 rng(20261123);
    int verified = 0;
    for (int i = 0; i < 400; i++) {
        Model mo;
        static const uint32_t dl[8] = {0, 500, 500, 1000, 1500, 2500, 1000, 1000};
        for (int c = 1; c <= 7; c++) { mo.latency[c] = rng() % (dl[c] + dl[c] / 4); mo.never[c] = (rng() % 20) == 0; mo.answer_on[c] = 1 + (rng() % 3 == 0); }
        if (rng() % 5 == 0) mo.adapter_keys = test_keys(0x40);
        if (rng() % 4 == 0) mo.reported_id = 0;
        mo.real_id = rng() % 70000;   // sometimes beyond the 65536 search
        if (rng() % 6 == 0) { mo.fail_prop = 0x21003 + rng() % 8; mo.fail_times = 1 + rng() % 2; }
        if (rng() % 7 == 0) mo.vbus_drop_at = rng() % 8000;
        const uint64_t detach = rng() % 9 == 0 ? rng() % 9000 : UINT64_MAX;
        Run r = run(mo, k, rng() % 3, detach);
        common_invariants(mo, r, "fuzz");
        // never verified unless the adapter really holds our key and its answer was for an ID we can reach
        const bool possible = mo.adapter_keys[0].seed[0] == k.k[0].seed[0] && (mo.real_id < 65536 || mo.real_id == mo.reported_id);
        if (r.m.verified) { verified++; CHECK(possible, "fuzz: verified though impossible (id %x)", mo.real_id); }
        CHECK(r.m.cleared || mo.verify_process == 0, "fuzz: 0x21002 left set");
    }
    std::printf("fuzz: 400 runs, %d verified\n", verified);
    CHECK(verified >= 10, "fuzz: the known positive (verified runs) %d", verified);
}

int main()
{
    test_vectors();
    test_keys_file();
    test_verified();
    test_search();
    test_wrong();
    test_timeouts();
    test_retry();
    test_gone();
    test_noadapter();
    test_transport();
    test_fuzz();
    std::printf("mipps_host: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
