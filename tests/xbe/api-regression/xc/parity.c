/*
 * XcDESKeyParity -- forces odd parity on each key byte (the low bit is the
 * parity bit; the upper seven bits are key material and stay put).  The
 * export was a no-op stub; the kernel now performs the real fixup.  These
 * cases assert the two invariants that fully characterise it, plus a few
 * explicit vectors, and were confirmed against the retail kernel.
 */

#include "../harness.h"

/* Odd parity: an odd number of set bits in the byte. */
static bool byte_is_odd_parity(unsigned char b)
{
    unsigned char p = b;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    return (p & 1) != 0;
}

/* Every byte value round-trips to odd parity, upper seven bits preserved. */
static bool t_all_bytes(void)
{
    unsigned char buf[256];
    unsigned int i;

    for (i = 0; i < 256; i++) buf[i] = (unsigned char)i;
    XcDESKeyParity(buf, sizeof(buf));

    for (i = 0; i < 256; i++) {
        if (!byte_is_odd_parity(buf[i]))
            FAIL_AND_RETURN("byte 0x%02x -> 0x%02x is not odd parity",
                            i, buf[i]);
        if ((buf[i] & 0xFE) != (unsigned char)(i & 0xFE))
            FAIL_AND_RETURN("byte 0x%02x -> 0x%02x changed the top 7 bits",
                            i, buf[i]);
    }
    return true;
}

/* Bytes that already have odd parity are left unchanged. */
static bool t_already_odd(void)
{
    unsigned char buf[4] = {0x01, 0x02, 0x7F, 0x80};
    XcDESKeyParity(buf, sizeof(buf));
    ASSERT_EQ_U32(buf[0], 0x01);
    ASSERT_EQ_U32(buf[1], 0x02);
    ASSERT_EQ_U32(buf[2], 0x7F);
    ASSERT_EQ_U32(buf[3], 0x80);   /* 0x80 is one bit -> already odd */
    return true;
}

/* Explicit fixups: only bit 0 moves to make the count odd. */
static bool t_known_vectors(void)
{
    unsigned char buf[6] = {0x00, 0x03, 0xF0, 0xFF, 0x88, 0xAA};
    XcDESKeyParity(buf, sizeof(buf));
    ASSERT_EQ_U32(buf[0], 0x01);   /* 0 ones  -> set bit 0 */
    ASSERT_EQ_U32(buf[1], 0x02);   /* 0x03 -> clear bit 0, top7 already odd */
    ASSERT_EQ_U32(buf[2], 0xF1);   /* 4 ones  -> set bit 0 */
    ASSERT_EQ_U32(buf[3], 0xFE);   /* 8 ones  -> clear bit 0 */
    ASSERT_EQ_U32(buf[4], 0x89);
    ASSERT_EQ_U32(buf[5], 0xAB);
    return true;
}

/* A zero length must touch nothing. */
static bool t_zero_length(void)
{
    unsigned char buf[2] = {0x00, 0xFF};
    XcDESKeyParity(buf, 0);
    ASSERT_EQ_U32(buf[0], 0x00);
    ASSERT_EQ_U32(buf[1], 0xFF);
    return true;
}

static const test_entry_t xc_parity_entries[] = {
    {"all_bytes_odd_parity", t_all_bytes},
    {"already_odd_unchanged", t_already_odd},
    {"known_vectors", t_known_vectors},
    {"zero_length_noop", t_zero_length},
};

DEFINE_GROUP(xc_parity, "xc/parity");
