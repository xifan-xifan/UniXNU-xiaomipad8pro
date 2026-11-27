// SPDX-License-Identifier: BSD-2-Clause-Patent
// c++ -std=c++20 -Wall -Wextra -Werror -I ../vt vt_host.cpp -o /tmp/vt_host && /tmp/vt_host
#include "PianoVideoVTWire.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL:%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main()
{
    using namespace vtwire;
    uint8_t b[256] = {};
    Writer w(b, sizeof b);
    uint8_t uuid[16] = {};
    w.header(cc("decs"), FlagRequest, uuid, 9);
    w.s32(cc(",cod"), int32_t(cc("avc1")));
    CHECK(w.ok());
    Header h;
    CHECK(header(b, w.size(), h) && h.type == cc("decs") && h.flags == FlagRequest && h.request == 9);
    int32_t codec = 0;
    CHECK(paramS32(b, w.size(), cc(",cod"), codec));
    CHECK(uint32_t(codec) == cc("avc1"));
    if (failures) return 1;
    std::puts("PianoVideo VT host tests: protocol6 wire pass");
    return 0;
}
