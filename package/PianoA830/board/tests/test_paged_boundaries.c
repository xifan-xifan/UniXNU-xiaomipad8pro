/* SPDX-License-Identifier: MIT */
// cc -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I../../kits/ArmSmmu4K test_paged_boundaries.c ../../kits/ArmSmmu4K/ArmSmmu4KPaged.c -o /tmp/test_paged_boundaries && /tmp/test_paged_boundaries
#include "ArmSmmu4KPaged.h"
#include <stdio.h>
#include <stdlib.h>

#define PAGE 4096u
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static _Alignas(PAGE) uint64_t tables[4][PAGE / sizeof(uint64_t)];
static const uint64_t addresses[] = {0, PAGE, UINT32_MAX - PAGE + 1u, UINT64_C(1) << 32};
static unsigned allocated, released, publishes, fail_publish;

static int allocate(void *cookie, uint64_t *pa, volatile uint64_t **cpu)
{
    (void)cookie;
    CHECK(allocated < sizeof(addresses) / sizeof(*addresses));
    *pa = addresses[allocated];
    *cpu = tables[allocated++];
    return 0;
}

static volatile uint64_t *lookup(void *cookie, uint64_t pa)
{
    (void)cookie;
    for (unsigned i = 0; i < allocated; ++i)
        if (addresses[i] == pa && !(released & (1u << i))) return tables[i];
    return NULL;
}

static void release(void *cookie, uint64_t pa, volatile uint64_t *cpu)
{
    CHECK(cpu && cpu == lookup(cookie, pa));
    for (unsigned i = 0; i < allocated; ++i)
        if (addresses[i] == pa) released |= 1u << i;
}

static int publish(void *cookie, volatile uint64_t *cpu, size_t bytes)
{
    (void)cookie;
    CHECK(cpu && bytes == PAGE);
    return ++publishes == fail_publish;
}

static const ArmSmmu4KPagedPages pages = {NULL, allocate, release, lookup, publish, NULL};
static const ArmSmmu4KPagedConfig narrow = {32, 32, 0, 0};

static void shared_output_width(void)
{
    const ArmSmmu4KPagedConfig wide = {32, 36, 0, 0};
    ArmSmmu4KPagedSpace owner = {0}, borrower = {0};
    uint64_t fit, high;
    CHECK(ArmSmmu4KPagedInit(&owner, &wide, &pages) == ArmSmmu4KSuccess);
    CHECK(ArmSmmu4KPagedInit(&borrower, &narrow, &pages) == ArmSmmu4KSuccess);
    CHECK(ArmSmmu4KPagedShare(&owner, 0, &fit) == ArmSmmu4KSuccess && fit == UINT32_MAX - PAGE + 1u);
    CHECK(ArmSmmu4KPagedShare(&owner, 1, &high) == ArmSmmu4KSuccess && high == (UINT64_C(1) << 32));
    CHECK(ArmSmmu4KPagedLinkShared(&borrower, 1, high) == ArmSmmu4KBadRange);
    CHECK(!borrower.root[1] && !borrower.generation && !borrower.foreign[0]);
    CHECK(ArmSmmu4KPagedLinkShared(&borrower, 0, fit) == ArmSmmu4KSuccess);
    CHECK(ArmSmmu4KPagedTeardown(&borrower) == ArmSmmu4KSuccess && released == 2u);
    CHECK(ArmSmmu4KPagedTeardown(&owner) == ArmSmmu4KSuccess && released == 15u);
}

static void shared_publish_failure(void)
{
    ArmSmmu4KPagedSpace owner = {0}, borrower = {0};
    uint64_t shared;
    CHECK(ArmSmmu4KPagedInit(&owner, &narrow, &pages) == ArmSmmu4KSuccess);
    CHECK(ArmSmmu4KPagedInit(&borrower, &narrow, &pages) == ArmSmmu4KSuccess);
    CHECK(ArmSmmu4KPagedShare(&owner, 0, &shared) == ArmSmmu4KSuccess);
    fail_publish = publishes + 1;
    CHECK(ArmSmmu4KPagedLinkShared(&borrower, 0, shared) == ArmSmmu4KIoError && borrower.poisoned);
    CHECK(ArmSmmu4KPagedTeardown(&borrower) == ArmSmmu4KSuccess && released == 2u);
    CHECK(!borrower.counters.table_pages && !borrower.counters.level_pages[borrower.start_level + 1]);
    CHECK(ArmSmmu4KPagedTeardown(&owner) == ArmSmmu4KSuccess && released == 7u);
}

int main(void)
{
    shared_publish_failure();
    allocated = released = publishes = fail_publish = 0;
    shared_output_width();
    puts("PASS: shared paged tables preserve ownership on publish failure and enforce output width");
}
