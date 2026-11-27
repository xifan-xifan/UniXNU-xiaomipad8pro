/* SPDX-License-Identifier: BSD-2-Clause-Patent */
// c++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -x c++ persist_ext4_ro_host.cpp ../SensorsPack.c -o /tmp/persist-ext4-ro-host && /tmp/persist-ext4-ro-host
#include "../PersistExt4Verify.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace persist_ext4;
using Bytes = std::vector<uint8_t>;

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); std::abort(); } } while (0)

static void put(Bytes &b, size_t offset, unsigned width, uint64_t value)
{
    CHECK(offset <= b.size() && width <= b.size() - offset);
    for (unsigned i = 0; i < width; ++i) {
        b[offset + i] = uint8_t(value);
        value >>= 8;
    }
    CHECK(!value);
}

static Bytes literal(std::string_view s) { return Bytes(s.begin(), s.end()); }

constexpr char PackPrefix[] = "/mnt/vendor/persist";
constexpr char Json[] = "/sensors/registry/registry/sample.json";
constexpr char Text[] = "/sensors/registry/registry/nested/text.txt";
constexpr char Sparse[] = "/sensors/registry/registry/nested/sparse.bin";
constexpr char Empty[] = "/sensors/registry/registry/nested/empty";
constexpr char Version[] = "/sensors/registry/sns_reg_version";

struct Fixture {
    static constexpr unsigned GroupBlocks = 128, GroupInodes = 32;
    static constexpr unsigned Root = 2, Sensors = 11, Registry = 12, Inner = 13, Nested = 14;
    static constexpr unsigned VersionInode = 15, EmptyInode = 16, SparseInode = 17, TextInode = 18, Other = 19;
    static constexpr unsigned JsonInode = 35; // group 1
    static constexpr unsigned LeftLeaf = 40, RightLeaf = 41;
    static constexpr unsigned FirstTable = 5, SecondTable = 135;
    static constexpr unsigned NestedBlock = 24, InnerBlock = 26;
    unsigned blockSize, first, descSize, inodeSize;
    bool fileType;
    Bytes image;
    std::map<std::string, Bytes> files;

    Fixture(unsigned shift = 0, bool wide = false, unsigned depth = 1, bool legacy = false)
        : blockSize(1024u << shift), first(shift == 0 ? 1u : 0u), descSize(wide ? 64 : 32),
          inodeSize(wide ? 256 : 128), fileType(!legacy), image((2 * GroupBlocks + first) * blockSize)
    {
        setSb(0x00, 4, 2 * GroupInodes);
        setSb(0x04, 4, 2 * GroupBlocks + first);
        setSb(0x14, 4, first);
        setSb(0x18, 4, shift);
        setSb(0x1c, 4, shift);
        setSb(0x20, 4, GroupBlocks);
        setSb(0x24, 4, GroupBlocks);
        setSb(0x28, 4, GroupInodes);
        setSb(0x38, 2, 0xef53);
        setSb(0x3a, 2, 1);
        setSb(0x4c, 4, legacy ? 0 : 1);
        setSb(0x58, 2, inodeSize);
        setSb(0x5c, 4, legacy ? 0 : 0x2c); // clean journal, xattrs, directory indexing
        setSb(0x60, 4, (legacy ? 0 : 0x42) | (wide ? 0x280 : 0));
        setSb(0xfe, 2, descSize);
        put(image, group(0) + 8, 4, FirstTable);
        put(image, group(1) + 8, 4, SecondTable);

        directory(Root, 20, {{Sensors, "sensors", 2}, {Other, "direct.bin", 1}});
        directory(Sensors, 21, {{Registry, "registry", 2}});
        directory(Registry, 22, {{Inner, "registry", 2}, {VersionInode, "sns_reg_version", 1}});
        if (legacy) {
            directory(Inner, 23, {{Nested, "nested", 2}, {JsonInode, "sample.json", 1}});
        } else {
            // An htree root and an interior node are skipped by linear enumeration.
            makeInode(Inner, 0x4000, 3 * blockSize, 0x81000);
            header(inodeMap(Inner), 2, 4, 0);
            leaf(inodeMap(Inner), 0, 0, 1, 23);
            leaf(inodeMap(Inner), 1, 1, 2, 25);
            entries(23, {{Inner, ".", 2}, {Registry, "..", 2}});
            entries(25, {{0, "", 0}});
            entries(InnerBlock, {{0, "deleted", 1}, {Nested, "nested", 2}, {JsonInode, "sample.json", 1}}, true);
        }
        directory(Nested, NestedBlock, {{TextInode, "text.txt", 1}, {SparseInode, "sparse.bin", 1}, {EmptyInode, "empty", 1}});
        regular(VersionInode, 31, Version, "1\n");
        regular(JsonInode, 32, Json, "{\"gain\":7,\"offset\":-2}\n");
        regular(TextInode, 33, Text, "synthetic registry text\n");
        regular(EmptyInode, 0, Empty, "");
        makeInode(Other, 0x8000, blockSize + 1, 0);
        put(image, inodeMap(Other), 4, 39);
        image[39 * blockSize] = 0x93;
        if (legacy) {
            regular(SparseInode, 38, Sparse, "legacy registry fixture");
        } else {
            makeSparse(depth);
        }
    }

