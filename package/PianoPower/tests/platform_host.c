/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Synthetic images and shared-memory words for the firmware and IPC helpers. */
#include "FatVolume.h"
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
#include "Sm8750Scm.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void put(uint8_t *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

typedef struct { uint8_t *data; size_t size; } Buffer;

static int read_bytes(void *context, uint64_t offset, void *to, size_t length)
{
    const Buffer *b = context;
    if (offset > b->size || length > b->size - offset) return -1;
    memcpy(to, b->data + offset, length);
    return 0;
}

static void fat_boot(uint8_t *b, int fat32)
{
    put(b + 11, 512, 2);
    b[13] = 1;
    put(b + 14, 1, 2);
    b[16] = 1;
    put(b + 17, fat32 ? 0 : 16, 2);
    put(b + 32, fat32 ? 70000 : 5000, 4);
    if (fat32) {
        put(b + 36, 550, 4);
        put(b + 44, 2, 4);
    } else {
        put(b + 22, 20, 2);
    }
    b[510] = 0x55;
    b[511] = 0xaa;
}

static void fat_entry(uint8_t *d, const char *name, uint8_t attr, uint32_t cluster, uint32_t bytes)
{
    memcpy(d, name, 11);
    d[11] = attr;
    put(d + 20, cluster >> 16, 2);
    put(d + 26, cluster, 2);
    put(d + 28, bytes, 4);
}

static void test_fat(void)
{
    static uint8_t disk[553 * 512];
    Buffer source = {disk, 24 * 512};
    FatVolume v;
    FatFile f;
    char bytes[3];
    fat_boot(disk, 0);
    uint8_t *root = disk + 21 * 512;
    fat_entry(root, "FOLDER     ", 0x10, 0, 0);
    fat_entry(root + 32, "IMAGE   BIN", 0x20, 3, 3);
    memcpy(disk + 23 * 512, "abc", 3);
    CHECK(fat_mount(&v, source.size, read_bytes, &source) == FatNotFat);
    CHECK(fat_mount_prefix(&v, source.size, read_bytes, &source) == FatOk);
    CHECK(fat_open(&v, "image.bin", &f) == FatOk);
    CHECK(fat_read(&v, &f, 0, bytes, sizeof bytes) == FatOk && !memcmp(bytes, "abc", 3));
    CHECK(fat_open(&v, "folder/image.bin", &f) == FatDamaged);
    put(root + 26, 2, 2);
    memset(disk + 22 * 512, 0xe5, 512);
    v.cached_sector = UINT64_MAX;
    put(disk + 512 + 4, 0xffff, 2);
    CHECK(fat_open(&v, "folder/missing", &f) == FatNotFound);
    v.cached_sector = UINT64_MAX;
    put(disk + 512 + 4, 0xfff7, 2);
    CHECK(fat_open(&v, "folder/missing", &f) == FatDamaged);

    memset(disk, 0, sizeof disk);
    fat_boot(disk, 1);
    source.size = sizeof disk;
    put(disk + 512 + 12, 0x0fffffff, 4);
    fat_entry(disk + 551 * 512, "IMAGE   BIN", 0x20, 0xf0000003, 3);
    memcpy(disk + 552 * 512, "xyz", 3);
    CHECK(fat_mount_prefix(&v, source.size, read_bytes, &source) == FatOk);
    CHECK(fat_open(&v, "image.bin", &f) == FatOk && f.first_cluster == 3);
    CHECK(fat_read(&v, &f, 0, bytes, sizeof bytes) == FatOk && !memcmp(bytes, "xyz", 3));
}

