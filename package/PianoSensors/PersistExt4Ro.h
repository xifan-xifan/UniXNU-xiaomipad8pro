/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PERSIST_EXT4_RO_H
#define PERSIST_EXT4_RO_H

#include <stddef.h>
#include <stdint.h>

namespace persist_ext4 {

constexpr size_t MaxBlock = 4096, MaxName = 255, MaxPath = 4096;
constexpr unsigned MaxDepth = 32;
constexpr uint64_t MaxDirectoryBytes = 4u << 20;

enum class Result { Ok, End, NotFound, BadArgument, Corrupt, Unsupported, Io, Limit };

// Byte offsets relative to the partition image. True means all len bytes were copied to out.
using ReadBlocks = bool (*)(const void *ctx, uint64_t offset, size_t len, void *out);

class Reader;

class Inode {
public:
    uint64_t size() const { return size_; }
    uint32_t number() const { return number_; }
    bool directory() const { return directory_; }

private:
    friend class Reader;
    const Reader *owner_ = nullptr;
    uint64_t size_ = 0;
    uint32_t number_ = 0, checksumSeed_ = 0;
    bool directory_ = false, extents_ = false;
    bool indexed_ = false;
    uint8_t map_[60] = {};
};

struct DirEntry {
    uint32_t inode;
    size_t length;
    char name[MaxName + 1];
};

// A read-only view of an immutable image. Each reader owns one block of scratch space.
// Serialize operations on a Reader. Inodes and cursors belong to the current open image.
class Reader {
public:
    Result open(const void *ctx, uint64_t imageBytes, ReadBlocks readBlocks)
    {
        read_ = nullptr;
        cachedBlock_ = 0;
        if (!readBlocks) return Result::BadArgument;
        if (!fits(1024, 1024, imageBytes)) return Result::Corrupt;
        if (!readBlocks(ctx, 1024, 1024, block_)) return Result::Io;

        const uint8_t *sb = block_;
        if (le16(sb + 0x38) != 0xef53) return Result::Corrupt;
        // This reader only interprets clean on-disk metadata.
        if (le16(sb + 0x3a) != 1 || le32(sb + 0xe8)) return Result::Unsupported;
        const uint32_t revision = le32(sb + 0x4c);
        const uint32_t incompat = le32(sb + 0x60), roCompat = le32(sb + 0x64);
        // Flexible block groups, EA inodes, and the checksum seed leave these read paths intact.
        constexpr uint32_t supportedIncompat = FileType | Extents | Blocks64 | 0x200 | 0x400 | ChecksumSeed;
        // Sparse superblocks, large/huge files, GDT checksums, dir_nlink, extra_isize,
        // quota, metadata checksums, read-only, project quota, and verity.
        constexpr uint32_t supportedRo = 0x7b | 0x100 | 0x400 | 0x1000 | 0x2000 | 0x8000;
        if (revision > 1 || (incompat & ~supportedIncompat) || (roCompat & ~supportedRo))
            return Result::Unsupported;
        metadataCsum_ = (roCompat & 0x400) != 0;
        gdtCsum_ = (roCompat & 0x10) != 0;
        if (metadataCsum_) {
            if (sb[0x175] != 1 || le32(sb + 0x48) != 0) return Result::Unsupported; // CRC32C, Linux inode layout
            if (crc(UINT32_MAX, sb, 0x3fc) != le32(sb + 0x3fc)) return Result::Corrupt;
            checksumSeed_ = incompat & ChecksumSeed ? le32(sb + 0x270) : crc(UINT32_MAX, sb + 0x68, 16);
        } else if (gdtCsum_) {
            checksumSeed_ = crc(UINT16_MAX, sb + 0x68, 16, Crc16);
        }

        const uint32_t shift = le32(sb + 0x18);
        if (shift > 2) return Result::Unsupported;
        blockSize_ = 1024u << shift;
        const uint32_t first = le32(sb + 0x14);
        const uint32_t blocksPerGroup = le32(sb + 0x20);
        inodesPerGroup_ = le32(sb + 0x28);
        inodeCount_ = le32(sb);
        inodeSize_ = revision == 0 ? 128 : le16(sb + 0x58);
        descSize_ = incompat & Blocks64 ? le16(sb + 0xfe) : 32;
        blocks_ = le32(sb + 4);
        if (incompat & Blocks64) blocks_ |= uint64_t(le32(sb + 0x150)) << 32;

        if (first != (blockSize_ == 1024 ? 1u : 0u) || shift != le32(sb + 0x1c) ||
            blocksPerGroup != le32(sb + 0x24) || !blocksPerGroup || blocksPerGroup > blockSize_ * 8 ||
            !inodesPerGroup_ || inodesPerGroup_ > blockSize_ * 8 || inodeCount_ < 2 ||
            inodeSize_ < 128 || inodeSize_ > blockSize_ || (inodeSize_ & (inodeSize_ - 1)) ||
            ((incompat & Blocks64) && descSize_ != 64) ||
            blocks_ <= first || blocks_ > imageBytes / blockSize_)
            return Result::Corrupt;

        const uint64_t groups = ceilDiv(blocks_ - first, blocksPerGroup);
        if (groups != ceilDiv(inodeCount_, inodesPerGroup_)) return Result::Corrupt;
        const uint64_t tableFirst = uint64_t(first) + 1;
        const uint64_t tableBlocks = ceilDiv(groups * descSize_, blockSize_);
        if (!fits(tableFirst, tableBlocks, blocks_)) return Result::Corrupt;
        dataFirst_ = tableFirst + tableBlocks;
        descOffset_ = tableFirst * blockSize_;
        inodeTableBlocks_ = ceilDiv(uint64_t(inodesPerGroup_) * inodeSize_, blockSize_);
        if (!validBlocks(dataFirst_, inodeTableBlocks_)) return Result::Corrupt;

        bytes_ = blocks_ * blockSize_;
        features_ = incompat;
        ctx_ = ctx;
        read_ = readBlocks;
        return Result::Ok;
    }

