/*
 * RtlZeroMemory – verify the kernel export (ordinal 320) zeroes memory
 * correctly for various sizes and alignments.
 */

#include "../harness.h"
#include <string.h>

/* Helper: fill buffer with a non-zero pattern so we can detect zeroing. */
static void poison(void *buf, size_t len)
{
    memset(buf, 0xCC, len);
}

/* Helper: return true if every byte in [buf, buf+len) is zero. */
static bool all_zero(const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    for (size_t i = 0; i < len; i++) {
        if (p[i] != 0) return false;
    }
    return true;
}

/* --- individual tests --------------------------------------------------- */

static bool t_zero_small(void)
{
    unsigned char buf[16];
    poison(buf, sizeof(buf));
    RtlZeroMemory(buf, sizeof(buf));
    ASSERT_TRUE(all_zero(buf, sizeof(buf)));
    return true;
}

static bool t_zero_single_byte(void)
{
    unsigned char b = 0xAB;
    RtlZeroMemory(&b, 1);
    ASSERT_EQ_U32(b, 0);
    return true;
}

static bool t_zero_length(void)
{
    unsigned char buf[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    RtlZeroMemory(buf, 0);
    /* Nothing should change. */
    ASSERT_EQ_U32(buf[0], 0xAA);
    ASSERT_EQ_U32(buf[3], 0xDD);
    return true;
}

static bool t_zero_large(void)
{
    /* 4 KiB – exercises the ULONG-at-a-time fast path. */
    unsigned char buf[4096];
    poison(buf, sizeof(buf));
    RtlZeroMemory(buf, sizeof(buf));
    ASSERT_TRUE(all_zero(buf, sizeof(buf)));
    return true;
}

static bool t_zero_unaligned(void)
{
    unsigned char buf[64];
    poison(buf, sizeof(buf));
    /* Start one byte past a ULONG boundary. */
    RtlZeroMemory(buf + 1, 31);
    /* buf[0] must still be poisoned. */
    ASSERT_EQ_U32(buf[0], 0xCC);
    /* The 31 bytes starting at buf[1] must be zero. */
    ASSERT_TRUE(all_zero(buf + 1, 31));
    /* buf[32] must still be poisoned. */
    ASSERT_EQ_U32(buf[32], 0xCC);
    return true;
}

static bool t_zero_partial(void)
{
    unsigned char buf[16];
    poison(buf, sizeof(buf));
    RtlZeroMemory(buf + 4, 8);
    /* First 4 bytes untouched. */
    ASSERT_EQ_U32(buf[0], 0xCC);
    ASSERT_EQ_U32(buf[3], 0xCC);
    /* Middle 8 bytes zeroed. */
    ASSERT_TRUE(all_zero(buf + 4, 8));
    /* Last 4 bytes untouched. */
    ASSERT_EQ_U32(buf[12], 0xCC);
    ASSERT_EQ_U32(buf[15], 0xCC);
    return true;
}

/* --- group definition --------------------------------------------------- */

static const test_entry_t rtl_zeromemory_entries[] = {
    {"small",       t_zero_small},
    {"single_byte", t_zero_single_byte},
    {"zero_length", t_zero_length},
    {"large",       t_zero_large},
    {"unaligned",   t_zero_unaligned},
    {"partial",     t_zero_partial},
};

DEFINE_GROUP(rtl_zeromemory, "rtl/zeromemory");
