/*
 * The Rtl memory-family exports: RtlZeroMemory, RtlFillMemory,
 * RtlMoveMemory, RtlFillMemoryUlong, RtlCompareMemory, and
 * RtlCompareMemoryUlong.  Five of the six were unmapped bugcheck
 * stubs; the real functions were already linked.
 */

#include "../harness.h"
#include <string.h>

/*
 * nxdk's xboxkrnl.h macro-defines the first three to memset/memmove
 * (and only declares them when the macros are absent), which would
 * compile every call below into plain libc calls and never touch the
 * export table this group exists to verify.  Drop the macros and
 * declare the imports.
 */
#undef RtlZeroMemory
#undef RtlFillMemory
#undef RtlMoveMemory
XBAPI VOID NTAPI RtlZeroMemory(IN PVOID Destination, IN SIZE_T Length);
XBAPI VOID NTAPI RtlFillMemory(IN PVOID Destination, IN SIZE_T Length,
                               IN UCHAR Fill);
XBAPI VOID NTAPI RtlMoveMemory(IN PVOID Destination, IN const VOID *Source,
                               IN SIZE_T Length);

static void poison(void *buf, size_t len)
{
    memset(buf, 0xCC, len);
}

static bool all_bytes(const void *buf, size_t len, unsigned char v)
{
    const unsigned char *p = buf;
    for (size_t i = 0; i < len; i++)
        if (p[i] != v)
            return false;
    return true;
}

static bool t_zero_fills(void)
{
    static unsigned char big[4096];
    unsigned char small[16];

    poison(small, sizeof(small));
    RtlZeroMemory(small, sizeof(small));
    ASSERT_TRUE(all_bytes(small, sizeof(small), 0));

    poison(big, sizeof(big));
    RtlZeroMemory(big, sizeof(big));
    ASSERT_TRUE(all_bytes(big, sizeof(big), 0));
    return true;
}

static bool t_zero_length_noop(void)
{
    unsigned char buf[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    RtlZeroMemory(buf, 0);
    ASSERT_EQ_U32(buf[0], 0xAA);
    ASSERT_EQ_U32(buf[3], 0xDD);
    return true;
}

static bool t_zero_bounds(void)
{
    unsigned char buf[64];

    /* Unaligned start, odd length: neighbours must survive. */
    poison(buf, sizeof(buf));
    RtlZeroMemory(buf + 1, 31);
    ASSERT_EQ_U32(buf[0], 0xCC);
    ASSERT_TRUE(all_bytes(buf + 1, 31, 0));
    ASSERT_EQ_U32(buf[32], 0xCC);

    /* Single byte. */
    poison(buf, sizeof(buf));
    RtlZeroMemory(buf + 7, 1);
    ASSERT_EQ_U32(buf[6], 0xCC);
    ASSERT_EQ_U32(buf[7], 0);
    ASSERT_EQ_U32(buf[8], 0xCC);
    return true;
}

static bool t_fill_pattern(void)
{
    unsigned char buf[64];

    poison(buf, sizeof(buf));
    RtlFillMemory(buf, sizeof(buf), 0x5A);
    ASSERT_TRUE(all_bytes(buf, sizeof(buf), 0x5A));

    /* Partial fill: bounds preserved. */
    poison(buf, sizeof(buf));
    RtlFillMemory(buf + 8, 16, 0xE7);
    ASSERT_EQ_U32(buf[7], 0xCC);
    ASSERT_TRUE(all_bytes(buf + 8, 16, 0xE7));
    ASSERT_EQ_U32(buf[24], 0xCC);
    return true;
}

static bool t_fill_ulong(void)
{
    ULONG buf[10];

    memset(buf, 0xCC, sizeof(buf));
    /* Length is in bytes; fill the middle 8 ULONGs. */
    RtlFillMemoryUlong(buf + 1, 8 * sizeof(ULONG), 0xA1B2C3D4);
    ASSERT_EQ_U32(buf[0], 0xCCCCCCCC);
    for (int i = 1; i < 9; i++)
        ASSERT_EQ_U32(buf[i], 0xA1B2C3D4);
    ASSERT_EQ_U32(buf[9], 0xCCCCCCCC);
    return true;
}

static bool t_move_overlap(void)
{
    unsigned char buf[32];

    /* Plain copy. */
    for (int i = 0; i < 16; i++)
        buf[i] = (unsigned char)i;
    RtlMoveMemory(buf + 16, buf, 16);
    ASSERT_TRUE(memcmp(buf, buf + 16, 16) == 0);

    /* Overlapping move forward (dst > src): memmove semantics. */
    for (int i = 0; i < 24; i++)
        buf[i] = (unsigned char)i;
    RtlMoveMemory(buf + 8, buf, 16);
    for (int i = 0; i < 16; i++)
        ASSERT_EQ_U32(buf[8 + i], i);

    /* Overlapping move backward (dst < src). */
    for (int i = 0; i < 24; i++)
        buf[i] = (unsigned char)i;
    RtlMoveMemory(buf, buf + 8, 16);
    for (int i = 0; i < 16; i++)
        ASSERT_EQ_U32(buf[i], 8 + i);
    return true;
}

static bool t_compare_memory(void)
{
    unsigned char a[16], b[16];

    for (int i = 0; i < 16; i++)
        a[i] = b[i] = (unsigned char)(i * 3);

    /* Full match returns the whole length. */
    ASSERT_EQ_U32((ULONG)RtlCompareMemory(a, b, sizeof(a)), sizeof(a));

    /* First difference bounds the count. */
    b[9] ^= 0xFF;
    ASSERT_EQ_U32((ULONG)RtlCompareMemory(a, b, sizeof(a)), 9);
    ASSERT_EQ_U32((ULONG)RtlCompareMemory(a, b, 4), 4);
    return true;
}

static bool t_compare_ulong(void)
{
    ULONG a[6] = { 1, 2, 3, 4, 5, 6 };

    /* Counts matching bytes against a single pattern, ULONG-stepped. */
    ULONG same[4] = { 7, 7, 7, 7 };
    ASSERT_EQ_U32((ULONG)RtlCompareMemoryUlong(same, sizeof(same), 7),
                  sizeof(same));
    ASSERT_EQ_U32((ULONG)RtlCompareMemoryUlong(a, sizeof(a), 1),
                  sizeof(ULONG));
    ASSERT_EQ_U32((ULONG)RtlCompareMemoryUlong(a, sizeof(a), 99), 0);
    return true;
}

static const test_entry_t rtl_memory_entries[] = {
    {"zero_fills",       t_zero_fills},
    {"zero_length_noop", t_zero_length_noop},
    {"zero_bounds",      t_zero_bounds},
    {"fill_pattern",     t_fill_pattern},
    {"fill_ulong",       t_fill_ulong},
    {"move_overlap",     t_move_overlap},
    {"compare_memory",   t_compare_memory},
    {"compare_ulong",    t_compare_ulong},
};

DEFINE_GROUP(rtl_memory, "rtl/memory");