static void test_mdt(void)
{
    enum { header_bytes = 64 + 3 * 56 };
    uint8_t elf[header_bytes + 8] = {0x7f, 'E', 'L', 'F', 2, 1};
    Buffer b = {elf, sizeof elf};
    QcomMdtImage image;
    uint8_t *hash = elf + 64 + 56, *load = hash + 56;
    put(elf + 32, 64, 8);
    put(elf + 54, 56, 2);
    put(elf + 56, 3, 2);
    put(elf + 64 + 32, header_bytes, 8);
    put(hash + 4, QCOM_MDT_TYPE_HASH, 4);
    put(hash + 8, header_bytes, 8);
    put(hash + 32, 4, 8);
    memcpy(elf + header_bytes, "hash", 4);
    put(load, 1, 4);
    put(load + 4, QCOM_MDT_RELOCATABLE, 4);
    put(load + 8, header_bytes + 4, 8);
    put(load + 24, 0x1000, 8);
    put(load + 32, 4, 8);
    put(load + 40, 5, 8);
    CHECK(qcom_mdt_parse(&image, b.size, read_bytes, &b) == QcomMdtOk);
    CHECK(qcom_mdt_span(&image) == 0x1000 && qcom_mdt_metadata_size(&image) == header_bytes + 4);
    uint8_t metadata[header_bytes + 4];
    CHECK(qcom_mdt_metadata(&image, read_bytes, &b, metadata, sizeof metadata) == QcomMdtOk &&
          !memcmp(metadata + header_bytes, "hash", 4));
    put(hash + 32, UINT64_MAX, 8);
    CHECK(qcom_mdt_parse(&image, b.size, read_bytes, &b) == QcomMdtOutside);
    put(hash + 32, 4, 8);
    put(hash + 8, b.size, 8);
    CHECK(qcom_mdt_parse(&image, b.size, read_bytes, &b) == QcomMdtOutside);
    /* Qualcomm's packed metadata form stores the hash directly after the headers. */
    b.size = sizeof metadata;
    CHECK(qcom_mdt_parse(&image, b.size, read_bytes, &b) == QcomMdtOk);
    CHECK(qcom_mdt_metadata(&image, read_bytes, &b, metadata, sizeof metadata) == QcomMdtOk &&
          !memcmp(metadata + header_bytes, "hash", 4));
    put(load + 24, UINT64_MAX - 5, 8);
    CHECK(qcom_mdt_parse(&image, b.size, read_bytes, &b) == QcomMdtBadHeaders);
}

static void test_smem(void)
{
    uint32_t words[0x2000 / 4] = {0};
    QcomSmem smem = {(uint8_t *)words, sizeof words};
    uint32_t *toc = words + 0x1000 / 4, *part = words + 0x100 / 4;
    toc[0] = 0x434f5424;
    toc[1] = toc[2] = 1;
    toc[8] = 0x100;
    toc[9] = 512;
    toc[11] = 2u << 16;
    toc[12] = 8;
    part[0] = 0x54525024;
    part[1] = toc[11];
    part[2] = part[4] = toc[9];
    part[3] = 32;
    QcomSmemPartition p;
    uint32_t offset, size;
    CHECK(qcom_smem_partition(&smem, 0, 2, &p) == QcomSmemOk);
    CHECK(qcom_smem_alloc(&smem, &p, 42, 5) == QcomSmemOk);
    CHECK(qcom_smem_find(&smem, &p, 42, &offset, &size) == QcomSmemOk && offset == 0x130 && size == 5);
    CHECK(qcom_smem_alloc(&smem, &p, 42, 5) == QcomSmemExists);

    part[3] = p.size + 4;
    CHECK(qcom_smem_find(&smem, &p, 42, &offset, &size) == QcomSmemCorrupt);
    part[3] = 56;
    part[9] = 7;
    part[10] = 1u << 16;
    CHECK(qcom_smem_find(&smem, &p, 42, &offset, &size) == QcomSmemCorrupt);
    part[9] = 8;
    part[10] = 3;
    part[4] = 448;
    part[496 / 4] = (43u << 16) | 0xa5a5u;
    part[(496 + 4) / 4] = 48;
    CHECK(qcom_smem_find(&smem, &p, 43, &offset, &size) == QcomSmemOk && offset == 0x100 + 448 && size == 48);
    part[(496 + 4) / 4] = 52;
    CHECK(qcom_smem_find(&smem, &p, 43, &offset, &size) == QcomSmemCorrupt);
    CHECK(qcom_smem_alloc(&smem, &p, 44, 8) == QcomSmemCorrupt && part[3] == 56);
}

