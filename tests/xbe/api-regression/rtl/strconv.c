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

static bool t_copy_unicode(void)
{
    UNICODE_STRING src, dst;
    WCHAR dstbuf[16];

    RtlInitUnicodeString(&src, L"payload");
    dst.Length = 0;
    dst.MaximumLength = sizeof(dstbuf);
    dst.Buffer = dstbuf;

    RtlCopyUnicodeString(&dst, &src);
    ASSERT_EQ_U32(dst.Length, src.Length);            /* 7 * 2 */
    ASSERT_TRUE(wcsncmp(dst.Buffer, L"payload", 7) == 0);

    /* Copy truncates to the destination's MaximumLength. */
    WCHAR smallbuf[4];
    UNICODE_STRING small = { 0, sizeof(smallbuf), smallbuf };
    RtlCopyUnicodeString(&small, &src);
    ASSERT_EQ_U32(small.Length, sizeof(smallbuf));    /* 4 bytes = 2 chars */
    ASSERT_TRUE(wcsncmp(small.Buffer, L"pa", 2) == 0);
    return true;
}

static bool t_upcase_unicode_inplace(void)
{
    UNICODE_STRING src, dst;
    WCHAR srcbuf[] = L"MixedCase123";
    WCHAR dstbuf[16];

    RtlInitUnicodeString(&src, srcbuf);
    dst.Length = 0;
    dst.MaximumLength = sizeof(dstbuf);
    dst.Buffer = dstbuf;

    ASSERT_NTSTATUS(RtlUpcaseUnicodeString(&dst, &src, FALSE),
                    STATUS_SUCCESS);
    ASSERT_EQ_U32(dst.Length, src.Length);
    ASSERT_TRUE(wcsncmp(dst.Buffer, L"MIXEDCASE123", 12) == 0);
    return true;
}

static bool t_upcase_unicode_alloc(void)
{
    UNICODE_STRING src, dst;
    RtlInitUnicodeString(&src, L"hello");

    ASSERT_NTSTATUS(RtlUpcaseUnicodeString(&dst, &src, TRUE),
                    STATUS_SUCCESS);
    ASSERT_NOT_NULL(dst.Buffer);
    ASSERT_EQ_U32(dst.Length, src.Length);
    ASSERT_TRUE(wcsncmp(dst.Buffer, L"HELLO", 5) == 0);
    RtlFreeUnicodeString(&dst);
    return true;
}

static bool t_create_unicode(void)
{
    UNICODE_STRING u;
    ASSERT_TRUE(RtlCreateUnicodeString(&u, L"created"));
    ASSERT_NOT_NULL(u.Buffer);
    ASSERT_EQ_U32(u.Length, 7 * sizeof(WCHAR));
    /* MaximumLength includes the NUL terminator. */
    ASSERT_EQ_U32(u.MaximumLength, 8 * sizeof(WCHAR));
    ASSERT_TRUE(wcscmp(u.Buffer, L"created") == 0);
    RtlFreeUnicodeString(&u);
    return true;
}

static bool t_append_unicode_to_string(void)
{
    UNICODE_STRING d;
    WCHAR buf[32];

    d.Length = 0;
    d.MaximumLength = sizeof(buf);
    d.Buffer = buf;

    ASSERT_NTSTATUS(RtlAppendUnicodeToString(&d, L"foo"), STATUS_SUCCESS);
    ASSERT_NTSTATUS(RtlAppendUnicodeToString(&d, L"bar"), STATUS_SUCCESS);
    ASSERT_EQ_U32(d.Length, 6 * sizeof(WCHAR));
    ASSERT_TRUE(wcsncmp(d.Buffer, L"foobar", 6) == 0);

    /* Overflow past MaximumLength returns STATUS_BUFFER_TOO_SMALL. */
    WCHAR tiny[3];
    UNICODE_STRING t = { 0, sizeof(tiny), tiny };
    ASSERT_NTSTATUS(RtlAppendUnicodeToString(&t, L"toolong"),
                    STATUS_BUFFER_TOO_SMALL);
    return true;
}

static const test_entry_t rtl_strconv_entries[] = {
    {"upper_char", t_upper_char},
    {"copy_unicode", t_copy_unicode},
    {"upcase_unicode_inplace", t_upcase_unicode_inplace},
    {"upcase_unicode_alloc", t_upcase_unicode_alloc},
    {"create_unicode", t_create_unicode},
    {"append_unicode_to_string", t_append_unicode_to_string},
};

DEFINE_GROUP(rtl_strconv, "rtl/strconv");