    // Loads a live regular file or directory. Symlinks and special files are refused.
    Result inode(uint32_t number, Inode &out)
    {
        if (!read_) return Result::BadArgument;
        if (!number || number > inodeCount_) return Result::Corrupt;
        const uint64_t group = (number - 1) / inodesPerGroup_;
        const uint32_t index = (number - 1) % inodesPerGroup_;
        uint8_t scratch[128];
        Result rc = readAt(descOffset_ + group * descSize_, descSize_, scratch);
        if (rc != Result::Ok) return rc;
        if (!validDescriptor(scratch, uint32_t(group))) return Result::Corrupt;
        if (le16(scratch + 0x12) & 1) return Result::Corrupt; // BG_INODE_UNINIT
        uint64_t table = le32(scratch + 8);
        if (descSize_ == 64) table |= uint64_t(le32(scratch + 0x28)) << 32;
        if (!validBlocks(table, inodeTableBlocks_)) return Result::Corrupt;

        const uint64_t offset = uint64_t(index) * inodeSize_;
        const uint8_t *bytes = scratch;
        uint32_t seed = 0;
        if (metadataCsum_) {
            rc = loadBlock(table + offset / blockSize_);
            if (rc != Result::Ok) return rc;
            bytes = block_ + offset % blockSize_;
            seed = crc(crcWord(checksumSeed_, number), bytes + 0x64, 4); // i_generation
            if (!validInodeChecksum(bytes, seed)) return Result::Corrupt;
        } else {
            rc = readAt(table * blockSize_ + offset, sizeof scratch, scratch);
            if (rc != Result::Ok) return rc;
        }

        const uint16_t type = le16(bytes) & 0xf000;
        if (!type || !le16(bytes + 0x1a) || le32(bytes + 0x14)) return Result::Corrupt;
        if (type != 0x8000 && type != 0x4000) return Result::Unsupported;
        const uint32_t flags = le32(bytes + 0x20);
        constexpr uint32_t unsupportedFlags = 0x4 | 0x800 | 0x2000 | 0x200000 | 0x10000000 | 0x40000000;
        if (flags & unsupportedFlags) return Result::Unsupported;
        const bool extents = (flags & 0x80000) != 0; // EXT4_EXTENTS_FL
        if (extents && !(features_ & Extents)) return Result::Corrupt;
        const uint64_t size = le32(bytes + 4) | (uint64_t(le32(bytes + 0x6c)) << 32);
        const bool directory = type == 0x4000;
        const uint64_t fanout = blockSize_ / 4;
        const uint64_t maxBlocks = extents ? LogicalLimit : 12 + fanout + fanout * fanout + fanout * fanout * fanout;
        if (ceilDiv(size, blockSize_) > maxBlocks ||
            (directory && (!size || size % blockSize_ || size > bytes_)))
            return Result::Corrupt;

        out.owner_ = this;
        out.size_ = size;
        out.number_ = number;
        out.checksumSeed_ = seed;
        out.directory_ = directory;
        out.extents_ = extents;
        out.indexed_ = (flags & 0x1000) != 0; // EXT4_INDEX_FL
        for (size_t i = 0; i < sizeof out.map_; ++i) out.map_[i] = bytes[0x28 + i];
        return Result::Ok;
    }

