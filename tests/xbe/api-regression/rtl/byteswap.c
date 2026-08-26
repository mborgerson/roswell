/*
 * Rtl*ByteSwap exports -- FASTCALL endianness reversal of a 16/32-bit
 * value.  Their ordinals were unmapped bugcheck stubs; the kernel now
 * implements them directly (no NT/ReactOS source ships them as real
 * functions -- they are compiler intrinsics there).  One test per export.
 */

#include "../harness.h"

static bool t_ushort_byte_swap(void)
{
    ASSERT_EQ_U32(RtlUshortByteSwap(0x1234), 0x3412);
    ASSERT_EQ_U32(RtlUshortByteSwap(0x00ff), 0xff00);
    ASSERT_EQ_U32(RtlUshortByteSwap(0xff00), 0x00ff);
    ASSERT_EQ_U32(RtlUshortByteSwap(0x0000), 0x0000);
    ASSERT_EQ_U32(RtlUshortByteSwap(0xabcd), 0xcdab);
    /* A second swap restores the original. */
    ASSERT_EQ_U32(RtlUshortByteSwap(RtlUshortByteSwap(0x1234)), 0x1234);
    return true;
}

static bool t_ulong_byte_swap(void)
{
    ASSERT_EQ_U32(RtlUlongByteSwap(0x12345678UL), 0x78563412UL);
    ASSERT_EQ_U32(RtlUlongByteSwap(0x000000ffUL), 0xff000000UL);
    ASSERT_EQ_U32(RtlUlongByteSwap(0xff000000UL), 0x000000ffUL);
    ASSERT_EQ_U32(RtlUlongByteSwap(0x00000000UL), 0x00000000UL);
    ASSERT_EQ_U32(RtlUlongByteSwap(0xdeadbeefUL), 0xefbeaddeUL);
    /* A second swap restores the original. */
    ASSERT_EQ_U32(RtlUlongByteSwap(RtlUlongByteSwap(0x12345678UL)),
                  0x12345678UL);
    return true;
}

static const test_entry_t rtl_byteswap_entries[] = {
    {"ushort_byte_swap", t_ushort_byte_swap},
    {"ulong_byte_swap", t_ulong_byte_swap},
};

DEFINE_GROUP(rtl_byteswap, "rtl/byteswap");
