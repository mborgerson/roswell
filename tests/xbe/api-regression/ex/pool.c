/*
 * ExAllocatePoolWithTag + ExFreePool.
 */

#include "../harness.h"

#define TAG_TEST  'tseT'

static bool t_alloc_small(void)
{
    PVOID p = ExAllocatePoolWithTag(64, TAG_TEST);
    ASSERT_NOT_NULL(p);
    /* Round-trip a known pattern. */
    unsigned char *b = (unsigned char *)p;
    for (int i = 0; i < 64; i++) b[i] = (unsigned char)(i ^ 0xA5);
    for (int i = 0; i < 64; i++) {
        if (b[i] != (unsigned char)(i ^ 0xA5)) {
            ExFreePool(p);
            FAIL_AND_RETURN("byte %d corrupted", i);
        }
    }
    ExFreePool(p);
    return true;
}

static bool t_alloc_repeated(void)
{
    /* Allocate, free, allocate again; verify the pool keeps working. */
    for (int i = 0; i < 32; i++) {
        PVOID p = ExAllocatePoolWithTag(0x100, TAG_TEST);
        if (!p) FAIL_AND_RETURN("alloc %d failed", i);
        ExFreePool(p);
    }
    return true;
}

static bool t_alloc_page_sized(void)
{
    PVOID p = ExAllocatePoolWithTag(4096, TAG_TEST);
    ASSERT_NOT_NULL(p);
    ExFreePool(p);
    return true;
}

static bool t_alloc_large(void)
{
    PVOID p = ExAllocatePoolWithTag(64 * 1024, TAG_TEST);
    ASSERT_NOT_NULL(p);
    ExFreePool(p);
    return true;
}

static const test_entry_t ex_pool_entries[] = {
    {"alloc_small",      t_alloc_small},
    {"alloc_repeated",   t_alloc_repeated},
    {"alloc_page_sized", t_alloc_page_sized},
    {"alloc_large",      t_alloc_large},
};

DEFINE_GROUP(ex_pool, "ex/pool");