    // Exact range read. On error, callers must discard the output, which may be partly filled.
    Result read(const Inode &file, uint64_t offset, size_t len, void *out)
    {
        if (!read_ || file.owner_ != this || file.directory_ || (!out && len) || !fits(offset, len, file.size_))
            return Result::BadArgument;
        auto *dst = static_cast<uint8_t *>(out);
        while (len) {
            const size_t within = offset % blockSize_;
            const size_t count = len < blockSize_ - within ? len : blockSize_ - within;
            uint64_t disk;
            Result rc = map(file, offset / blockSize_, disk);
            if (rc != Result::Ok) return rc;
            if (disk) {
                rc = readAt(disk * blockSize_ + within, count, dst);
                if (rc != Result::Ok) return rc;
            } else {
                for (size_t i = 0; i < count; ++i) dst[i] = 0;
            }
            offset += count;
            dst += count;
            len -= count;
        }
        return Result::Ok;
    }

    // Set cursor to zero to start. Reuse only the cursor returned for this directory.
    // Skips unused records (including htree metadata), "." and "..".
    Result next(const Inode &dir, uint64_t &cursor, DirEntry &out)
    {
        if (!read_ || dir.owner_ != this || !dir.directory_ || cursor > dir.size_ || cursor % 4)
            return Result::BadArgument;
        if (dir.size_ > MaxDirectoryBytes) return Result::Limit;
        while (cursor < dir.size_) {
            const size_t within = cursor % blockSize_;
            const uint64_t logical = cursor / blockSize_;
            if (cachedDir_ != dir.number_ || cachedLogical_ != logical || !cachedBlock_) {
                uint64_t disk;
                Result rc = map(dir, logical, disk);
                if (rc != Result::Ok) return rc;
                if (!disk) return Result::Corrupt;
                rc = loadBlock(disk);
                if (rc != Result::Ok) return rc;
                size_t dirBytes = blockSize_;
                if (metadataCsum_ && !validDirectoryChecksum(dir, logical, dirBytes)) return Result::Corrupt;
                cachedDir_ = dir.number_;
                cachedLogical_ = logical;
                cachedDirBytes_ = dirBytes;
            }
            if (within == cachedDirBytes_) {
                cursor += blockSize_ - within;
                continue;
            }
            if (within > cachedDirBytes_ || cachedDirBytes_ - within < 8) return Result::Corrupt;
            const size_t left = cachedDirBytes_ - within;

            const uint8_t *entry = block_ + within;
            const uint32_t number = le32(entry);
            const size_t record = le16(entry + 4), nameLen = entry[6];
            if (record < 8 || record > left || record % 4) return Result::Corrupt;
            cursor += record;
            if (!number) continue;
            if (number > inodeCount_ || !nameLen || nameLen > record - 8 ||
                entry[7] > ((features_ & FileType) ? 7 : 0))
                return Result::Corrupt;
            const uint8_t *name = entry + 8;
            if (name[0] == '.' && (nameLen == 1 || (nameLen == 2 && name[1] == '.'))) continue;
            for (size_t i = 0; i < nameLen; ++i) {
                if (!name[i] || name[i] == '/') return Result::Corrupt;
                out.name[i] = static_cast<char>(name[i]);
            }
            out.name[nameLen] = '\0';
            out.inode = number;
            out.length = nameLen;
            return Result::Ok;
        }
        return Result::End;
    }

