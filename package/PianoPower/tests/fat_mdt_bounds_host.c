// SPDX-License-Identifier: BSD-2-Clause-Patent
// Build: clang -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all -I.. fat_mdt_bounds_host.c ../FatVolume.c ../QcomMdt.c -o /tmp/fat_mdt_bounds_host && /tmp/fat_mdt_bounds_host

#include "FatVolume.h"
#include "QcomMdt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct {
    const uint8_t *bytes;
    size_t size;
} Buffer;

static int read_buffer(void *context, uint64_t offset, void *to, size_t length)
{
    const Buffer *b = context;
    if (offset > b->size || length > b->size - offset) return -1;
    if (length) memcpy(to, b->bytes + offset, length);
    return 0;
}

static int read_checked(void *context, uint64_t offset, void *to, size_t length)
{
    CHECK(read_buffer(context, offset, to, length) == 0);
    return 0;
}

static void put_le(uint8_t *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

enum {
    SectorBytes = 512,
    DirEntryBytes = 32,
    FatSectors = 20,
    RootOffset = (1 + FatSectors) * SectorBytes,
    RootBytes = 2 * SectorBytes,
    DataOffset = RootOffset + RootBytes,
    Fat16End = 0xffff,
    LfnCharsPerEntry = 13,
    MaxLfnEntries = 20,
    MaxLfnLength = 255,
};

static const char short_alias[] = "FIRM~1  BIN";
static const char short_path[] = "firm~1.bin";
static const uint8_t file_bytes[] = {0x11, 0x28, 0x37};
static uint8_t disk[DataOffset + 3 * SectorBytes];

static void set_fat_link(unsigned cluster, unsigned next)
{
    put_le(disk + SectorBytes + 2 * cluster, next, 2);
}

static uint8_t *cluster_data(unsigned cluster)
{
    return disk + DataOffset + (cluster - 2) * SectorBytes;
}

static Buffer make_fat16(void)
{
    memset(disk, 0, sizeof disk);
    put_le(disk + 11, SectorBytes, 2);
    disk[13] = 1;
    put_le(disk + 14, 1, 2);
    disk[16] = 1;
    put_le(disk + 17, RootBytes / DirEntryBytes, 2);
    put_le(disk + 19, 5000, 2);
    disk[21] = 0xf8;
    put_le(disk + 22, FatSectors, 2);
    disk[510] = 0x55;
    disk[511] = 0xaa;
    set_fat_link(0, 0xfff8);
    set_fat_link(1, Fat16End);
    set_fat_link(3, Fat16End);
    memcpy(cluster_data(3), file_bytes, sizeof file_bytes);
    return (Buffer){disk, sizeof disk};
}

static void short_entry(uint8_t *entry, const char *alias, uint8_t attr, unsigned cluster, unsigned size)
{
    memcpy(entry, alias, 11);
    entry[11] = attr;
    put_le(entry + 26, cluster, 2);
    put_le(entry + 28, size, 4);
}

static void make_lfn(char name[MaxLfnLength + 1], unsigned entries)
{
    const unsigned length = entries == MaxLfnEntries ? MaxLfnLength : entries * LfnCharsPerEntry;
    for (unsigned i = 0; i < length; ++i)
        name[i] = (char)('a' + (i / LfnCharsPerEntry + i % LfnCharsPerEntry) % 26);
    name[length] = '\0';
}

static void write_lfn(uint8_t *dir, const char *name, unsigned entries)
{
    static const unsigned slots[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    const size_t length = strlen(name);
    uint8_t checksum = 0;
    for (unsigned i = 0; i < 11; ++i)
        checksum = (uint8_t)((checksum >> 1) + ((checksum & 1) << 7) + short_alias[i]);

    for (unsigned seq = entries; seq > 0; --seq, dir += DirEntryBytes) {
        memset(dir, 0, DirEntryBytes);
        dir[0] = (uint8_t)(seq | (seq == entries ? 0x40 : 0));
        dir[11] = 0x0f;
        dir[13] = checksum;
        for (unsigned i = 0; i < LfnCharsPerEntry; ++i) {
            const size_t at = (seq - 1) * LfnCharsPerEntry + i;
            put_le(dir + slots[i], at <= length ? (uint8_t)name[at] : 0xffff, 2);
        }
    }
    short_entry(dir, short_alias, 0x20, 3, sizeof file_bytes);
}

static void expect_fat_file(FatVolume *volume, const char *name)
{
    FatFile file;
    uint8_t out[sizeof file_bytes];
    CHECK(fat_open(volume, name, &file) == FatOk);
    CHECK(!file.directory && file.first_cluster == 3 && file.size == sizeof out);
    CHECK(fat_read(volume, &file, 0, out, sizeof out) == FatOk);
    CHECK(memcmp(out, file_bytes, sizeof out) == 0);
}

static void test_valid_lfn(void)
{
    char name[MaxLfnLength + 1];
    for (unsigned entries = 1; entries <= MaxLfnEntries; ++entries) {
        Buffer source = make_fat16();
        FatVolume volume;
        make_lfn(name, entries);
        write_lfn(disk + RootOffset, name, entries);
        CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatOk);
        expect_fat_file(&volume, name);
    }
}

static void test_invalid_lfn(void)
{
    const struct { unsigned entries, entry, byte; uint8_t value; } corruptions[] = {
        {3, 1, 13, 0},       // continuation checksum differs from the first entry's
        {1, 0, 13, 0},       // LFN checksum differs from the short entry's
        {3, 1, 0, 3},        // repeat ordinal 3 where ordinal 2 is required
        {3, 1, 0, 1},        // skip from ordinal 3 to ordinal 1
        {3, 0, 0, 0x40 | 21}, // last-entry ordinal exceeds 20
        {3, 1, 0, 21},       // continuation ordinal exceeds 20
    };
    char name[MaxLfnLength + 1];
    for (size_t i = 0; i < sizeof corruptions / sizeof corruptions[0]; ++i) {
        Buffer source = make_fat16();
        FatVolume volume;
        FatFile file;
        uint8_t *root = disk + RootOffset;
        make_lfn(name, corruptions[i].entries);
        write_lfn(root, name, corruptions[i].entries);
        uint8_t *field = root + corruptions[i].entry * DirEntryBytes + corruptions[i].byte;
        CHECK(*field != corruptions[i].value);
        *field = corruptions[i].value;
        CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatOk);
        CHECK(fat_open(&volume, name, &file) == FatNotFound);
        expect_fat_file(&volume, short_path);
    }
}

