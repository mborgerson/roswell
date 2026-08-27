/*
 * The interlocked ordinals a title can reach: the 64-bit statistic add
 * and compare-exchange, and the plain 32-bit compare-exchange and
 * exchange-add.
 *
 * All of them are __fastcall on the console, which is the detail worth
 * covering -- the first two arguments arrive in ECX/EDX and any further
 * one on the stack, so a wrapper declared with the wrong convention
 * reads garbage for the tail arguments rather than failing to link.
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

static bool t_compare_exchange64(void)
{
    LONGLONG dest, exch, cmp, prev;

    /* Comparand matches: destination is replaced, old value returned. */
    dest = 100; exch = 999; cmp = 100;
    prev = ExInterlockedCompareExchange64(&dest, &exch, &cmp);
    ASSERT_TRUE(prev == 100);
    ASSERT_TRUE(dest == 999);

    /* Comparand mismatches: destination untouched, old value returned. */
    dest = 100; exch = 999; cmp = 50;
    prev = ExInterlockedCompareExchange64(&dest, &exch, &cmp);
    ASSERT_TRUE(prev == 100);
    ASSERT_TRUE(dest == 100);

    /* Full 64-bit compare/exchange across the 32-bit boundary. */
    dest = 0x00000001DEADBEEFULL;
    exch = 0xFEEDFACE12345678ULL;
    cmp  = 0x00000001DEADBEEFULL;
    prev = ExInterlockedCompareExchange64(&dest, &exch, &cmp);
    ASSERT_TRUE((ULONGLONG)prev == 0x00000001DEADBEEFULL);
    ASSERT_TRUE((ULONGLONG)dest == 0xFEEDFACE12345678ULL);

    /* High-dword-only difference is detected (no exchange). */
    dest = 0x0000000100000000ULL;
    exch = 0x1111111111111111ULL;
    cmp  = 0x0000000200000000ULL;
    prev = ExInterlockedCompareExchange64(&dest, &exch, &cmp);
    ASSERT_TRUE((ULONGLONG)prev == 0x0000000100000000ULL);
    ASSERT_TRUE((ULONGLONG)dest == 0x0000000100000000ULL);
    return true;
}

/* The plain fastcall InterlockedCompareExchange / InterlockedExchangeAdd
 * at ordinals 51 and 55.  Argument ORDER is the thing worth pinning:
 * both of CompareExchange's value arguments are plain LONGs, so swapping
 * Exchange and Comparand still compiles, still links, and fails only on
 * the values -- silently, and only when a comparison happens to match. */
static bool t_compare_exchange(void)
{
    LONG dest, prev;

    /* Comparand matches: the exchange happens and the OLD value comes
     * back.  A swapped Exchange/Comparand pair would store 0x1111 here
     * instead of 0x2222. */
    dest = 0x1111;
    prev = InterlockedCompareExchange(&dest, 0x2222, 0x1111);
    ASSERT_EQ_U32(prev, 0x1111);
    ASSERT_EQ_U32(dest, 0x2222);

    /* Comparand does not match: the destination is left alone and the
     * current value is still what comes back. */
    dest = 0x1111;
    prev = InterlockedCompareExchange(&dest, 0x2222, 0x3333);
    ASSERT_EQ_U32(prev, 0x1111);
    ASSERT_EQ_U32(dest, 0x1111);

    /* Zero is a legitimate comparand, not an "unset" sentinel. */
    dest = 0;
    prev = InterlockedCompareExchange(&dest, 0x4444, 0);
    ASSERT_EQ_U32(prev, 0);
    ASSERT_EQ_U32(dest, 0x4444);

    /* The sign bit survives the round trip in both arguments. */
    dest = -1;
    prev = InterlockedCompareExchange(&dest, 0x7FFFFFFF, -1);
    ASSERT_EQ_U32(prev, 0xFFFFFFFFu);
    ASSERT_EQ_U32(dest, 0x7FFFFFFF);
    return true;
}

static bool t_exchange_add(void)
{
    LONG addend, prev;

    /* Returns the value from BEFORE the add. */
    addend = 100;
    prev = InterlockedExchangeAdd(&addend, 23);
    ASSERT_EQ_U32(prev, 100);
    ASSERT_EQ_U32(addend, 123);

    /* A negative increment subtracts. */
    addend = 50;
    prev = InterlockedExchangeAdd(&addend, -20);
    ASSERT_EQ_U32(prev, 50);
    ASSERT_EQ_U32(addend, 30);

    /* Adding zero reports the current value and changes nothing. */
    addend = 0x12345678;
    prev = InterlockedExchangeAdd(&addend, 0);
    ASSERT_EQ_U32(prev, 0x12345678);
    ASSERT_EQ_U32(addend, 0x12345678);

    /* Wraps rather than saturating. */
    addend = (LONG)0x7FFFFFFF;
    prev = InterlockedExchangeAdd(&addend, 1);
    ASSERT_EQ_U32(prev, 0x7FFFFFFFu);
    ASSERT_EQ_U32(addend, 0x80000000u);
    return true;
}

static const test_entry_t ex_interlocked_entries[] = {
    {"add_large_statistic", t_add_large_statistic},
    {"compare_exchange64", t_compare_exchange64},
    {"compare_exchange",   t_compare_exchange},
    {"exchange_add",       t_exchange_add},
};

DEFINE_GROUP(ex_interlocked, "ex/interlocked");
