// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang++ -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I.. -x c++ mipps_mca_host.cpp -x c ../ChargeMipps.c ../PianoMca.c -o /tmp/mipps_mca_host && /tmp/mipps_mca_host
// MiPPS over the MCA codec: PianoMca's auxiliary request channel (its allowlist, encoding, answers, timeout, interleaving with the
// cycle) and the MiPPS engine (ChargeMipps.c) driven end to end through it against a firmware + adapter model. TEST keys only.
#include "ChargeMipps.h"
#include "PianoMca.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static uint32_t rd(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

// one answer of the firmware to request REQ (property, seq, opcode from it), retcode and payload
static std::vector<uint8_t> answer(const uint8_t *req, uint32_t retcode, const uint8_t *payload, uint32_t n)
{
    std::vector<uint8_t> a(PIANO_MCA_RESPONSE_BYTES, 0);
    wr(&a[0], PIANO_MCA_OWNER); wr(&a[4], PIANO_MCA_TYPE_REQUEST); wr(&a[8], rd(req + 8));
    wr(&a[12], rd(req + 12)); wr(&a[16], retcode); wr(&a[20], rd(req + 16));
    if (payload) std::memcpy(&a[24], payload, n);
    return a;
}

static void test_allowlist()
{
    uint8_t one[4] = {1, 0, 0, 0}, two[4] = {2, 0, 0, 0}, z16[16] = {};
    CHECK(piano_mca_aux_allowed(1, 0x21002, one, 4) && !piano_mca_aux_allowed(1, 0x21002, two, 4), "VERIFY_PROCESS 0/1 only");
    CHECK(!piano_mca_aux_allowed(1, 0x2100a, one, 4) && !piano_mca_aux_allowed(1, 0x2100a, z16, 16), "command 8 never");
    CHECK(piano_mca_aux_allowed(1, 0x21006, z16, 16) && !piano_mca_aux_allowed(1, 0x21006, one, 4), "seed 16 bytes");
    CHECK(piano_mca_aux_allowed(1, 0x21008, one, 4) && !piano_mca_aux_allowed(1, 0x21008, two, 4), "command 6 flag 0/1");
    CHECK(!piano_mca_aux_allowed(1, 0x2008, one, 4) && !piano_mca_aux_allowed(1, 0x21018, one, 4), "no charger limit or voltage");
    CHECK(piano_mca_aux_allowed(0, 0x21007, nullptr, 16) && !piano_mca_aux_allowed(0, 0x21007, nullptr, 4), "answer read 16");
    CHECK(!piano_mca_aux_allowed(0, 0x21011, nullptr, 4) && piano_mca_aux_allowed(0, 0x21012, nullptr, 4), "reads list");
}

