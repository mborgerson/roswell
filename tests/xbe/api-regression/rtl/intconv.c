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

static bool t_integer_to_char(void)
{
    char buf[16];

    ASSERT_NTSTATUS(RtlIntegerToChar(12345, 10, sizeof(buf), buf),
                    STATUS_SUCCESS);
    ASSERT_TRUE(strcmp(buf, "12345") == 0);

    ASSERT_NTSTATUS(RtlIntegerToChar(0xdead, 16, sizeof(buf), buf),
                    STATUS_SUCCESS);
    ASSERT_TRUE(strcmp(buf, "DEAD") == 0);

    /* base 0 behaves as base 10. */
    ASSERT_NTSTATUS(RtlIntegerToChar(42, 0, sizeof(buf), buf),
                    STATUS_SUCCESS);
    ASSERT_TRUE(strcmp(buf, "42") == 0);

    /* Too small a buffer overflows. */
    ASSERT_NTSTATUS(RtlIntegerToChar(12345, 10, 3, buf),
                    STATUS_BUFFER_OVERFLOW);
    return true;
}

static bool t_integer_to_unicode_string(void)
{
    UNICODE_STRING u;
    WCHAR buf[16];

    u.Length = 0;
    u.MaximumLength = sizeof(buf);
    u.Buffer = buf;

    ASSERT_NTSTATUS(RtlIntegerToUnicodeString(65535, 10, &u), STATUS_SUCCESS);
    ASSERT_EQ_U32(u.Length, 5 * sizeof(WCHAR));
    ASSERT_TRUE(wcsncmp(u.Buffer, L"65535", 5) == 0);

    ASSERT_NTSTATUS(RtlIntegerToUnicodeString(0x100, 16, &u), STATUS_SUCCESS);
    ASSERT_TRUE(wcsncmp(u.Buffer, L"100", 3) == 0);
    return true;
}

static bool t_compare_string(void)
{
    ANSI_STRING a, b;

    RtlInitAnsiString(&a, "alpha");
    RtlInitAnsiString(&b, "alpha");
    ASSERT_EQ_U32(RtlCompareString(&a, &b, FALSE), 0);

    RtlInitAnsiString(&b, "alphb");
    ASSERT_TRUE(RtlCompareString(&a, &b, FALSE) < 0);

    /* Case-insensitive compare folds case. */
    RtlInitAnsiString(&b, "ALPHA");
    ASSERT_TRUE(RtlCompareString(&a, &b, FALSE) != 0);
    ASSERT_EQ_U32(RtlCompareString(&a, &b, TRUE), 0);

    /* Equal prefix, differing length orders by length. */
    RtlInitAnsiString(&b, "alp");
    ASSERT_TRUE(RtlCompareString(&a, &b, FALSE) > 0);
    return true;
}

static bool t_copy_string(void)
{
    ANSI_STRING src, dst;
    char dstbuf[16];

    RtlInitAnsiString(&src, "content");
    dst.Length = 0;
    dst.MaximumLength = sizeof(dstbuf);
    dst.Buffer = dstbuf;

    RtlCopyString(&dst, &src);
    ASSERT_EQ_U32(dst.Length, 7);
    ASSERT_TRUE(strncmp(dst.Buffer, "content", 7) == 0);

    /* A NULL source empties the destination. */
    RtlCopyString(&dst, NULL);
    ASSERT_EQ_U32(dst.Length, 0);

    /* Copy truncates to the destination's MaximumLength. */
    char smallbuf[3];
    ANSI_STRING small = { 0, sizeof(smallbuf), smallbuf };
    RtlCopyString(&small, &src);
    ASSERT_EQ_U32(small.Length, 3);
    ASSERT_TRUE(strncmp(small.Buffer, "con", 3) == 0);
    return true;
}

static const test_entry_t rtl_intconv_entries[] = {
    {"char_to_integer", t_char_to_integer},
    {"integer_to_char", t_integer_to_char},
    {"integer_to_unicode_string", t_integer_to_unicode_string},
    {"compare_string", t_compare_string},
    {"copy_string", t_copy_string},
};

DEFINE_GROUP(rtl_intconv, "rtl/intconv");