    size_t group(unsigned number) const { return (first + 1) * blockSize + number * descSize; }
    size_t inodeOffset(unsigned number) const
    {
        const unsigned index = number - 1;
        return (index < GroupInodes ? FirstTable : SecondTable) * blockSize + index % GroupInodes * inodeSize;
    }
    size_t inodeMap(unsigned number) const { return inodeOffset(number) + 0x28; }
    void setSb(size_t offset, unsigned width, uint64_t value) { put(image, 1024 + offset, width, value); }

    void header(size_t node, unsigned count, unsigned maximum, unsigned depth)
    {
        put(image, node, 2, 0xf30a);
        put(image, node + 2, 2, count);
        put(image, node + 4, 2, maximum);
        put(image, node + 6, 2, depth);
    }

    void leaf(size_t node, unsigned slot, uint32_t logical, unsigned length, uint64_t disk)
    {
        const size_t at = node + 12 + slot * 12;
        put(image, at, 4, logical);
        put(image, at + 4, 2, length);
        put(image, at + 6, 2, disk >> 32);
        put(image, at + 8, 4, uint32_t(disk));
    }

    void index(size_t node, unsigned slot, uint32_t logical, uint64_t disk)
    {
        const size_t at = node + 12 + slot * 12;
        put(image, at, 4, logical);
        put(image, at + 4, 4, uint32_t(disk));
        put(image, at + 8, 2, disk >> 32);
    }

    void makeInode(unsigned number, unsigned mode, uint64_t size, unsigned flags)
    {
        const size_t at = inodeOffset(number);
        put(image, at, 2, mode | 0644);
        put(image, at + 4, 4, uint32_t(size));
        put(image, at + 0x1a, 2, 1);
        put(image, at + 0x20, 4, flags);
        put(image, at + 0x6c, 4, size >> 32);
    }

    struct Entry { unsigned inode; std::string_view name; unsigned type; };

    void entries(unsigned disk, std::initializer_list<Entry> list, bool tail = false)
    {
        size_t at = disk * blockSize;
        const size_t end = at + blockSize - (tail ? 12 : 0);
        for (const Entry &e : list) {
            const size_t rec = &e == list.end() - 1 ? end - at : (8 + e.name.size() + 3) & ~size_t{3};
            CHECK(rec >= 8 + e.name.size() && at + rec <= end);
            put(image, at, 4, e.inode);
            put(image, at + 4, 2, rec);
            put(image, at + 6, 1, e.name.size());
            put(image, at + 7, 1, fileType ? e.type : 0);
            std::copy(e.name.begin(), e.name.end(), image.begin() + at + 8);
            at += rec;
        }
        if (tail) {
            put(image, end + 4, 2, 12);
            put(image, end + 7, 1, 0xde);
        }
    }

    void single(unsigned number, unsigned mode, unsigned disk, size_t size)
    {
        makeInode(number, mode, size, fileType ? 0x80000 : 0);
        if (fileType) {
            header(inodeMap(number), disk ? 1 : 0, 4, 0);
            if (disk) leaf(inodeMap(number), 0, 0, 1, disk);
        } else {
            put(image, inodeMap(number), 4, disk);
        }
    }

    void directory(unsigned number, unsigned disk, std::initializer_list<Entry> list)
    {
        single(number, 0x4000, disk, blockSize);
        entries(disk, list);
    }

    void regular(unsigned number, unsigned disk, const char *path, std::string_view value)
    {
        files[path] = literal(value);
        single(number, 0x8000, disk, value.size());
        std::copy(value.begin(), value.end(), image.begin() + disk * blockSize);
    }

    void makeSparse(unsigned depth)
    {
        Bytes data(5 * blockSize + 19);
        for (unsigned i = 0; i < blockSize; ++i) data[i] = uint8_t(i % 251);
        std::fill(data.begin() + 4 * blockSize, data.end(), 0x6a);
        files[Sparse] = data;
        std::copy_n(data.begin(), blockSize, image.begin() + 34 * blockSize);
        std::copy(data.begin() + 4 * blockSize, data.end(), image.begin() + 36 * blockSize);
        std::fill_n(image.begin() + 35 * blockSize, blockSize, 0xcc); // unwritten extent must hide old bytes
        makeInode(SparseInode, 0x8000, data.size(), 0x80000);
        size_t node = inodeMap(SparseInode);
        unsigned capacity = 4;
        const unsigned blockCapacity = (blockSize - 12) / 12;
        for (unsigned d = depth; d > 1; --d) {
            const unsigned branch = 42 + d - 2;
            header(node, 1, capacity, d);
            index(node, 0, 0, branch);
            node = branch * blockSize;
            capacity = blockCapacity;
        }
        if (depth) {
            header(node, 2, capacity, 1);
            index(node, 0, 0, LeftLeaf);
            index(node, 1, 4, RightLeaf);
            node = LeftLeaf * blockSize;
            header(node, 2, blockCapacity, 0);
            header(RightLeaf * blockSize, 1, blockCapacity, 0);
            leaf(RightLeaf * blockSize, 0, 4, 2, 36);
        } else {
            header(node, 3, capacity, 0);
            leaf(node, 2, 4, 2, 36);
        }
        leaf(node, 0, 0, 1, 34);
        leaf(node, 1, 2, 32769, 35);
    }
};