    // A canonical absolute path of length bytes, excluding any terminator.
    Result lookup(const char *path, size_t length, Inode &out)
    {
        if (!path || !length || path[0] != '/') return Result::BadArgument;
        if (length > MaxPath) return Result::Limit;
        Inode node;
        Result rc = inode(2, node);
        if (rc != Result::Ok) return rc;
        if (!node.directory_) return Result::Corrupt;
        uint32_t ancestors[MaxDepth + 1] = {2};
        unsigned depth = 0;
        for (size_t at = 1; at < length;) {
            if (depth == MaxDepth) return Result::Limit;
            const size_t start = at;
            while (at < length && path[at] != '/') {
                if (!path[at++]) return Result::BadArgument;
            }
            const size_t len = at - start;
            if (!len || len > MaxName || (at < length && at + 1 == length) ||
                (path[start] == '.' && (len == 1 || (len == 2 && path[start + 1] == '.'))))
                return Result::BadArgument;
            if (!node.directory_) return Result::NotFound;
            uint32_t child;
            rc = findChild(node, path + start, len, child);
            if (rc != Result::Ok) return rc;
            for (unsigned i = 0; i <= depth; ++i) if (ancestors[i] == child) return Result::Corrupt;
            rc = inode(child, node);
            if (rc != Result::Ok) return rc;
            ancestors[++depth] = child;
            ++at;
        }
        out = node;
        return Result::Ok;
    }

private:
    static constexpr uint32_t FileType = 0x2, Extents = 0x40, Blocks64 = 0x80, ChecksumSeed = 0x2000;
    static constexpr uint32_t Crc32c = 0x82f63b78, Crc16 = 0xa001;
    static constexpr uint64_t LogicalLimit = uint64_t{1} << 32;
    static constexpr unsigned MaxExtentDepth = 4;

    static uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
    static uint32_t le32(const uint8_t *p) { return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16; }
    static uint64_t ceilDiv(uint64_t n, uint64_t d) { return n / d + (n % d != 0); }
    static bool fits(uint64_t at, uint64_t len, uint64_t total) { return at <= total && len <= total - at; }

    static uint32_t crc(uint32_t value, const uint8_t *p, size_t length, uint32_t polynomial = Crc32c)
    {
        for (size_t i = 0; i < length; ++i) {
            value ^= p[i];
            for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (polynomial & (0u - (value & 1)));
        }
        return value;
    }

    static uint32_t crcWord(uint32_t seed, uint32_t word, uint32_t polynomial = Crc32c)
    {
        const uint8_t bytes[] = {uint8_t(word), uint8_t(word >> 8), uint8_t(word >> 16), uint8_t(word >> 24)};
        return crc(seed, bytes, sizeof bytes, polynomial);
    }

    bool validDescriptor(const uint8_t *desc, uint32_t group) const
    {
        if (!metadataCsum_ && !gdtCsum_) return true;
        const uint32_t polynomial = metadataCsum_ ? Crc32c : Crc16;
        const uint8_t zero[2] = {};
        uint32_t sum = crc(crcWord(checksumSeed_, group, polynomial), desc, 0x1e, polynomial);
        if (metadataCsum_) sum = crc(sum, zero, sizeof zero);
        sum = crc(sum, desc + 0x20, descSize_ - 0x20, polynomial);
        return uint16_t(sum) == le16(desc + 0x1e);
    }

    bool validInodeChecksum(const uint8_t *inode, uint32_t seed) const
    {
        const unsigned extra = inodeSize_ > 128 ? le16(inode + 0x80) : 0;
        if (extra > inodeSize_ - 128 || extra % 4) return false;
        const uint8_t zero[2] = {};
        uint32_t sum = crc(crc(seed, inode, 0x7c), zero, sizeof zero);
        const uint32_t low = le16(inode + 0x7c);
        if (extra >= 4) {
            sum = crc(crc(sum, inode + 0x7e, 4), zero, sizeof zero);
            sum = crc(sum, inode + 0x84, inodeSize_ - 0x84);
            return sum == (low | (uint32_t(le16(inode + 0x82)) << 16));
        }
        return uint16_t(crc(sum, inode + 0x7e, inodeSize_ - 0x7e)) == low;
    }

