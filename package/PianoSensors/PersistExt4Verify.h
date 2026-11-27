/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PERSIST_EXT4_VERIFY_H
#define PERSIST_EXT4_VERIFY_H

#include "PersistExt4Ro.h"
#include "SensorsLoad.h"

namespace persist_ext4 {

enum class Equivalence { Match, Mismatch, InvalidPack, ImageError, Limit };

// Compare the registry's regular file set and contents. The pack buffer must contain
// packBytes readable bytes. The result carries no path or calibration data.
inline Equivalence verifyRegistry(Reader &image, const uint8_t *packBytes, uint32_t packSize)
{
    SspHeader header;
    SspPack pack;
    if (packSize > SL_MAX_BYTES || ssp_header(packBytes, packSize, &header) ||
        header.total != packSize || ssp_open(&pack, &header, packBytes + SSP_HEADER))
        return Equivalence::InvalidPack;

    constexpr char imageRoot[] = "/sensors/registry";
    constexpr char packRoot[] = "/mnt/vendor/persist/sensors/registry";
    constexpr size_t rootLength = sizeof packRoot - 1;
    const int rootIndex = ssp_find(&pack, packRoot);
    if (rootIndex < 0) return Equivalence::Mismatch;
    SspEntry root;
    ssp_entry(&pack, uint32_t(rootIndex), &root);
    if (root.flags != SSP_DIRECTORY) return Equivalence::Mismatch;

    unsigned expected = 0;
    for (uint32_t i = uint32_t(rootIndex) + 1; i < pack.count; ++i) {
        SspEntry entry;
        ssp_entry(&pack, i, &entry);
        size_t n = 0;
        while (n < rootLength && entry.name[n] == packRoot[n]) ++n;
        if (n == rootLength && entry.name[n] == '/' && entry.flags != SSP_DIRECTORY) ++expected;
    }
    if (!expected) return Equivalence::Mismatch;

    struct Frame {
        Inode dir;
        uint64_t cursor;
        size_t pathLength;
    };
    Frame stack[MaxDepth - 1]; // imageRoot uses two path components
    Result rc = image.lookup(imageRoot, sizeof imageRoot - 1, stack[0].dir);
    if (rc == Result::NotFound) return Equivalence::Mismatch;
    if (rc == Result::Limit) return Equivalence::Limit;
    if (rc != Result::Ok || !stack[0].dir.directory()) return Equivalence::ImageError;

    char path[SSP_PATH] = "/mnt/vendor/persist/sensors/registry";
    stack[0].cursor = 0;
    stack[0].pathLength = rootLength;
    unsigned depth = 0, visited = 0, matched = 0;
    constexpr uint64_t maxScannedBytes = 16u << 20;
    uint64_t scannedBytes = stack[0].dir.size();
    uint8_t seen[(SSP_MAX_ENTRIES + 7) / 8] = {}, bytes[MaxBlock];
    for (;;) {
        const Frame &frame = stack[depth];
        DirEntry entry;
        rc = image.next(frame.dir, stack[depth].cursor, entry);
        if (rc == Result::End) {
            if (!depth) break;
            --depth;
            continue;
        }
        if (rc == Result::Limit) return Equivalence::Limit;
        if (rc != Result::Ok) return Equivalence::ImageError;
        if (++visited > SSP_MAX_ENTRIES || depth + 2 == MaxDepth || frame.pathLength + 1 + entry.length >= sizeof path)
            return Equivalence::Limit;

        size_t length = frame.pathLength;
        path[length++] = '/';
        for (size_t i = 0; i < entry.length; ++i) path[length++] = entry.name[i];
        path[length] = '\0';
        const int index = ssp_find(&pack, path);
        SspEntry packed;
        if (index >= 0) {
            ssp_entry(&pack, uint32_t(index), &packed);
            const unsigned mask = 1u << (unsigned(index) % 8);
            auto &slot = seen[unsigned(index) / 8];
            if (slot & mask) return Equivalence::ImageError;
            slot |= mask;
        }

        Inode child;
        if (image.inode(entry.inode, child) != Result::Ok) return Equivalence::ImageError;
        if (child.directory()) {
            if (index >= 0 && packed.flags != SSP_DIRECTORY) return Equivalence::Mismatch;
            if (child.size() > maxScannedBytes - scannedBytes)
                return Equivalence::Limit;
            for (unsigned i = 0; i <= depth; ++i) if (stack[i].dir.number() == child.number()) return Equivalence::ImageError;
            scannedBytes += child.size();
            stack[++depth] = {child, 0, length};
            continue;
        }
        if (index < 0 || packed.flags == SSP_DIRECTORY || child.size() != packed.size) return Equivalence::Mismatch;
        for (uint64_t offset = 0; offset < packed.size;) {
            const uint64_t left = packed.size - offset;
            const size_t count = left < sizeof bytes ? size_t(left) : sizeof bytes;
            if (image.read(child, offset, count, bytes) != Result::Ok) return Equivalence::ImageError;
            for (size_t i = 0; i < count; ++i) {
                if (bytes[i] != packBytes[packed.offset + offset + i]) return Equivalence::Mismatch;
            }
            offset += count;
        }
        ++matched;
    }
    return matched == expected ? Equivalence::Match : Equivalence::Mismatch;
}

} // namespace persist_ext4
#endif