struct Source {
    const Bytes &bytes;
    uint64_t limit;
    uint64_t failAt = UINT64_MAX;
    uint64_t highBase = 0;
    mutable size_t calls = 0;

    static bool read(const void *context, uint64_t offset, size_t len, void *out)
    {
        const auto &s = *static_cast<const Source *>(context);
        ++s.calls;
        CHECK(offset <= s.limit && len <= s.limit - offset);
        CHECK(len && len <= MaxBlock);
        if (offset <= s.failAt && s.failAt - offset < len) return false;
        if (offset >= s.highBase) offset -= s.highBase;
        if (offset > s.bytes.size() || len > s.bytes.size() - offset) return false;
        std::memcpy(out, s.bytes.data() + offset, len);
        return true;
    }
};

static Result lookup(Reader &reader, std::string_view path, Inode &inode)
{
    return reader.lookup(path.data(), path.size(), inode);
}

// Independently encode the documented SSCPACK1 wire format from the expected file bytes.
static Bytes makePack(const std::map<std::string, Bytes> &files)
{
    const Bytes unrelated = literal("unrelated to persist");
    std::map<std::string, const Bytes *> entries = {{"/", nullptr}, {"/vendor/synthetic", &unrelated}};
    for (const auto &file : files) entries[PackPrefix + file.first] = &file.second;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        std::string path = it->first;
        for (size_t slash; (slash = path.rfind('/')) > 0;) {
            path.resize(slash);
            entries.emplace(path, nullptr);
        }
    }

    Bytes pack(SSP_HEADER + entries.size() * SSP_ENTRY);
    const size_t names = pack.size();
    size_t at = SSP_HEADER;
    for (const auto &entry : entries) {
        put(pack, at + 16, 4, pack.size() - names);
        pack.insert(pack.end(), entry.first.begin(), entry.first.end());
        pack.push_back(0);
        at += SSP_ENTRY;
    }
    const size_t data = pack.size();
    at = SSP_HEADER;
    for (const auto &entry : entries) {
        if (entry.second) {
            put(pack, at, 8, pack.size());
            put(pack, at + 8, 8, entry.second->size());
            pack.insert(pack.end(), entry.second->begin(), entry.second->end());
        } else {
            put(pack, at + 20, 4, SSP_DIRECTORY);
        }
        at += SSP_ENTRY;
    }
    std::memcpy(pack.data(), "SSCPACK1", 8);
    put(pack, 8, 4, entries.size());
    put(pack, 12, 4, data - names);
    put(pack, 16, 8, pack.size());
    uint32_t sum = 0;
    for (size_t i = SSP_HEADER; i < data; ++i) sum += pack[i];
    put(pack, 24, 4, sum);
    return pack;
}

static Equivalence verify(Reader &reader, const Bytes &pack)
{
    return verifyRegistry(reader, pack.data(), uint32_t(pack.size()));
}

static void extraction(Fixture f)
{
    const Bytes original = f.image;
    Source src{f.image, f.image.size()};
    Reader reader;
    Inode node;
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, "/", node) == Result::Ok && node.number() == 2 && node.directory());
    CHECK(reader.read(node, 0, 0, nullptr) == Result::BadArgument);
    for (const auto &file : f.files) {
        CHECK(lookup(reader, file.first, node) == Result::Ok);
        CHECK(!node.directory() && node.size() == file.second.size());
        Bytes actual(file.second.size(), 0xda);
        CHECK(reader.read(node, 0, actual.size(), actual.data()) == Result::Ok);
        CHECK(actual == file.second);
        CHECK(reader.read(node, node.size(), 0, nullptr) == Result::Ok);
        CHECK(reader.read(node, node.size(), 1, actual.data()) == Result::BadArgument);
        CHECK(reader.read(node, UINT64_MAX, 1, actual.data()) == Result::BadArgument);
        CHECK(reader.read(node, 0, SIZE_MAX, actual.data()) == Result::BadArgument);
        if (actual.size() > f.blockSize) {
            uint8_t split[8];
            CHECK(reader.read(node, f.blockSize - 3, sizeof split, split) == Result::Ok);
            CHECK(std::equal(split, split + sizeof split, actual.data() + f.blockSize - 3));
        }
    }
    CHECK(lookup(reader, "/sensors/registry/registry/absent", node) == Result::NotFound);
    CHECK(lookup(reader, "/sensors/registry/registry/Sample.json", node) == Result::NotFound);
    CHECK(lookup(reader, "/direct.bin/child", node) == Result::NotFound);
    for (const std::string_view path : {"", "sensors", "//sensors", "/sensors/", "/sensors//registry", "/sensors/.", "/sensors/../x"})
        CHECK(lookup(reader, path, node) == Result::BadArgument);
    CHECK(lookup(reader, std::string_view("/sen\0sors", 9), node) == Result::BadArgument);
    CHECK(lookup(reader, std::string(MaxPath + 1, '/'), node) == Result::Limit);
    CHECK(lookup(reader, "/" + std::string(MaxName + 1, 'a'), node) == Result::BadArgument);
    CHECK(verify(reader, makePack(f.files)) == Equivalence::Match);
    CHECK(f.image == original);
}