static void test_zero_lfn_continuation(void)
{
    Buffer source = make_fat16();
    FatVolume volume;
    FatFile file;
    char name[MaxLfnLength + 1];
    uint8_t *root = disk + RootOffset;
    make_lfn(name, 2);
    write_lfn(root, name, 2);
    CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatOk);
    expect_fat_file(&volume, name);

    memcpy(root + 3 * DirEntryBytes, root + 2 * DirEntryBytes, DirEntryBytes);
    memcpy(root + 2 * DirEntryBytes, root + DirEntryBytes, DirEntryBytes);
    // The reserved bit keeps the entry visible to lookup while its low five bits decode to zero.
    root[2 * DirEntryBytes] = 0x20;
    CHECK(fat_open(&volume, name, &file) == FatNotFound);
    expect_fat_file(&volume, short_path);
}

static void test_fat_truncation(void)
{
    Buffer source = make_fat16();
    FatVolume volume;
    FatFile file;
    char name[MaxLfnLength + 1];
    uint8_t *root = disk + RootOffset;
    make_lfn(name, 1);
    memset(root, 0xe5, RootBytes);
    // The alias immediately beyond the fixed root must be invisible.
    write_lfn(root + RootBytes - DirEntryBytes, name, 1);
    CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatOk);
    CHECK(fat_open(&volume, name, &file) == FatNotFound);
    CHECK(fat_open(&volume, short_path, &file) == FatNotFound);

    source.size = DataOffset - 1;
    CHECK(fat_mount(&volume, source.size, read_checked, &source) == FatNotFat);
    CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatNotFat);

    source = make_fat16();
    uint8_t dir[2 * SectorBytes] = {0};
    char path[sizeof "image/" + 2 * LfnCharsPerEntry] = "image/";
    make_lfn(name, 2);
    strcat(path, name);
    memset(dir, 0xe5, SectorBytes - DirEntryBytes);
    write_lfn(dir + SectorBytes - DirEntryBytes, name, 2);
    short_entry(root, "IMAGE      ", 0x10, 2, 0);
    memcpy(cluster_data(2), dir, SectorBytes);
    memcpy(cluster_data(4), dir + SectorBytes, SectorBytes);
    set_fat_link(2, 4);
    set_fat_link(4, Fat16End);
    CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatOk);
    expect_fat_file(&volume, path);

    set_fat_link(2, Fat16End);
    CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatOk);
    CHECK(fat_open(&volume, path, &file) == FatNotFound);

    set_fat_link(2, 4);
    --source.size;   // only a partial final directory cluster is physically present
    CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatOk);
    CHECK(fat_open(&volume, path, &file) == FatDamaged);

    short_entry(root + DirEntryBytes, short_alias, 0x20, 3, SectorBytes + 1);
    CHECK(fat_open(&volume, short_path, &file) == FatOk);
    uint8_t out = 0xaa;
    CHECK(fat_read(&volume, &file, SectorBytes, &out, 1) == FatDamaged);
    CHECK(out == 0xaa);
}