static void test_channel()
{
    PianoMca m; piano_mca_init(&m, 1, 1); PianoMcaAux aux;
    uint8_t one[4] = {1, 0, 0, 0};
    CHECK(piano_mca_aux_request(&m, 1, 0x21002, one, 4) == -1, "refused without allow");
    piano_mca_allow_mipps(&m, &aux);
    uint8_t seed[16]; for (int i = 0; i < 16; i++) seed[i] = (uint8_t)(0x30 + i);
    CHECK(piano_mca_aux_request(&m, 1, 0x21006, seed, 16) == 0, "queued");
    CHECK(piano_mca_aux_request(&m, 1, 0x21002, one, 4) == -1, "one at a time");
    uint8_t out[PIANO_MCA_REQUEST_BYTES];
    CHECK(piano_mca_next(&m, out) == PIANO_MCA_REQUEST_BYTES, "sent");
    CHECK(rd(out) == PIANO_MCA_OWNER && rd(out + 8) == PIANO_MCA_OP_WRITE && rd(out + 12) == 0x21006 && !std::memcmp(out + 20, seed, 16), "encoding");
    CHECK(piano_mca_next(&m, out) == 0, "nothing while in flight");
    auto a = answer(out, 0, nullptr, 0);
    CHECK(piano_mca_receive(&m, a.data(), (uint32_t)a.size()) == McaAuxDone, "aux answer");
    int32_t rc = 99; uint8_t data[16]; uint32_t len = 99;
    CHECK(piano_mca_aux_take(&m, &rc, data, &len) == 1 && rc == 0 && len == 0, "write done");
    CHECK(piano_mca_aux_take(&m, &rc, data, &len) == 0, "taken once");
    // a 16-byte read
    CHECK(piano_mca_aux_request(&m, 0, 0x21007, nullptr, 16) == 0, "read queued");
    piano_mca_next(&m, out);
    CHECK(rd(out + 8) == PIANO_MCA_OP_READ && rd(out + 12) == 0x21007, "read encoding");
    uint8_t ans[16]; for (int i = 0; i < 16; i++) ans[i] = (uint8_t)(0xa0 ^ i);
    a = answer(out, 0, ans, 16);
    piano_mca_receive(&m, a.data(), (uint32_t)a.size());
    CHECK(piano_mca_aux_take(&m, &rc, data, &len) == 1 && rc == 0 && len == 16 && !std::memcmp(data, ans, 16), "read data");
    // the firmware's error code and a timeout
    piano_mca_aux_request(&m, 0, 0x21012, nullptr, 4); piano_mca_next(&m, out);
    a = answer(out, 5, nullptr, 0); piano_mca_receive(&m, a.data(), (uint32_t)a.size());
    CHECK(piano_mca_aux_take(&m, &rc, data, &len) == 1 && rc == 5, "error code");
    piano_mca_aux_request(&m, 0, 0x21012, nullptr, 4); piano_mca_next(&m, out); piano_mca_timeout(&m);
    CHECK(piano_mca_aux_take(&m, &rc, data, &len) == 1 && rc == -60, "timeout");
    // a stale answer after the timeout is ignored, the channel free again
    a = answer(out, 0, nullptr, 0);
    CHECK(piano_mca_receive(&m, a.data(), (uint32_t)a.size()) == McaIgnored, "late aux answer ignored");
    CHECK(piano_mca_aux_request(&m, 0, 0x21012, nullptr, 4) == 0, "free after timeout");
    piano_mca_allow_mipps(&m, nullptr);
    CHECK(piano_mca_aux_request(&m, 0, 0x21012, nullptr, 4) == -1, "refused once withdrawn");
    CHECK(piano_mca_next(&m, out) != PIANO_MCA_REQUEST_BYTES || rd(out + 12) != 0x21012, "withdrawn when disallowed");
}

// the firmware + adapter model at the MCA level (a subset of mipps_host.cpp's)
struct Fw {
    uint8_t key[32], seed[16];
    uint32_t real_id = 0x2468, pending = 0, uvdm = 0, verify = 0, pdv = 0; uint64_t at = 0;
    uint8_t ans[16] = {}; bool sent8 = false;
    std::vector<uint8_t> handle(const uint8_t *req, uint64_t now)
    {
        const uint32_t op = rd(req + 8), prop = rd(req + 12);
        uint8_t pl[16] = {};
        if (op == PIANO_MCA_OP_WRITE) {
            if (prop >= 0x21003 && prop <= 0x2100a) {
                const uint32_t cmd = prop - 0x21003 + 1;
                if (cmd == 8) sent8 = true;
                if (cmd == 5) {   // challenge: groups byte-swapped on the wire
                    uint8_t c[16], msg[20], d[32];
                    for (int g = 0; g < 4; g++) for (int b = 0; b < 4; b++) c[4 * g + b] = req[20 + 4 * g + 3 - b];
                    std::memcpy(msg, c, 16); msg[16] = (uint8_t)(real_id >> 24); msg[17] = (uint8_t)(real_id >> 16); msg[18] = (uint8_t)(real_id >> 8); msg[19] = (uint8_t)real_id;
                    piano_mipps_hmac_sha256(key, 32, msg, 20, d);
                    std::memcpy(ans, d, 16);
                }
                pending = cmd; at = now + 20;
            } else if (prop == 0x21002) verify = rd(req + 20);
            else if (prop == 0x21010) pdv = rd(req + 20);
            return answer(req, 0, nullptr, 0);
        }
        switch (prop) {
        case 0x20002: wr(pl, 9000000); break;
        case 0x21012: if (pending && now >= at) { uvdm = pending; pending = 0; } wr(pl, uvdm); break;
        case 0x21003: wr(pl, 0x10003); break;
        case 0x21007: for (int g = 0; g < 4; g++) for (int b = 0; b < 4; b++) pl[4 * g + b] = ans[4 * g + 3 - b]; return answer(req, 0, pl, 16);
        case 0x2100e: wr(pl, real_id); break;
        default: return answer(req, 1, nullptr, 0);
        }
        return answer(req, 0, pl, 4);
    }
};