static void indirect()
{
    Fixture f;
    const uint64_t fanout = f.blockSize / 4;
    const uint64_t single = 12, twice = single + fanout, triple = twice + fanout * fanout;
    f.makeInode(Fixture::Other, 0x8000, (triple + 2) * f.blockSize, 0);
    const size_t map = f.inodeMap(Fixture::Other);
    put(f.image, map + 12 * 4, 4, 50);
    put(f.image, map + 13 * 4, 4, 51);
    put(f.image, map + 14 * 4, 4, 52);
    put(f.image, 50 * f.blockSize, 4, 60);
    put(f.image, 51 * f.blockSize, 4, 53);
    put(f.image, 52 * f.blockSize, 4, 54);
    put(f.image, 53 * f.blockSize, 4, 61);
    put(f.image, 54 * f.blockSize, 4, 55);
    put(f.image, 55 * f.blockSize, 4, 62);
    f.image[60 * f.blockSize] = 0x12;
    f.image[61 * f.blockSize] = 0x34;
    f.image[62 * f.blockSize] = 0x56;

    Source src{f.image, f.image.size()};
    Reader reader;
    Inode node;
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, "/direct.bin", node) == Result::Ok);
    const auto expect = [&](uint64_t block, uint8_t value) {
        uint8_t byte = 0xff;
        CHECK(reader.read(node, block * f.blockSize, 1, &byte) == Result::Ok && byte == value);
    };
    expect(0, 0x93);
    expect(single - 1, 0);
    expect(single, 0x12);
    expect(twice - 1, 0);
    expect(twice, 0x34);
    expect(triple - 1, 0);
    expect(triple, 0x56);
    expect(triple + 1, 0);

    put(f.image, 54 * f.blockSize, 4, 52);
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, "/direct.bin", node) == Result::Ok);
    uint8_t byte;
    CHECK(reader.read(node, triple * f.blockSize, 1, &byte) == Result::Corrupt);
    put(f.image, map + 12 * 4, 4, f.image.size() / f.blockSize);
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, "/direct.bin", node) == Result::Ok);
    CHECK(reader.read(node, single * f.blockSize, 1, &byte) == Result::Corrupt);
}

static void wideAddresses()
{
    Fixture f(0, true);
    constexpr uint64_t high = uint64_t{1} << 32;
    constexpr unsigned blocksPerGroup = 8192;
    constexpr uint64_t total = high + 128;
    const uint64_t groups = (total - 1 + blocksPerGroup - 1) / blocksPerGroup;
    f.setSb(0, 4, groups * Fixture::GroupInodes);
    f.setSb(4, 4, uint32_t(total));
    f.setSb(0x150, 4, total >> 32);
    f.setSb(0x20, 4, blocksPerGroup);
    f.setSb(0x24, 4, blocksPerGroup);
    put(f.image, f.group(0) + 0x28, 4, 1);
    f.leaf(f.inodeMap(Fixture::Root), 0, 0, 1, high + 20);
    f.makeInode(Fixture::Other, 0x8000, 1, 0x80000);
    f.header(f.inodeMap(Fixture::Other), 1, 4, 1);
    f.index(f.inodeMap(Fixture::Other), 0, 0, high + Fixture::LeftLeaf);
    const size_t leaf = Fixture::LeftLeaf * f.blockSize;
    f.header(leaf, 1, (f.blockSize - 12) / 12, 0);
    f.leaf(leaf, 0, 0, 1, high + 39);

    Source src{f.image, total * f.blockSize, UINT64_MAX, high * f.blockSize};
    Reader reader;
    Inode node;
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, "/direct.bin", node) == Result::Ok);
    uint8_t byte;
    CHECK(reader.read(node, 0, 1, &byte) == Result::Ok && byte == 0x93);
    // The callback would alias the low block if high bits were silently lost.
    src.failAt = (high + 39) * f.blockSize;
    CHECK(reader.read(node, 0, 1, &byte) == Result::Io);
}

