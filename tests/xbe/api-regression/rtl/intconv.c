/*
 * Rtl integer/string-conversion and extended-integer-math exports whose
 * ordinals were unmapped bugcheck stubs.  The routines live in already
 * compiled translation units (unicode.c, largeint.c) and only needed an
 * export to reference them.  One test per export.
 */

/* The vendored nxdk header decorates this one with XBAPI where NTAPI
 * belongs, so its declaration comes out cdecl and cannot resolve the
 * @12 stdcall export.  Rename it out of the way on the way in and
 * declare the shape the import library carries. */
#define RtlUnicodeStringToInteger nxdk_RtlUnicodeStringToInteger
#include "../harness.h"
#undef RtlUnicodeStringToInteger
#include <string.h>
#include <wchar.h>

__declspec(dllimport) NTSTATUS NTAPI RtlUnicodeStringToInteger
(
    PUNICODE_STRING String,
    ULONG Base,
    PULONG Value
);

/* The counted-string counterpart of RtlCharToInteger: same bases, same
 * autodetection, and it stops at the string's length rather than at a
 * terminator. */
static bool t_unicode_string_to_integer(void)
{
    UNICODE_STRING s;
    ULONG v;

    RtlInitUnicodeString(&s, L"12345");
    ASSERT_NTSTATUS(RtlUnicodeStringToInteger(&s, 10, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 12345);

    RtlInitUnicodeString(&s, L"ff");
    ASSERT_NTSTATUS(RtlUnicodeStringToInteger(&s, 16, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 0xff);

    RtlInitUnicodeString(&s, L"777");
    ASSERT_NTSTATUS(RtlUnicodeStringToInteger(&s, 8, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 0777);

    /* base 0 autodetects the prefix, as the char form does. */
    RtlInitUnicodeString(&s, L"0x1f");
    ASSERT_NTSTATUS(RtlUnicodeStringToInteger(&s, 0, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 0x1f);

    /* A leading sign is taken. */
    RtlInitUnicodeString(&s, L"-5");
    ASSERT_NTSTATUS(RtlUnicodeStringToInteger(&s, 10, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, (ULONG)-5);

    /* The length bounds the scan: the digits past it are not read. */
    RtlInitUnicodeString(&s, L"1234");
    s.Length = 2 * sizeof(WCHAR);
    ASSERT_NTSTATUS(RtlUnicodeStringToInteger(&s, 10, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 12);

    /* A digit the base does not have ends the number. */
    RtlInitUnicodeString(&s, L"12a");
    ASSERT_NTSTATUS(RtlUnicodeStringToInteger(&s, 10, &v), STATUS_SUCCESS);
    ASSERT_EQ_U32(v, 12);
    return true;
}

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

static bool t_upper_string(void)
{
    ANSI_STRING src, dst;
    char dstbuf[16];

    RtlInitAnsiString(&src, "MixedCase!");
    dst.Length = 0;
    dst.MaximumLength = sizeof(dstbuf);
    dst.Buffer = dstbuf;

    RtlUpperString(&dst, &src);
    ASSERT_EQ_U32(dst.Length, src.Length);
    ASSERT_TRUE(strncmp(dst.Buffer, "MIXEDCASE!", 10) == 0);
    return true;
}

static bool t_append_string_to_string(void)
{
    ANSI_STRING d, s;
    char buf[16];

    d.Length = 0;
    d.MaximumLength = sizeof(buf);
    d.Buffer = buf;

    RtlInitAnsiString(&s, "head");
    ASSERT_NTSTATUS(RtlAppendStringToString(&d, &s), STATUS_SUCCESS);
    RtlInitAnsiString(&s, "-tail");
    ASSERT_NTSTATUS(RtlAppendStringToString(&d, &s), STATUS_SUCCESS);
    ASSERT_EQ_U32(d.Length, 9);
    ASSERT_TRUE(strncmp(d.Buffer, "head-tail", 9) == 0);

    /* Overflow past MaximumLength is rejected. */
    char tiny[4];
    ANSI_STRING t = { 0, sizeof(tiny), tiny };
    RtlInitAnsiString(&s, "toolong");
    ASSERT_NTSTATUS(RtlAppendStringToString(&t, &s),
                    STATUS_BUFFER_TOO_SMALL);
    return true;
}

static bool t_free_ansi_string(void)
{
    UNICODE_STRING u;
    ANSI_STRING a;

    RtlInitUnicodeString(&u, L"deallocate");
    ASSERT_NTSTATUS(RtlUnicodeStringToAnsiString(&a, &u, TRUE),
                    STATUS_SUCCESS);
    ASSERT_NOT_NULL(a.Buffer);
    ASSERT_EQ_U32(a.Length, 10);

    RtlFreeAnsiString(&a);
    ASSERT_TRUE(a.Buffer == NULL);
    ASSERT_EQ_U32(a.Length, 0);
    return true;
}

static bool t_extended_integer_multiply(void)
{
    LARGE_INTEGER m, r;

    m.QuadPart = 1000000;
    r = RtlExtendedIntegerMultiply(m, 1000);
    ASSERT_TRUE(r.QuadPart == 1000000000LL);

    r = RtlExtendedIntegerMultiply(m, -3);
    ASSERT_TRUE(r.QuadPart == -3000000LL);
    return true;
}

static bool t_extended_large_integer_divide(void)
{
    LARGE_INTEGER d, r;
    ULONG rem = 0;

    d.QuadPart = 1000000007LL;
    r = RtlExtendedLargeIntegerDivide(d, 7, &rem);
    ASSERT_TRUE(r.QuadPart == 142857143LL);
    ASSERT_EQ_U32(rem, 6);

    /* A NULL remainder pointer is tolerated. */
    r = RtlExtendedLargeIntegerDivide(d, 1000, NULL);
    ASSERT_TRUE(r.QuadPart == 1000000LL);
    return true;
}

static bool t_extended_magic_divide(void)
{
    LARGE_INTEGER d, magic, r;

    /* magic/shift chosen so the result equals Dividend / 3. */
    magic.QuadPart = (LONGLONG)0xAAAAAAAAAAAAAAABULL;

    d.QuadPart = 30;
    r = RtlExtendedMagicDivide(d, magic, 1);
    ASSERT_TRUE(r.QuadPart == 10LL);

    d.QuadPart = 99;
    r = RtlExtendedMagicDivide(d, magic, 1);
    ASSERT_TRUE(r.QuadPart == 33LL);

    d.QuadPart = -30;
    r = RtlExtendedMagicDivide(d, magic, 1);
    ASSERT_TRUE(r.QuadPart == -10LL);
    return true;
}

static const test_entry_t rtl_intconv_entries[] = {
    {"char_to_integer", t_char_to_integer},
    {"unicode_string_to_integer", t_unicode_string_to_integer},
    {"integer_to_char", t_integer_to_char},
    {"integer_to_unicode_string", t_integer_to_unicode_string},
    {"compare_string", t_compare_string},
    {"copy_string", t_copy_string},
    {"upper_string", t_upper_string},
    {"append_string_to_string", t_append_string_to_string},
    {"free_ansi_string", t_free_ansi_string},
    {"extended_integer_multiply", t_extended_integer_multiply},
    {"extended_large_integer_divide", t_extended_large_integer_divide},
    {"extended_magic_divide", t_extended_magic_divide},
};

DEFINE_GROUP(rtl_intconv, "rtl/intconv");
