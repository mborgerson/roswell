/*
 * Rtl string-conversion and Unicode-builder exports whose ordinals
 * were unmapped bugcheck stubs even though the real routines are
 * already linked into the kernel.  One test per export.
 */

#include "../harness.h"
#include <string.h>
#include <wchar.h>

static bool t_upper_char(void)
{
    ASSERT_EQ_U32((unsigned char)RtlUpperChar('a'), 'A');
    ASSERT_EQ_U32((unsigned char)RtlUpperChar('m'), 'M');
    ASSERT_EQ_U32((unsigned char)RtlUpperChar('z'), 'Z');
    /* Already-upper and non-letters pass through unchanged. */
    ASSERT_EQ_U32((unsigned char)RtlUpperChar('A'), 'A');
    ASSERT_EQ_U32((unsigned char)RtlUpperChar('5'), '5');
    ASSERT_EQ_U32((unsigned char)RtlUpperChar('-'), '-');
    return true;
}

static const test_entry_t rtl_strconv_entries[] = {
    {"upper_char", t_upper_char},
};

DEFINE_GROUP(rtl_strconv, "rtl/strconv");