static void partitionBounds()
{
    const Fixture good(0, true);
    struct Change { size_t at; unsigned width; uint64_t value; Result status; };
    const Change cases[] = {
        {0x38, 2, 0xef52, Result::Corrupt}, {0x3a, 2, 0, Result::Unsupported},
        {0x3a, 2, 3, Result::Unsupported}, {0xe8, 4, 2, Result::Unsupported},
        {0x60, 4, 0x2c6, Result::Unsupported}, {0x64, 4, 0x10400, Result::Unsupported},
        {0x60, 4, 0x2d2, Result::Unsupported}, {0x60, 4, 0x82c2, Result::Unsupported},
        {0x64, 4, 0x600, Result::Unsupported}, {0x4c, 4, 2, Result::Unsupported},
        {0x18, 4, UINT32_MAX, Result::Unsupported}, {0x14, 4, 0, Result::Corrupt},
        {0x1c, 4, 1, Result::Corrupt}, {0x20, 4, 0, Result::Corrupt},
        {0x28, 4, 0, Result::Corrupt}, {0, 4, UINT32_MAX, Result::Corrupt},
        {0x58, 2, 0, Result::Corrupt}, {0x58, 2, 192, Result::Corrupt},
        {0x58, 2, 2048, Result::Corrupt}, {0xfe, 2, 32, Result::Corrupt},
        {0xfe, 2, 128, Result::Corrupt}, {4, 4, 0, Result::Corrupt},
        {0x150, 4, UINT32_MAX, Result::Corrupt}
    };
    for (const Change &c : cases) {
        Fixture f = good;
        f.setSb(c.at, c.width, c.value);
        Source src{f.image, f.image.size()};
        Reader reader;
        CHECK(reader.open(&src, src.limit, Source::read) == c.status);
        CHECK(src.calls == 1);
    }
    Reader reader;
    Inode node;
    for (uint64_t n : {uint64_t(0), uint64_t(2047), uint64_t(2048), uint64_t(good.image.size() - 1)}) {
        Source src{good.image, n};
        CHECK(reader.open(&src, n, Source::read) == Result::Corrupt);
        CHECK(src.calls == (n >= 2048 ? 1u : 0u));
        CHECK(lookup(reader, "/", node) == Result::BadArgument);
    }
    Source src{good.image, good.image.size(), 1024};
    CHECK(reader.open(&src, src.limit, nullptr) == Result::BadArgument);
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Io);
    src.failAt = good.group(1);
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, Json, node) == Result::Io);
    src.failAt = 32 * good.blockSize;
    CHECK(lookup(reader, Json, node) == Result::Ok);
    uint8_t output;
    CHECK(reader.read(node, 0, 1, &output) == Result::Io);

    const Bytes truncated(good.image.begin(), good.image.begin() + 135 * good.blockSize);
    Source shortSource{truncated, good.image.size()};
    CHECK(reader.open(&shortSource, shortSource.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, Json, node) == Result::Io);
}

static void corruptMetadata()
{
    const Fixture good(0, true, 2);
    const size_t dir = Fixture::NestedBlock * good.blockSize;
    const size_t target = good.inodeOffset(Fixture::TextInode);
    const size_t root = good.inodeMap(Fixture::SparseInode);
    const size_t index = 42 * good.blockSize;
    const size_t left = Fixture::LeftLeaf * good.blockSize;
    const size_t right = Fixture::RightLeaf * good.blockSize;
    struct Change {
        size_t at;
        unsigned width;
        uint64_t value;
        std::string_view path;
        Result status;
        bool duringRead = false;
        uint64_t offset = 0;
    };
    const Change cases[] = {
        {good.group(1) + 8, 4, 2, Json, Result::Corrupt},
        {good.group(1) + 8, 4, 255, Json, Result::Corrupt}, // table end beyond filesystem
        {good.group(1) + 0x28, 4, UINT32_MAX, Json, Result::Corrupt},
        {good.group(1) + 0x12, 2, 1, Json, Result::Corrupt},
        {target + 0x1a, 2, 0, Text, Result::Corrupt},
        {target + 0x14, 4, 1, Text, Result::Corrupt},
        {target, 2, 0xa1ff, Text, Result::Unsupported}, // symlink with misleading regular-file dirent
        {target, 2, 0x21ff, Text, Result::Unsupported},
        {target + 0x20, 4, 0x10000000, Text, Result::Unsupported},
        {target + 0x6c, 4, UINT32_MAX, Text, Result::Corrupt},
        {dir + 4, 2, 0, Text, Result::Corrupt},
        {dir + 4, 2, 10, Text, Result::Corrupt},
        {dir + 4, 2, 1028, Text, Result::Corrupt},
        {dir + 4, 2, 1020, Text, Result::Corrupt},
        {dir + 6, 1, 255, Text, Result::Corrupt},
        {dir + 7, 1, 8, Text, Result::Corrupt},
        {dir + 8, 1, 0, Text, Result::Corrupt},
        {dir + 8, 1, '/', Text, Result::Corrupt},
        {dir, 4, 65, Text, Result::Corrupt},
        {dir, 4, Fixture::Registry, Text, Result::Corrupt}, // ancestor cycle
        {good.inodeMap(Fixture::Nested) + 16, 2, 32769, Text, Result::Corrupt}, // directory hole
        {root, 2, 0, Sparse, Result::Corrupt, true},
        {root + 2, 2, 5, Sparse, Result::Corrupt, true},
        {root + 4, 2, 0xffff, Sparse, Result::Corrupt, true},
        {root + 6, 2, 5, Sparse, Result::Unsupported, true},
        {root + 2, 2, 0, Sparse, Result::Corrupt, true},
        {root + 20, 2, 1, Sparse, Result::Corrupt, true},
        {index + 6, 2, 0, Sparse, Result::Corrupt, true},
        {index + 16, 4, 42, Sparse, Result::Corrupt, true}, // index cycle
        {index + 24, 4, 0, Sparse, Result::Corrupt, true}, // unsorted indices
        {left + 12, 4, 1, Sparse, Result::Corrupt, true}, // child key differs from parent
        {left + 16, 2, 0, Sparse, Result::Corrupt, true},
        {left + 16, 2, 3, Sparse, Result::Corrupt, true}, // overlapping extents
        {left + 28, 2, 32771, Sparse, Result::Corrupt, true}, // extent crosses sibling boundary
        {left + 20, 4, 40, Sparse, Result::Corrupt, true}, // data aliases its index
        {left + 32, 4, 42, Sparse, Result::Corrupt, true}, // unwritten run aliases its index
        {left + 20, 4, 0, Sparse, Result::Corrupt, true},
        {right + 20, 4, 256, Sparse, Result::Corrupt, true, 4096}, // physical run crosses image end
        {right + 18, 2, 0xffff, Sparse, Result::Corrupt, true, 4096}
    };
    for (const Change &c : cases) {
        Fixture f = good;
        put(f.image, c.at, c.width, c.value);
        Source src{f.image, f.image.size()};
        Reader reader;
        Inode node;
        CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
        const Result status = lookup(reader, c.path, node);
        if (c.duringRead) {
            uint8_t byte;
            CHECK(status == Result::Ok);
            CHECK(reader.read(node, c.offset, 1, &byte) == c.status);
        } else {
            CHECK(status == c.status);
        }
    }

    Fixture f(0, false, 0);
    Reader reader;
    Inode node;
    Source src{f.image, f.image.size()};
    const size_t map = f.inodeMap(Fixture::SparseInode);
    f.leaf(map, 2, UINT32_MAX, 2, 36);
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, Sparse, node) == Result::Ok);
    uint8_t byte;
    CHECK(reader.read(node, 0, 1, &byte) == Result::Corrupt);

    // Initialized ee_len=32768 must not be mistaken for an unwritten extent.
    f.image.resize((32768 + 129) * f.blockSize);
    f.setSb(4, 4, 32768 + 129);
    f.setSb(0, 4, (32768 / Fixture::GroupBlocks + 1) * Fixture::GroupInodes);
    f.header(map, 1, 4, 0);
    f.leaf(map, 0, 0, 32768, 128);
    f.image[128 * f.blockSize] = 0x52;
    src.limit = f.image.size();
    // The large geometry reserves more GDT blocks. Move only group 0's table and load the inode directly.
    std::copy_n(f.image.begin() + Fixture::FirstTable * f.blockSize, 4 * f.blockSize, f.image.begin() + 70 * f.blockSize);
    put(f.image, f.group(0) + 8, 4, 70);
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(reader.inode(Fixture::SparseInode, node) == Result::Ok);
    CHECK(reader.read(node, 0, 1, &byte) == Result::Ok && byte == 0x52);
}

