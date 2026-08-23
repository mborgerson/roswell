/*
 * Big (page-and-larger) pool allocations under churn.
 *
 * Regression coverage for the big-page-tracker trim (ExpReallocateBigPageTable
 * stub): allocations of a page and up route through the
 * page allocator and the big-page tracker table; the trim freezes that
 * table at its boot size, so the title-observable contract this group
 * pins is that page-sized-and-up ExAllocatePoolWithTag traffic keeps
 * succeeding, returns page-aligned distinct blocks, round-trips data,
 * and frees cleanly under interleaved alloc/free churn (the free path
 * is what reaches the tracker's shrink rebalance).
 *
 * Retail-validated: same observable behavior -- the tracker
 * is internal bookkeeping only; no alloc/free shape here can fail or
 * misaccount in a way a title can see.
 */

#include "../harness.h"

#define TAG_BIG  'gibT'

#define N_FIRST   64            /* 64 x 8 KB  = 512 KB           */
#define N_SECOND  32            /* 32 x 16 KB = 512 KB, staggered */

static void fill_pattern(void *p, SIZE_T bytes, unsigned seed)
{
    unsigned char *b = (unsigned char *)p;
    SIZE_T i;
    for (i = 0; i < bytes; i++)
        b[i] = (unsigned char)(seed ^ (i * 13));
}

static bool check_pattern(const void *p, SIZE_T bytes, unsigned seed)
{
    const unsigned char *b = (const unsigned char *)p;
    SIZE_T i;
    for (i = 0; i < bytes; i++)
        if (b[i] != (unsigned char)(seed ^ (i * 13)))
            return false;
    return true;
}

/* Page-and-up allocations are page-aligned and distinct. */
static bool t_bigpool_alignment(void)
{
    PVOID a = ExAllocatePoolWithTag(4096, TAG_BIG);
    PVOID b = ExAllocatePoolWithTag(2 * 4096, TAG_BIG);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    if (((ULONG_PTR)a & (4096 - 1)) || ((ULONG_PTR)b & (4096 - 1))) {
        ExFreePool(a);
        ExFreePool(b);
        FAIL_AND_RETURN("big pool block not page-aligned (%p, %p)", a, b);
    }
    if (a == b) {
        ExFreePool(a);
        FAIL_AND_RETURN("distinct allocations aliased");
    }
    ExFreePool(a);
    ExFreePool(b);
    return true;
}

/* Many live big blocks, interleaved frees, then a second wave: data
 * integrity and free-path health while the tracker table churns. */
static bool t_bigpool_churn(void)
{
    static PVOID first[N_FIRST];
    static PVOID second[N_SECOND];
    int i;

    for (i = 0; i < N_FIRST; i++) {
        first[i] = ExAllocatePoolWithTag(8 * 1024, TAG_BIG);
        if (!first[i]) FAIL_AND_RETURN("first wave alloc %d failed", i);
        fill_pattern(first[i], 8 * 1024, (unsigned)i);
    }

    /* Free the even half; the survivors must be untouched. */
    for (i = 0; i < N_FIRST; i += 2) {
        ExFreePool(first[i]);
        first[i] = NULL;
    }
    for (i = 1; i < N_FIRST; i += 2) {
        if (!check_pattern(first[i], 8 * 1024, (unsigned)i))
            FAIL_AND_RETURN("first wave block %d corrupted after frees", i);
    }

    /* Second wave at a different size while the odd half is still live. */
    for (i = 0; i < N_SECOND; i++) {
        second[i] = ExAllocatePoolWithTag(16 * 1024, TAG_BIG);
        if (!second[i]) FAIL_AND_RETURN("second wave alloc %d failed", i);
        fill_pattern(second[i], 16 * 1024, (unsigned)(0x40 + i));
    }
    for (i = 0; i < N_SECOND; i++) {
        if (!check_pattern(second[i], 16 * 1024, (unsigned)(0x40 + i)))
            FAIL_AND_RETURN("second wave block %d corrupted", i);
    }
    for (i = 1; i < N_FIRST; i += 2) {
        if (!check_pattern(first[i], 8 * 1024, (unsigned)i))
            FAIL_AND_RETURN("first wave block %d corrupted by second wave", i);
    }

    for (i = 0; i < N_SECOND; i++) ExFreePool(second[i]);
    for (i = 1; i < N_FIRST; i += 2) ExFreePool(first[i]);
    return true;
}

