// SPDX-License-Identifier: MIT
#include "AirSignature.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

int main(int argc, char **argv) {
    assert(argc == 2);
    using air_signature::detail::Bits;
    // Width-8 VBR carries seven data bits per byte. The tenth byte can
    // contribute only bit 63, and cannot request another byte.
    uint8_t vbr[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1, 0};
    Bits full(vbr, 10);
    assert(full.vbr(8) == UINT64_MAX && !full.bad());
    Bits cut(vbr, 9);
    assert(cut.vbr(8) == 0 && cut.bad());
    for (uint8_t last : {2, 0x81}) {
        vbr[9] = last;
        Bits overflow(vbr, sizeof vbr);
        assert(overflow.vbr(8) == 0 && overflow.bad());
    }

    Bits empty(nullptr, 0);
    std::string bytes = "previous";
    assert(empty.bytes(0, bytes) && bytes.empty());
    assert(empty.fixed(1) == 0 && empty.bad());

    air_signature::Signature sig;
    std::string why;
    const uint8_t noModule[] = {'B', 'C', 0xc0, 0xde, 0, 0, 0, 0};
    assert(!air_signature::read(nullptr, 20, "test", sig, why) && why == "no bitcode");
    assert(!air_signature::read(noModule, 4, "test", sig, why) && why == "not LLVM bitcode");
    assert(!air_signature::read(noModule, sizeof noModule, "test", sig, why) &&
           why == "no !air.visible entry for the function");
    const uint8_t badWrapper[20] = {0xde, 0xc0, 0x17, 0x0b, 0, 0, 0, 0, 20, 0, 0, 0, 1};
    assert(!air_signature::read(badWrapper, sizeof badWrapper, "test", sig, why) &&
           why == "a bitcode wrapper past the end");
    std::ifstream fixture(argv[1], std::ios::binary);
    const std::vector<uint8_t> bitcode{std::istreambuf_iterator<char>{fixture}, std::istreambuf_iterator<char>{}};
    assert(air_signature::read(bitcode.data(), bitcode.size(), "fixture", sig, why));
    assert(sig.result->kind == air_signature::Type::Void && sig.arguments.empty());
    std::puts("PASS AIR argument-free signature, empty metadata, truncated bitcode and overflowing VBR");
}
