/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomSmem.h"

#define TOC_MAGIC 0x434f5424u   /* "$TOC" */
#define PART_MAGIC 0x54525024u  /* "$PRT" */
#define CANARY 0xa5a5u
#define TOC_HEADER 32u
#define TOC_ENTRY 48u
#define PART_HEADER 32u
#define ENTRY 16u

uint32_t qcom_smem_read32(const QcomSmem *smem, uint32_t offset) {return *(volatile uint32_t *)(smem->base + offset);}
void qcom_smem_write32(const QcomSmem *smem, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(smem->base + offset) = value;
#if defined(__aarch64__) && defined(KERNEL)
    __asm__ volatile("dmb oshst" ::: "memory");
#endif
}
static uint32_t rd(const QcomSmem *s, uint32_t at) {return qcom_smem_read32(s, at);}
static int fits(uint64_t at, uint64_t length, uint64_t size) {return at <= size && length <= size - at;}
static uint32_t align_up(uint32_t value, uint32_t to) {return (value + to - 1) & ~(to - 1);}

int qcom_smem_partition(const QcomSmem *smem, uint16_t host, uint16_t remote, QcomSmemPartition *out)
{
    if (!smem || !smem->base || smem->size < 0x2000 || (smem->size & 0xfff)) return QcomSmemNoTable;
    const uint32_t toc = smem->size - 0x1000;
    if (rd(smem, toc) != TOC_MAGIC || rd(smem, toc + 4) != 1) return QcomSmemNoTable;
    const uint32_t count = rd(smem, toc + 8);
    if (count > (0x1000 - TOC_HEADER) / TOC_ENTRY) return QcomSmemNoTable;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = toc + TOC_HEADER + i * TOC_ENTRY;
        const uint32_t offset = rd(smem, e), size = rd(smem, e + 4), hosts = rd(smem, e + 12), cacheline = rd(smem, e + 16);
        const uint16_t h0 = (uint16_t)hosts, h1 = (uint16_t)(hosts >> 16);
        if (!offset || !size) continue;
        if (!((h0 == host && h1 == remote) || (h0 == remote && h1 == host))) continue;
        if (!fits(offset, size, toc) || size < PART_HEADER + 2 * ENTRY || (offset & 7)) return QcomSmemBadPartition;
        if (cacheline & (cacheline - 1)) return QcomSmemBadPartition;
        if (rd(smem, offset) != PART_MAGIC || rd(smem, offset + 4) != hosts || rd(smem, offset + 8) != size) return QcomSmemBadPartition;
        const uint32_t free_uncached = rd(smem, offset + 12), free_cached = rd(smem, offset + 16);
        if (free_uncached < PART_HEADER || free_uncached > free_cached || free_cached > size) return QcomSmemBadPartition;
        *out = (QcomSmemPartition){offset, size, cacheline ? cacheline : 8u, h0, h1};
        return QcomSmemOk;
    }
    return QcomSmemNoPartition;
}

/* The remote can update these after qcom_smem_partition. Bound each walk by a fresh snapshot. */
static int partition_marks(const QcomSmem *smem, const QcomSmemPartition *p, uint32_t *end, uint32_t *last)
{
    *end = rd(smem, p->offset + 12);
    *last = rd(smem, p->offset + 16);
    return *end >= PART_HEADER && *end <= *last && *last <= p->size;
}

static int find_uncached(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t end,
                         uint32_t *offset, uint32_t *size)
{
    for (uint32_t at = PART_HEADER; at < end;) {
        if (!fits(at, ENTRY, end)) return QcomSmemCorrupt;
        const uint32_t bytes = rd(smem, p->offset + at + 4), pads = rd(smem, p->offset + at + 8);
        const uint32_t tag = rd(smem, p->offset + at), pad = pads & 0xffff;
        const uint64_t data = (uint64_t)at + ENTRY + (pads >> 16), next = data + bytes;
        if ((tag & 0xffff) != CANARY || pad > bytes || next > end || ((data | next) & 3)) return QcomSmemCorrupt;
        if ((tag >> 16) == item) {
            *offset = p->offset + (uint32_t)data;
            *size = bytes - pad;
            return QcomSmemOk;
        }
        at = (uint32_t)next;
    }
    return QcomSmemMissing;
}

static int find_cached(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t last,
                       uint32_t *offset, uint32_t *size)
{
    /* Cached items: each entry sits at the end of its cache line slot, its data right below it. */
    if (last == p->size) return QcomSmemMissing;
    const uint32_t header = align_up(ENTRY, p->cacheline);
    if (p->size < header) return QcomSmemCorrupt;
    uint32_t e = p->size - header;
    while (e > last) {
        if (e & 3) return QcomSmemCorrupt;
        const uint32_t w0 = rd(smem, p->offset + e), bytes = rd(smem, p->offset + e + 4), pads = rd(smem, p->offset + e + 8);
        if ((w0 & 0xffff) != CANARY || bytes > e - last || (pads & 0xffff) > bytes || ((e - bytes) & 3))
            return QcomSmemCorrupt;
        if ((w0 >> 16) == item) {*offset = p->offset + e - bytes; *size = bytes - (pads & 0xffff); return QcomSmemOk;}
        if (e - bytes == last) break;
        if (e - bytes - last < header) return QcomSmemCorrupt;
        e -= bytes + header;
    }
    return QcomSmemMissing;
}

static int find_item(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t end, uint32_t last,
                     uint32_t *offset, uint32_t *size)
{
    const int rc = find_uncached(smem, p, item, end, offset, size);
    return rc == QcomSmemMissing ? find_cached(smem, p, item, last, offset, size) : rc;
}

int qcom_smem_find(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t *offset, uint32_t *size)
{
    uint32_t end, last;
    if (!partition_marks(smem, p, &end, &last)) return QcomSmemCorrupt;
    return find_item(smem, p, item, end, last, offset, size);
}

int qcom_smem_alloc(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t size)
{
    uint32_t at, last;
    if (!partition_marks(smem, p, &at, &last)) return QcomSmemCorrupt;
    uint32_t offset, bytes;
    const int rc = find_item(smem, p, item, at, last, &offset, &bytes);
    if (rc != QcomSmemMissing) return rc == QcomSmemOk ? QcomSmemExists : rc;
    if (!size || (uint64_t)size + ENTRY > last - at) return QcomSmemNoSpace;
    const uint32_t aligned = align_up(size, 8), need = ENTRY + aligned;
    if (need > last - at) return QcomSmemNoSpace;
    qcom_smem_write32(smem, p->offset + at, CANARY | (uint32_t)item << 16);
    qcom_smem_write32(smem, p->offset + at + 4, aligned);
    qcom_smem_write32(smem, p->offset + at + 8, aligned - size);  /* padding_data, padding_header 0 */
    qcom_smem_write32(smem, p->offset + at + 12, 0);
    for (uint32_t i = 0; i < aligned; i += 4) qcom_smem_write32(smem, p->offset + at + ENTRY + i, 0);
    /* The entry is complete before the free mark moves over it. */
    qcom_smem_write32(smem, p->offset + 12, at + need);
    return QcomSmemOk;
}