static void test_zero_fat_geometry(void)
{
    Buffer source = make_fat16();
    FatVolume volume;
    CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatOk);
    put_le(disk + 11, 0, 2);
    CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatNotFat);
    put_le(disk + 11, SectorBytes, 2);
    disk[13] = 0;
    CHECK(fat_mount_prefix(&volume, source.size, read_checked, &source) == FatNotFat);
}

enum {
    HeaderPiece, HashPiece, FirstLoad, BssLoad, LastLoad, PieceCount,
    HashOffset = 0x180,
    FirstOffset = 0x190,
    LastOffset = 0x1a0,
    LoadAddress = 0x8000,
};

static const uint8_t hash_bytes[] = {0x12, 0x34, 0x56};
static const uint8_t first_bytes[] = {0x10, 0x20, 0x30, 0x40};
static const uint8_t last_bytes[] = {0x89, 0xab, 0xcd};

typedef struct {
    uint8_t headers[64 + PieceCount * 56];
    Buffer piece[PieceCount];
    QcomMdtSplit split;
} SplitFixture;

static uint8_t *program_header(SplitFixture *fixture, int elf64, unsigned index)
{
    return fixture->headers + (elf64 ? 64 + index * 56 : 52 + index * 32);
}

static void write_program_header(SplitFixture *fixture, int elf64, unsigned index, unsigned type,
                                 uint32_t flags, uint64_t offset, uint64_t address,
                                 unsigned file_size, unsigned memory_size)
{
    uint8_t *p = program_header(fixture, elf64, index);
    const unsigned word_bytes = elf64 ? 8 : 4;
    put_le(p, type, 4);
    put_le(p + (elf64 ? 4 : 24), flags, 4);
    put_le(p + (elf64 ? 8 : 4), offset, word_bytes);
    put_le(p + (elf64 ? 16 : 8), address, word_bytes);
    put_le(p + (elf64 ? 24 : 12), address, word_bytes);
    put_le(p + (elf64 ? 32 : 16), file_size, word_bytes);
    put_le(p + (elf64 ? 40 : 20), memory_size, word_bytes);
}

static void make_mdt(SplitFixture *fixture, int elf64)
{
    *fixture = (SplitFixture){0};
    uint8_t *e = fixture->headers;
    const unsigned header_size = elf64 ? 64 : 52;
    const unsigned ph_size = elf64 ? 56 : 32;
    const unsigned table_end = header_size + PieceCount * ph_size;
    memcpy(e, "\x7f" "ELF", 4);
    e[4] = elf64 ? 2 : 1;
    e[5] = 1;
    e[6] = 1;
    put_le(e + 16, 2, 2);  // ET_EXEC
    put_le(e + 18, elf64 ? 183 : 40, 2);  // EM_AARCH64 or EM_ARM
    put_le(e + 20, 1, 4);  // EV_CURRENT
    put_le(e + (elf64 ? 32 : 28), header_size, elf64 ? 8 : 4);
    put_le(e + (elf64 ? 52 : 40), header_size, 2);
    put_le(e + (elf64 ? 54 : 42), ph_size, 2);
    put_le(e + (elf64 ? 56 : 44), PieceCount, 2);
    write_program_header(fixture, elf64, HeaderPiece, 0, 0, 0, 0, table_end, 0);
    write_program_header(fixture, elf64, HashPiece, 0, QCOM_MDT_TYPE_HASH, HashOffset, 0, sizeof hash_bytes, 0);
    write_program_header(fixture, elf64, FirstLoad, 1, elf64 ? QCOM_MDT_RELOCATABLE : 0,
                         FirstOffset, LoadAddress, sizeof first_bytes, 6);
    write_program_header(fixture, elf64, BssLoad, 1, 0, 0, LoadAddress + 8, 0, 4);
    write_program_header(fixture, elf64, LastLoad, 1, 0, LastOffset, LoadAddress + 16, sizeof last_bytes, 5);
    fixture->piece[HeaderPiece] = (Buffer){e, table_end};
    fixture->piece[HashPiece] = (Buffer){hash_bytes, sizeof hash_bytes};
    fixture->piece[FirstLoad] = (Buffer){first_bytes, sizeof first_bytes};
    fixture->piece[LastLoad] = (Buffer){last_bytes, sizeof last_bytes};
}