// Generate reference checksums with a byte lookup table. The reader uses a bit-at-a-time implementation.
static uint32_t checksum(uint32_t initial, uint32_t polynomial, const Bytes &data, size_t at, size_t length)
{
    uint32_t table[256];
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (value & 1 ? polynomial : 0);
        table[i] = value;
    }
    for (size_t i = at; i < at + length; ++i) initial = (initial >> 8) ^ table[uint8_t(initial ^ data.at(i))];
    return initial;
}

static void checksumMetadata(Fixture &f, bool storedSeed, bool legacyGdt = false)
{
    constexpr uint32_t crc32c = 0x82f63b78, crc16 = 0xa001;
    const unsigned bs = f.blockSize;
    for (unsigned i = 0; i < 16; ++i) f.setSb(0x68 + i, 1, 17 * i);
    f.setSb(0x64, 4, legacyGdt ? 0x10 : 0x400);
    uint32_t seed = checksum(legacyGdt ? UINT16_MAX : UINT32_MAX, legacyGdt ? crc16 : crc32c, f.image, 1024 + 0x68, 16);
    if (storedSeed) {
        seed = 0x76543210;
        f.setSb(0x60 + 1, 1, 0x20 | (f.descSize == 64 ? 2 : 0));
        f.setSb(0x270, 4, seed);
    }
    for (unsigned group = 0; group < 2; ++group) {
        Bytes g(4);
        put(g, 0, 4, group);
        g.insert(g.end(), f.image.begin() + f.group(group), f.image.begin() + f.group(group) + f.descSize);
        if (legacyGdt) g.erase(g.begin() + 4 + 0x1e, g.begin() + 4 + 0x20);
        put(f.image, f.group(group) + 0x1e, 2, uint16_t(checksum(seed, legacyGdt ? crc16 : crc32c, g, 0, g.size())));
    }
    if (legacyGdt) return;
    f.setSb(0x175, 1, 1);
    f.setSb(0x3fc, 4, checksum(UINT32_MAX, crc32c, f.image, 1024, 0x3fc));

    uint32_t inodeSeed[Fixture::JsonInode + 1] = {};
    for (unsigned inode : {Fixture::Root, Fixture::Sensors, Fixture::Registry, Fixture::Inner, Fixture::Nested,
                           Fixture::VersionInode, Fixture::JsonInode, Fixture::TextInode, Fixture::EmptyInode, Fixture::SparseInode,
                           Fixture::Other}) {
        const size_t at = f.inodeOffset(inode);
        Bytes id(8);
        put(id, 0, 4, inode);
        put(id, 4, 4, 0x1234 + inode);
        put(f.image, at + 0x64, 4, 0x1234 + inode);
        inodeSeed[inode] = checksum(seed, crc32c, id, 0, id.size());
        // One wide inode uses only the low checksum because i_extra_isize is zero.
        const bool full = f.inodeSize > 128 && inode != Fixture::JsonInode;
        if (full) put(f.image, at + 0x80, 2, 32);
        const uint32_t sum = checksum(inodeSeed[inode], crc32c, f.image, at, f.inodeSize);
        put(f.image, at + 0x7c, 2, uint16_t(sum));
        if (full) put(f.image, at + 0x82, 2, sum >> 16);
    }

    for (unsigned disk : {42u, Fixture::LeftLeaf, Fixture::RightLeaf}) {
        const size_t at = disk * bs, tail = bs - bs % 12;
        put(f.image, at + tail, 4, checksum(inodeSeed[Fixture::SparseInode], crc32c, f.image, at, tail));
    }
    const unsigned dirs[][2] = {{Fixture::Root, 20}, {Fixture::Sensors, 21}, {Fixture::Registry, 22},
                               {Fixture::Inner, Fixture::InnerBlock}, {Fixture::Nested, Fixture::NestedBlock}};
    for (const auto &dir : dirs) {
        const size_t at = dir[1] * bs, tail = at + bs - 12;
        if (dir[0] != Fixture::Inner) {
            size_t pos = at;
            for (;;) {
                const size_t record = f.image[pos + 4] + 256u * f.image[pos + 5];
                CHECK(record && pos + record <= at + bs);
                if (pos + record == at + bs) {
                    put(f.image, pos + 4, 2, record - 12);
                    break;
                }
                pos += record;
            }
        }
        put(f.image, tail + 4, 2, 12);
        put(f.image, tail + 7, 1, 0xde);
        put(f.image, tail + 8, 4, checksum(inodeSeed[dir[0]], crc32c, f.image, at, bs - 12));
    }
    for (unsigned disk : {23u, 25u}) {
        const size_t at = disk * bs, count = disk == 23 ? 32 : 8;
        if (disk == 23) put(f.image, at + 29, 1, 8);     // root info length
        put(f.image, at + count, 2, (bs - count - 8) / 8);
        put(f.image, at + count + 2, 2, 1);
        uint32_t sum = checksum(inodeSeed[Fixture::Inner], crc32c, f.image, at, count + 8);
        sum = checksum(sum, crc32c, f.image, at + bs - 8, 8);
        put(f.image, at + bs - 4, 4, sum);
    }
}