static void test_end_to_end()
{
    PianoMippsKeys keys{}; keys.count = 1;
    for (int i = 0; i < 16; i++) keys.k[0].seed[i] = (uint8_t)(i * 5 + 1);
    for (int i = 0; i < 32; i++) keys.k[0].key[i] = (uint8_t)(0x99 ^ (i * 3));
    Fw fw; std::memcpy(fw.key, keys.k[0].key, 32); std::memcpy(fw.seed, keys.k[0].seed, 16);
    PianoMca mca; piano_mca_init(&mca, 1, 1); PianoMcaAux aux; piano_mca_allow_mipps(&mca, &aux);
    PianoMipps e{}; uint8_t ch[16]; for (int i = 0; i < 16; i++) ch[i] = (uint8_t)(0x40 + i);
    piano_mipps_start(&e, &keys, 0, ch, 0);
    uint64_t now = 0; int guard = 0;
    while (e.state != PIANO_MIPPS_DONE && now < 60000 && guard++ < 200000) {
        PianoMippsRequest q; piano_mipps_next(&e, now, 1, 1, &q);
        if (q.kind == PIANO_MIPPS_REQ_NONE) { now = q.wait_until_ms > now ? q.wait_until_ms : now + 1; continue; }
        if (piano_mca_aux_request(&mca, q.kind == PIANO_MIPPS_REQ_WRITE, q.prop, q.data, q.len) != 0) {
            piano_mipps_reply(&e, now, -1, nullptr, 0); continue;   // refused by the allowlist: a transport failure for the engine
        }
        uint8_t out[PIANO_MCA_REQUEST_BYTES];
        CHECK(piano_mca_next(&mca, out) == PIANO_MCA_REQUEST_BYTES, "sent");
        auto a = fw.handle(out, now);
        now += 2;
        piano_mca_receive(&mca, a.data(), (uint32_t)a.size());
        int32_t rc; uint8_t data[16]; uint32_t len;
        CHECK(piano_mca_aux_take(&mca, &rc, data, &len) == 1, "answer taken");
        piano_mipps_reply(&e, now, rc, data, len);
    }
    CHECK(e.state == PIANO_MIPPS_DONE && e.verified && e.adapter_id == 0x2468 && e.cleared, "end to end: verified %u id %x err %d", e.verified, e.adapter_id, e.error);
    CHECK(fw.verify == 0 && fw.pdv == 1 && !fw.sent8, "firmware state after: verify %u pdv %u", fw.verify, fw.pdv);
    CHECK(piano_mipps_budget_ma(&e) == 6000, "budget 6 A");
    std::printf("end to end: %llu ms of firmware time, %u aux requests\n", (unsigned long long)now, aux.sent);
}

int main()
{
    test_allowlist();
    test_channel();
    test_end_to_end();
    std::printf("mipps_mca_host: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
