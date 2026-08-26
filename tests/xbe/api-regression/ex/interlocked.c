/*
 * ExInterlockedAddLargeStatistic -- atomically add a ULONG increment to a
 * 64-bit statistic counter.  The ordinal was an unmapped bugcheck stub; the
 * routine is already in the kernel (ex asm), matches the Xbox FASTCALL @8
 * ABI exactly, and only needed an export to reference it.
 */

#include "../harness.h"

static bool t_add_large_statistic(void)
{
    LARGE_INTEGER v;

    /* Simple add within the low dword. */
    v.QuadPart = 100;
    ExInterlockedAddLargeStatistic(&v, 23);
    ASSERT_TRUE(v.QuadPart == 123);

    /* Carry out of the low 32 bits into the high dword. */
    v.QuadPart = 0xFFFFFFFFULL;
    ExInterlockedAddLargeStatistic(&v, 1);
    ASSERT_TRUE(v.QuadPart == 0x100000000ULL);

    /* Add while the high dword is already set (no carry). */
    v.QuadPart = 0x0000000100000005ULL;
    ExInterlockedAddLargeStatistic(&v, 10);
    ASSERT_TRUE(v.QuadPart == 0x000000010000000FULL);

    /* Adding zero leaves the value unchanged. */
    v.QuadPart = 0x00000002DEADBEEFULL;
    ExInterlockedAddLargeStatistic(&v, 0);
    ASSERT_TRUE(v.QuadPart == 0x00000002DEADBEEFULL);

    /* Repeated adds accumulate, including across the boundary. */
    v.QuadPart = 0xFFFFFFF0ULL;
    for (int i = 0; i < 32; i++)
        ExInterlockedAddLargeStatistic(&v, 1);
    ASSERT_TRUE(v.QuadPart == 0x100000010ULL);
    return true;
}

static const test_entry_t ex_interlocked_entries[] = {
    {"add_large_statistic", t_add_large_statistic},
};

DEFINE_GROUP(ex_interlocked, "ex/interlocked");