static void metadataChecksums()
{
    for (unsigned shift = 0; shift <= 2; ++shift) {
        Fixture f(shift, shift != 0, 2);
        checksumMetadata(f, shift == 2);
        extraction(f);

        const auto expectCorrupt = [&](size_t at, bool superblock) {
            f.image[at] ^= 1;
            Source source{f.image, f.image.size()};
            Reader reader;
            CHECK(reader.open(&source, source.limit, Source::read) == (superblock ? Result::Corrupt : Result::Ok));
            if (!superblock) CHECK(verify(reader, makePack(f.files)) == Equivalence::ImageError);
            f.image[at] ^= 1;
        };
        expectCorrupt(1024 + 0x2c, true);                        // superblock mount time
        expectCorrupt(f.group(1) + 0x1e, false);                 // group descriptor checksum
        expectCorrupt(f.inodeOffset(Fixture::TextInode) + 0x7c, false);
        if (shift) {
            expectCorrupt(f.inodeOffset(Fixture::TextInode) + 0x82, false);
            expectCorrupt(f.inodeOffset(Fixture::JsonInode) + 0x90, false); // covered despite low-only checksum
        }
        expectCorrupt(Fixture::LeftLeaf * f.blockSize + f.blockSize - f.blockSize % 12, false);
        expectCorrupt((Fixture::NestedBlock + 1) * f.blockSize - 1, false); // leaf checksum
        expectCorrupt(24 * f.blockSize - 1, false);              // htree root checksum
        expectCorrupt(26 * f.blockSize - 1, false);              // htree interior checksum
    }
    for (bool wide : {false, true}) {
        Fixture f(1, wide, 2);
        checksumMetadata(f, false, true);
        extraction(f);
        f.image[f.group(1) + 0x1e] ^= 1;
        Source source{f.image, f.image.size()};
        Reader reader;
        Inode node;
        CHECK(reader.open(&source, source.limit, Source::read) == Result::Ok);
        CHECK(reader.inode(Fixture::JsonInode, node) == Result::Corrupt);
    }
}

static void pathAndDirectoryLimits()
{
    Fixture f;
    Reader reader;
    Source src{f.image, f.image.size()};
    Inode node;
    std::string path;
    // Build a chain with more than MaxDepth distinct directory inodes across two groups.
    f.directory(Fixture::Root, 70, {{11, "d", 2}});
    for (unsigned n = 11; n < 11 + MaxDepth; ++n) f.directory(n, 71 + n - 11, {{n + 1, "d", 2}});
    for (unsigned i = 0; i < MaxDepth; ++i) path += "/d";
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, path, node) == Result::Ok);
    CHECK(lookup(reader, path + "/d", node) == Result::Limit);

    // A declared directory size over the work budget is refused before reading its blocks.
    constexpr unsigned blocks = 8193;
    f.image.resize(blocks * f.blockSize);
    f.setSb(4, 4, blocks);
    f.setSb(0, 4, 64 * Fixture::GroupInodes);
    put(f.image, f.group(0) + 8, 4, 5);
    f.makeInode(Fixture::Root, 0x4000, MaxDirectoryBytes + f.blockSize, 0x80000);
    src.limit = f.image.size();
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    CHECK(lookup(reader, "/d", node) == Result::Limit);
}