    bool validDirectoryChecksum(const Inode &dir, uint64_t logical, size_t &dataBytes) const
    {
        const uint32_t seed = dir.checksumSeed_;
        size_t entries = 0;
        if (dir.indexed_) {
            if (!logical) {
                if (le16(block_ + 4) != 12 || le16(block_ + 16) != blockSize_ - 12 ||
                    le32(block_ + 24) || block_[29] != 8) return false;
                entries = 32;
            } else if (!le32(block_) && le16(block_ + 4) == blockSize_) {
                entries = 8;
            }
        }
        if (entries) {
            const size_t limit = le16(block_ + entries), count = le16(block_ + entries + 2);
            const size_t tail = entries + limit * 8;
            if (!count || count > limit || tail != blockSize_ - 8) return false;
            const uint32_t sum = crc(crc(seed, block_, entries + count * 8), block_ + tail, 4);
            return crcWord(sum, 0) == le32(block_ + tail + 4);
        }
        dataBytes = blockSize_ - 12;
        const uint8_t *tail = block_ + dataBytes;
        return !le32(tail) && le32(tail + 4) == 0xde00000c && crc(seed, block_, dataBytes) == le32(tail + 8);
    }

    bool validBlocks(uint64_t first, uint64_t count) const
    {
        return first >= dataFirst_ && fits(first, count, blocks_);
    }

    Result readAt(uint64_t offset, size_t count, void *out)
    {
        if (!fits(offset, count, bytes_)) return Result::Corrupt;
        return read_(ctx_, offset, count, out) ? Result::Ok : Result::Io;
    }

    Result loadBlock(uint64_t disk)
    {
        if (!validBlocks(disk, 1)) return Result::Corrupt;
        if (disk == cachedBlock_) return Result::Ok;
        cachedBlock_ = 0;
        cachedDir_ = 0;
        const Result rc = readAt(disk * blockSize_, blockSize_, block_);
        if (rc == Result::Ok) cachedBlock_ = disk;
        return rc;
    }

    Result findChild(const Inode &dir, const char *name, size_t len, uint32_t &child)
    {
        uint64_t cursor = 0;
        DirEntry entry;
        child = 0;
        for (;;) {
            const Result rc = next(dir, cursor, entry);
            if (rc == Result::End) return child ? Result::Ok : Result::NotFound;
            if (rc != Result::Ok) return rc;
            if (len != entry.length) continue;
            size_t i = 0;
            while (i < len && name[i] == entry.name[i]) ++i;
            if (i != len) continue;
            if (child) return Result::Corrupt;
            child = entry.inode;
        }
    }

    Result map(const Inode &file, uint64_t logical, uint64_t &disk)
    {
        disk = 0;
        return file.extents_ ? mapExtents(file, logical, disk) : mapIndirect(file.map_, logical, disk);
    }

    static const uint8_t *extentEntry(const uint8_t *node, unsigned index) { return node + 12 + index * 12; }
    static uint64_t extentDisk(const uint8_t *entry, bool index)
    {
        return index ? le32(entry + 4) | (uint64_t(le16(entry + 8)) << 32)
                     : le32(entry + 8) | (uint64_t(le16(entry + 6)) << 32);
    }

