/*
 * Rtl integer/string-conversion and extended-integer-math exports whose
 * ordinals were unmapped bugcheck stubs.  The routines live in already
 * compiled translation units (unicode.c, largeint.c) and only needed an
 * export to reference them.  One test per export.
 */

#include "../harness.h"
#include <string.h>
#include <wchar.h>

static bool t_char_to_integer(void)
{
    ULONG v;

    ASSERT_NTSTATUS(RtlCharToInteger("12345", 10, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 12345);

    ASSERT_NTSTATUS(RtlCharToInteger("ff", 16, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 0xff);

    ASSERT_NTSTATUS(RtlCharToInteger("777", 8, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 0777);

    /* base 0 autodetects the 0x prefix. */
    ASSERT_NTSTATUS(RtlCharToInteger("0x1a", 0, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 0x1a);

    /* Leading whitespace and a sign are skipped. */
    ASSERT_NTSTATUS(RtlCharToInteger("  -100", 10, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, (ULONG)(-100));

    /* An unsupported base is rejected. */
    ASSERT_NTSTATUS(RtlCharToInteger("10", 3, &v), STATUS_INVALID_PARAMETER);
    return true;
}

static const test_entry_t rtl_intconv_entries[] = {
    {"char_to_integer", t_char_to_integer},
};

DEFINE_GROUP(rtl_intconv, "rtl/intconv");