static int read_piece(void *context, unsigned index, uint64_t offset, void *to, size_t length)
{
    SplitFixture *fixture = context;
    CHECK(index < PieceCount);
    return read_buffer(&fixture->piece[index], offset, to, length);
}

static int read_split(void *context, uint64_t offset, void *to, size_t length)
{
    SplitFixture *fixture = context;
    return qcom_mdt_split_read(&fixture->split, read_piece, fixture, offset, to, length);
}

static void test_valid_mdt(int elf64)
{
    SplitFixture fixture;
    QcomMdtImage image;
    make_mdt(&fixture, elf64);
    const size_t headers = fixture.piece[HeaderPiece].size;
    uint8_t expected[LastOffset + sizeof last_bytes] = {0};
    uint8_t out[sizeof expected];
    memcpy(expected, fixture.headers, headers);
    memcpy(expected + HashOffset, hash_bytes, sizeof hash_bytes);
    memcpy(expected + FirstOffset, first_bytes, sizeof first_bytes);
    memcpy(expected + LastOffset, last_bytes, sizeof last_bytes);

    CHECK(qcom_mdt_split(&fixture.split, fixture.headers, headers) == QcomMdtOk);
    CHECK(fixture.split.count == PieceCount && fixture.split.size == sizeof expected);
    CHECK(fixture.split.piece[BssLoad].size == 0);
    memset(out, 0xa5, sizeof out);
    CHECK(read_split(&fixture, 0, out, sizeof out) == QcomMdtOk);
    CHECK(memcmp(out, expected, sizeof out) == 0);
    CHECK(read_split(&fixture, sizeof out, out, 1) == QcomMdtOutside);

    CHECK(qcom_mdt_parse(&image, fixture.split.size, read_split, &fixture) == QcomMdtOk);
    CHECK(image.elf64 == elf64 && image.relocatable == elf64);
    CHECK(image.hash == HashPiece && image.count == PieceCount && qcom_mdt_span(&image) == 0x1000);
    CHECK(qcom_mdt_metadata_size(&image) == headers + sizeof hash_bytes);
    CHECK(qcom_mdt_metadata(&image, read_split, &fixture, out, headers + sizeof hash_bytes) == QcomMdtOk);
    CHECK(memcmp(out, fixture.headers, headers) == 0);
    CHECK(memcmp(out + headers, hash_bytes, sizeof hash_bytes) == 0);

    const uint64_t base = elf64 ? 0x9000 : LoadAddress;
    uint64_t at;
    CHECK(qcom_mdt_place(&image, FirstLoad, base, 21, &at) == QcomMdtOk && at == 0);
    CHECK(qcom_mdt_place(&image, BssLoad, base, 21, &at) == QcomMdtOk && at == 8);
    CHECK(qcom_mdt_place(&image, LastLoad, base, 21, &at) == QcomMdtOk && at == 16);
    CHECK(qcom_mdt_place(&image, LastLoad, base, 20, &at) == QcomMdtOutside);
    CHECK(qcom_mdt_place(&image, HashPiece, base, 21, &at) == QcomMdtOutside);
    CHECK(qcom_mdt_place(&image, image.count, base, 21, &at) == QcomMdtOutside);
    CHECK(qcom_mdt_place(&image, QCOM_MDT_MAX_SEGMENTS, base, 21, &at) == QcomMdtOutside);

    const QcomMdtSegment *last = &image.segment[LastLoad];
    CHECK(last->file_offset == LastOffset && last->file_size == sizeof last_bytes && last->memory_size == 5);
    CHECK(read_split(&fixture, last->file_offset, out, (size_t)last->file_size) == QcomMdtOk);
    CHECK(memcmp(out, last_bytes, sizeof last_bytes) == 0);
    const Buffer saved = fixture.piece[LastLoad];
    fixture.piece[LastLoad] = (Buffer){0};
    fixture.piece[BssLoad] = saved;   // the data was supplied as .b03 instead of .b04
    CHECK(read_split(&fixture, last->file_offset, out, (size_t)last->file_size) == QcomMdtReadError);
    fixture.piece[BssLoad] = (Buffer){0};
    const uint8_t truncated[] = {last_bytes[0], last_bytes[1]};
    fixture.piece[LastLoad] = (Buffer){truncated, sizeof truncated};
    CHECK(read_split(&fixture, last->file_offset + 1, out, 1) == QcomMdtOk && out[0] == last_bytes[1]);
    CHECK(read_split(&fixture, last->file_offset, out, (size_t)last->file_size) == QcomMdtReadError);
}