    Result mapExtents(const Inode &file, uint64_t logical, uint64_t &disk)
    {
        const uint8_t *node = file.map_;
        unsigned depth = le16(node + 6);
        if (depth > MaxExtentDepth) return Result::Unsupported;
        uint64_t lower = 0, upper = LogicalLimit, visited[MaxExtentDepth] = {};
        unsigned level = 0;
        size_t capacity = 4;
        for (;;) {
            const unsigned entries = le16(node + 2), maximum = le16(node + 4);
            if (le16(node) != 0xf30a || le16(node + 6) != depth || !maximum ||
                maximum > capacity || entries > maximum || (!entries && (depth || level)))
                return Result::Corrupt;
            if (level && metadataCsum_) {
                const size_t tail = 12 + maximum * 12;
                if (crc(file.checksumSeed_, node, tail) != le32(node + tail)) return Result::Corrupt;
            }

            // Validate ordering and full spans before trusting the index for binary search.
            uint64_t end = lower;
            for (unsigned i = 0; i < entries; ++i) {
                const uint8_t *entry = extentEntry(node, i);
                const uint64_t start = le32(entry);
                const unsigned encoded = le16(entry + 4);
                const unsigned count = depth ? 1 : encoded > 32768 ? encoded - 32768 : encoded;
                const uint64_t physical = extentDisk(entry, depth != 0);
                if (!count || start < end || !fits(start, count, upper) || (!i && level && start != lower) ||
                    !validBlocks(physical, count))
                    return Result::Corrupt;
                for (unsigned j = 0; j < level; ++j)
                    if (visited[j] >= physical && visited[j] - physical < count) return Result::Corrupt;
                end = start + count;
            }

            unsigned lo = 0, hi = entries;
            while (lo < hi) {
                const unsigned mid = lo + (hi - lo) / 2;
                if (le32(extentEntry(node, mid)) <= logical) lo = mid + 1;
                else hi = mid;
            }
            if (!lo) return Result::Ok; // hole before the first extent
            const uint8_t *entry = extentEntry(node, lo - 1);
            const uint64_t physical = extentDisk(entry, depth != 0);
            if (!depth) {
                const uint64_t delta = logical - le32(entry);
                if (delta >= le16(entry + 4) || le16(entry + 4) > 32768) return Result::Ok;
                disk = physical + delta;
                return Result::Ok;
            }

            visited[level++] = physical;
            lower = le32(entry);
            if (lo < entries) upper = le32(extentEntry(node, lo));
            const Result rc = loadBlock(physical);
            if (rc != Result::Ok) return rc;
            node = block_;
            capacity = (blockSize_ - 12 - (metadataCsum_ ? 4 : 0)) / 12;
            --depth;
        }
    }

    Result mapIndirect(const uint8_t *map, uint64_t logical, uint64_t &disk)
    {
        const uint64_t fanout = blockSize_ / 4;
        uint64_t span = 1;
        unsigned depth = 0;
        if (logical >= 12) {
            logical -= 12;
            for (span = fanout, depth = 1; logical >= span; span *= fanout, ++depth) {
                if (depth == 3) return Result::Corrupt;
                logical -= span;
            }
        }
        uint64_t physical = le32(map + 4 * (depth ? 11 + depth : logical));
        uint64_t visited[3];
        for (unsigned level = 0; physical; ++level) {
            if (!validBlocks(physical, 1)) return Result::Corrupt;
            for (unsigned i = 0; i < level; ++i) if (visited[i] == physical) return Result::Corrupt;
            if (level == depth) {
                disk = physical;
                break;
            }
            visited[level] = physical;
            const Result rc = loadBlock(physical);
            if (rc != Result::Ok) return rc;
            span /= fanout;
            physical = le32(block_ + 4 * (logical / span));
            logical %= span;
        }
        return Result::Ok;
    }

    ReadBlocks read_ = nullptr;
    const void *ctx_ = nullptr;
    uint64_t bytes_ = 0, blocks_ = 0, dataFirst_ = 0, descOffset_ = 0, inodeTableBlocks_ = 0;
    uint32_t blockSize_ = 0, inodeSize_ = 0, descSize_ = 0, inodesPerGroup_ = 0, inodeCount_ = 0, features_ = 0;
    uint32_t checksumSeed_ = 0;
    bool metadataCsum_ = false, gdtCsum_ = false;
    uint64_t cachedBlock_ = 0, cachedLogical_ = 0;
    uint32_t cachedDir_ = 0;
    size_t cachedDirBytes_ = 0;
    uint8_t block_[MaxBlock];
};

} // namespace persist_ext4
#endif