static void equivalenceFailures()
{
    Fixture f;
    Source src{f.image, f.image.size()};
    Reader reader;
    CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
    const Bytes pack = makePack(f.files);
    CHECK(verify(reader, pack) == Equivalence::Match);
    auto different = f.files;
    different[Json][0] ^= 1;
    CHECK(verify(reader, makePack(different)) == Equivalence::Mismatch);
    different[Json].resize(1);
    CHECK(verify(reader, makePack(different)) == Equivalence::Mismatch);
    different = f.files;
    different.erase(Json);
    CHECK(verify(reader, makePack(different)) == Equivalence::Mismatch); // extra image file
    different = f.files;
    different["/sensors/registry/registry/missing"] = literal("synthetic");
    CHECK(verify(reader, makePack(different)) == Equivalence::Mismatch); // missing image file
    CHECK(verify(reader, makePack({})) == Equivalence::Mismatch);
    for (const size_t n : {size_t(0), size_t(31), pack.size() - 1}) {
        CHECK(verifyRegistry(reader, pack.data(), uint32_t(n)) == Equivalence::InvalidPack);
    }
    Bytes badPack = pack;
    badPack[SSP_HEADER] ^= 1;
    CHECK(verify(reader, badPack) == Equivalence::InvalidPack);
    CHECK(verifyRegistry(reader, nullptr, 0) == Equivalence::InvalidPack);

    const auto test = [&](Equivalence expected) {
        CHECK(reader.open(&src, src.limit, Source::read) == Result::Ok);
        CHECK(verify(reader, pack) == expected);
    };
    f.entries(Fixture::NestedBlock, {{Fixture::TextInode, "text.txt", 1}, {Fixture::TextInode, "text.txt", 1},
                                     {Fixture::SparseInode, "sparse.bin", 1}, {Fixture::EmptyInode, "empty", 1}});
    test(Equivalence::ImageError);
    f.entries(Fixture::NestedBlock, {{Fixture::Registry, "cycle", 2}});
    test(Equivalence::ImageError);
    f.entries(Fixture::NestedBlock, {{Fixture::TextInode, "link", 1}});
    put(f.image, f.inodeOffset(Fixture::TextInode), 2, 0xa1ff);
    test(Equivalence::ImageError);
    f = Fixture{};
    f.directory(20, 66, {{20, ".", 2}, {Fixture::Nested, "..", 2}});
    f.entries(Fixture::NestedBlock, {{Fixture::TextInode, "text.txt", 1}, {Fixture::SparseInode, "sparse.bin", 1},
                                     {Fixture::EmptyInode, "empty", 1}, {20, "empty-dir", 2}});
    test(Equivalence::Match); // the pack tool omits empty directories
}

static void mutate()
{
    Fixture f(0, true);
    const Bytes original = f.image;
    const Bytes pack = makePack(f.files);
    Reader reader;
    uint32_t state = 0x7523c941;
    const auto random = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    struct Range { size_t at, size; };
    const Range regions[] = {
        {1024, 1024}, {f.group(0), 128}, {f.inodeOffset(2), 128},
        {f.inodeOffset(Fixture::SparseInode), 128}, {Fixture::LeftLeaf * f.blockSize, 64},
        {Fixture::RightLeaf * f.blockSize, 32}, {Fixture::NestedBlock * f.blockSize, f.blockSize},
        {Fixture::InnerBlock * f.blockSize, f.blockSize}
    };
    for (unsigned trial = 0; trial < 2400; ++trial) {
        const auto &range = regions[trial % (sizeof regions / sizeof *regions)];
        const size_t at = range.at + random() % range.size;
        f.image[at] ^= uint8_t(1 + random() % 255);
        const uint64_t size = trial % 11 ? f.image.size() : random() % f.image.size();
        Source src{f.image, size};
        if (reader.open(&src, src.limit, Source::read) == Result::Ok) {
            Inode node;
            if (lookup(reader, Sparse, node) == Result::Ok) {
                uint8_t bytes[19];
                const uint64_t offset = trial % 7 ? random() % (node.size() + 1) : UINT64_MAX;
                reader.read(node, offset, sizeof bytes, bytes);
            }
            verify(reader, pack);
        }
        CHECK(src.calls < 10000);
        f.image[at] = original[at];
    }
}

int main()
{
    for (unsigned shift = 0; shift <= 2; ++shift) {
        extraction(Fixture(shift, false, 0));
        extraction(Fixture(shift, true, 1));
    }
    for (unsigned depth = 2; depth <= 4; ++depth) extraction(Fixture(0, false, depth));
    extraction(Fixture(0, false, 0, true));
    indirect();
    wideAddresses();
    partitionBounds();
    corruptMetadata();
    metadataChecksums();
    pathAndDirectoryLimits();
    equivalenceFailures();
    mutate();
    std::puts("PASS persist ext4 reader and registry pack equivalence (synthetic images)");
}