static void test_invalid_mdt(int elf64)
{
    SplitFixture fixture;
    QcomMdtImage image;
    make_mdt(&fixture, elf64);
    Buffer source = fixture.piece[HeaderPiece];
    --source.size;
    CHECK(qcom_mdt_split(&fixture.split, source.bytes, source.size) == QcomMdtBadHeaders);
    CHECK(qcom_mdt_parse(&image, source.size, read_checked, &source) == QcomMdtBadHeaders);

    ++source.size;
    uint8_t *phnum = fixture.headers + (elf64 ? 56 : 44);
    put_le(phnum, 0, 2);
    CHECK(qcom_mdt_split(&fixture.split, source.bytes, source.size) == QcomMdtBadHeaders);
    CHECK(qcom_mdt_parse(&image, source.size, read_checked, &source) == QcomMdtBadHeaders);
    put_le(phnum, PieceCount, 2);

    uint8_t *load = program_header(&fixture, elf64, FirstLoad);
    put_le(load + (elf64 ? 32 : 16), 7, elf64 ? 8 : 4);
    CHECK(qcom_mdt_split(&fixture.split, source.bytes, source.size) == QcomMdtOk);
    CHECK(qcom_mdt_parse(&image, fixture.split.size, read_split, &fixture) == QcomMdtBadHeaders);
    put_le(load + (elf64 ? 32 : 16), sizeof first_bytes, elf64 ? 8 : 4);

    const uint64_t limit = elf64 ? UINT64_MAX : UINT32_MAX;
    put_le(load + (elf64 ? 8 : 4), limit - 2, elf64 ? 8 : 4);
    const int split_result = qcom_mdt_split(&fixture.split, source.bytes, source.size);
    if (elf64) {
        CHECK(split_result == QcomMdtBadHeaders);
    } else {
        CHECK(split_result == QcomMdtOk && fixture.split.size == UINT64_C(0x100000001));
    }

    // With packed metadata, parse succeeds and placement must check the file extent itself.
    uint8_t packed[sizeof fixture.headers + sizeof hash_bytes];
    memcpy(packed, source.bytes, source.size);
    memcpy(packed + source.size, hash_bytes, sizeof hash_bytes);
    source = (Buffer){packed, source.size + sizeof hash_bytes};
    CHECK(qcom_mdt_parse(&image, source.size, read_checked, &source) == QcomMdtOk);
    uint64_t at = UINT64_MAX;
    CHECK(qcom_mdt_place(&image, FirstLoad, LoadAddress, 21, &at) == QcomMdtOutside);
    CHECK(at == UINT64_MAX);
}

int main(void)
{
    test_valid_lfn();
    test_invalid_lfn();
    test_zero_lfn_continuation();
    test_fat_truncation();
    test_zero_fat_geometry();
    test_valid_mdt(0);
    test_valid_mdt(1);
    test_invalid_mdt(0);
    test_invalid_mdt(1);
    puts("fat_mdt_bounds_host: FAT LFN and ELF32/ELF64 MDT bounds passed");
    return EXIT_SUCCESS;
}