/* Free-then-realloc cycles at big sizes: the free path must keep
 * returning pages the next allocation can claim. */
static bool t_bigpool_recycle(void)
{
    int i;
    for (i = 0; i < 48; i++) {
        PVOID p = ExAllocatePoolWithTag(32 * 1024, TAG_BIG);
        if (!p) FAIL_AND_RETURN("recycle alloc %d failed", i);
        fill_pattern(p, 32 * 1024, (unsigned)(0x80 + i));
        if (!check_pattern(p, 32 * 1024, (unsigned)(0x80 + i))) {
            ExFreePool(p);
            FAIL_AND_RETURN("recycle block %d corrupted", i);
        }
        ExFreePool(p);
    }
    return true;
}

/* Freed whole-page pool: is the VA still mapped afterwards?  This pins
 * whether the kernel unmaps big-block pool on free -- a release-after-
 * free in a driver faults on one behavior and silently reads garbage
 * on the other, so the allocator must match retail exactly. */
static bool t_bigpool_freed_unmapped(void)
{
    PVOID p = ExAllocatePoolWithTag(32 * 1024, TAG_BIG);
    BOOLEAN before, after;

    ASSERT_NOT_NULL(p);
    before = MmIsAddressValid(p);
    ExFreePool(p);
    after = MmIsAddressValid(p);

    tap_comment("bigpool: MmIsAddressValid before=%d after-free=%d",
                before, after);
    ASSERT_TRUE(before);
    /* Retail-validated below; see the --official run. */
    ASSERT_TRUE(!after);
    return true;
}

/* ExQueryPoolBlockSize over the small-block ladder: the returned size
 * is title-observable (the pool-size ordinal), so the bucket rounding
 * must match retail. */
static bool t_pool_query_sizes(void)
{
    static const ULONG Sizes[] = { 1, 8, 13, 16, 100, 1000, 4080 };
    ULONG Got[7];

    for (int i = 0; i < 7; i++) {
        PVOID p = ExAllocatePoolWithTag(Sizes[i], TAG_BIG);
        ASSERT_NOT_NULL(p);
        Got[i] = ExQueryPoolBlockSize(p);
        ExFreePool(p);
    }
    tap_comment("poolquery: req {1,8,13,16,100,1000,4080} -> "
                "{%lu,%lu,%lu,%lu,%lu,%lu,%lu}",
                (unsigned long)Got[0], (unsigned long)Got[1],
                (unsigned long)Got[2], (unsigned long)Got[3],
                (unsigned long)Got[4], (unsigned long)Got[5],
                (unsigned long)Got[6]);

    /* A query never reports less than the request. */
    for (int i = 0; i < 7; i++) {
        if (Got[i] < Sizes[i])
            FAIL_AND_RETURN("query for %lu returned %lu",
                            (unsigned long)Sizes[i], (unsigned long)Got[i]);
    }
    /* Big blocks report whole pages. */
    {
        PVOID p = ExAllocatePoolWithTag(5000, TAG_BIG);
        ULONG q;
        ASSERT_NOT_NULL(p);
        q = ExQueryPoolBlockSize(p);
        ExFreePool(p);
        tap_comment("poolquery: req 5000 -> %lu", (unsigned long)q);
        ASSERT_EQ_U32(q, 8192);
    }
    return true;
}

/* Freed small blocks live in pages the pool keeps; the VA must remain
 * valid (unlike freed whole-page blocks, which unmap). */
static bool t_pool_freed_small_still_mapped(void)
{
    PVOID p = ExAllocatePoolWithTag(64, TAG_BIG);
    BOOLEAN before, after;

    ASSERT_NOT_NULL(p);
    before = MmIsAddressValid(p);
    ExFreePool(p);
    after = MmIsAddressValid(p);
    tap_comment("poolquery: small block valid before=%d after-free=%d",
                before, after);
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    return true;
}

static const test_entry_t mm_bigpool_entries[] = {
    {"bigpool_alignment", t_bigpool_alignment},
    {"bigpool_churn",     t_bigpool_churn},
    {"bigpool_recycle",   t_bigpool_recycle},
    {"bigpool_freed_unmapped", t_bigpool_freed_unmapped},
    {"pool_query_sizes",  t_pool_query_sizes},
    {"pool_freed_small_still_mapped", t_pool_freed_small_still_mapped},
};

DEFINE_GROUP(mm_bigpool, "mm/bigpool");