static void test_mailboxes(void)
{
    uint32_t ram[64] = {0x4d41494c, 1};
    ram[13] = 104;
    ram[14] = 60;
    QcomQmp q;
    uint32_t short_ram[] = {ram[0]};
    qcom_qmp_init(&q, short_ram, sizeof short_ram, NULL, NULL);
    CHECK(qcom_qmp_step(&q) == QcomQmpBadDescriptor && short_ram[0] == ram[0]);
    qcom_qmp_init(&q, ram, sizeof ram, NULL, NULL);
    CHECK(qcom_qmp_step(&q) == QcomQmpLinkWait);
    ram[10] = ram[5] = ram[12] = 0xffff;
    CHECK(qcom_qmp_step(&q) == QcomQmpOpen);
    CHECK(qcom_qmp_send(&q, "abc") == QcomQmpSending && ram[15] == QCOM_QMP_MESSAGE && ram[16] == 0x636261);
    CHECK(qcom_qmp_send(&q, "def") == QcomQmpBusy);
    ram[15] = 0;
    CHECK(qcom_qmp_step(&q) == QcomQmpOpen);

    uint32_t local[QCOM_SMP2P_ITEM_BYTES / 4], remote[QCOM_SMP2P_ITEM_BYTES / 4];
    QcomSmp2p a, b;
    qcom_smp2p_init_out(&a, local, 0, 2);
    qcom_smp2p_init_out(&b, remote, 2, 0);
    a.in = remote;
    const int index = qcom_smp2p_add_entry(&b, "test-entry");
    uint32_t value, changed;
    CHECK(index == 0 && qcom_smp2p_update(&b, index, 3, 2) == 1);
    remote[4] = QCOM_SMP2P_FLAG_RESTART_DONE;
    CHECK(qcom_smp2p_service(&a, "test-entry", &value, &changed) == 1 &&
          value == 2 && changed == 2 && local[4] == QCOM_SMP2P_FLAG_RESTART_ACK);
    CHECK(qcom_smp2p_service(&a, "test-entry", &value, &changed) == 0 && value == 2 && changed == 0);
}

static int smc_reply(void *cookie, uint64_t regs[8])
{
    unsigned *calls = cookie;
    if ((*calls)++ == 0) {
        CHECK(regs[0] == SM8750_SCM_WAITQ_GET_WQ_CTX);
        regs[0] = Sm8750ScmX0Interrupted;
        regs[6] = 5;
    } else {
        CHECK(regs[0] == Sm8750ScmX0Interrupted && regs[6] == 5);
        regs[0] = 0;
    }
    return 0;
}

static void test_scm(void)
{
    unsigned calls = 0;
    const Sm8750ScmOps ops = {.smc = smc_reply, .cookie = &calls};
    Sm8750Scm scm;
    Sm8750SmcResult r;
    Sm8750ScmInit(&scm, &ops);
    CHECK(Sm8750ScmFastCall(&scm, SM8750_SCM_WAITQ_GET_WQ_CTX, 0, NULL, &r) == Sm8750ScmOk &&
          calls == 2 && r.calls == 2 && r.interrupted == 1);
}

static void test_rpmh(void)
{
    const QcomRscLayout layout = qcom_rsc_layout(3u << 16);
    const uint32_t address = 1, value = 2;
    QcomRpmhWrite writes[9];
    CHECK(qcom_rpmh_active_request(&layout, 31, &address, &value, 1, writes, 9) == 9 &&
          writes[1].value == (1u << 31));
    CHECK(qcom_rpmh_active_request(&layout, 32, &address, &value, 1, writes, 9) == 0);
    CHECK(qcom_rpmh_finish(&layout, 32, writes, 9) == 0);
}

int main(void)
{
    test_fat();
    test_mdt();
    test_smem();
    test_scm();
    test_rpmh();
    test_mailboxes();
    if (!failures) puts("platform_host: FAT, MDT, SMEM, SMP2P, QMP, RPMh and SCM passed");
    return failures != 0;
}
