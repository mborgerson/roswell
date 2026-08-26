/*
 * Rtl Unicode case-mapping and codepage-size exports whose ordinals
 * were unmapped bugcheck stubs.  The case routines drive off the
 * embedded l_intl.nls table; the codepage-size routines are the
 * ASCII/Latin-1 fast-paths.  One test per export.
 */

#include "../harness.h"
#include <string.h>
#include <wchar.h>

static bool t_upcase_unicode_char(void)
{
    ASSERT_EQ_U32(RtlUpcaseUnicodeChar(L'a'), L'A');
    ASSERT_EQ_U32(RtlUpcaseUnicodeChar(L'z'), L'Z');
    /* Already-upper and non-letters pass through unchanged. */
    ASSERT_EQ_U32(RtlUpcaseUnicodeChar(L'A'), L'A');
    ASSERT_EQ_U32(RtlUpcaseUnicodeChar(L'5'), L'5');
    /* Above U+007F the case table drives the fold:
     * a-grave (U+00E0) upcases to A-grave (U+00C0). */
    ASSERT_EQ_U32(RtlUpcaseUnicodeChar(0x00E0), 0x00C0);
    return true;
}

static bool t_downcase_unicode_char(void)
{
    ASSERT_EQ_U32(RtlDowncaseUnicodeChar(L'A'), L'a');
    ASSERT_EQ_U32(RtlDowncaseUnicodeChar(L'Z'), L'z');
    ASSERT_EQ_U32(RtlDowncaseUnicodeChar(L'a'), L'a');
    ASSERT_EQ_U32(RtlDowncaseUnicodeChar(L'0'), L'0');
    /* A-grave (U+00C0) downcases to a-grave (U+00E0). */
    ASSERT_EQ_U32(RtlDowncaseUnicodeChar(0x00C0), 0x00E0);
    return true;
}

static bool t_lower_char(void)
{
    ASSERT_EQ_U32((unsigned char)RtlLowerChar('A'), 'a');
    ASSERT_EQ_U32((unsigned char)RtlLowerChar('M'), 'm');
    ASSERT_EQ_U32((unsigned char)RtlLowerChar('Z'), 'z');
    /* Already-lower and non-letters pass through unchanged. */
    ASSERT_EQ_U32((unsigned char)RtlLowerChar('a'), 'a');
    ASSERT_EQ_U32((unsigned char)RtlLowerChar('5'), '5');
    ASSERT_EQ_U32((unsigned char)RtlLowerChar('-'), '-');
    return true;
}

static bool t_downcase_unicode_string(void)
{
    UNICODE_STRING src, dst;
    WCHAR srcbuf[] = L"MixedCASE123";
    WCHAR dstbuf[16];

    RtlInitUnicodeString(&src, srcbuf);
    dst.Length = 0;
    dst.MaximumLength = sizeof(dstbuf);
    dst.Buffer = dstbuf;

    ASSERT_NTSTATUS(RtlDowncaseUnicodeString(&dst, &src, FALSE),
                    STATUS_SUCCESS);
    ASSERT_EQ_U32(dst.Length, src.Length);
    ASSERT_TRUE(wcsncmp(dst.Buffer, L"mixedcase123", 12) == 0);

    /* Allocating variant returns a fresh buffer. */
    UNICODE_STRING out;
    ASSERT_NTSTATUS(RtlDowncaseUnicodeString(&out, &src, TRUE),
                    STATUS_SUCCESS);
    ASSERT_NOT_NULL(out.Buffer);
    ASSERT_EQ_U32(out.Length, src.Length);
    ASSERT_TRUE(wcsncmp(out.Buffer, L"mixedcase123", 12) == 0);
    RtlFreeUnicodeString(&out);
    return true;
}

static bool t_multibyte_to_unicode_size(void)
{
    ULONG size = 0xdead;

    /* Single-byte codepage: each byte becomes one WCHAR. */
    ASSERT_NTSTATUS(RtlMultiByteToUnicodeSize(&size, "hello", 5),
                    STATUS_SUCCESS);
    ASSERT_EQ_U32(size, 5 * sizeof(WCHAR));

    ASSERT_NTSTATUS(RtlMultiByteToUnicodeSize(&size, "", 0),
                    STATUS_SUCCESS);
    ASSERT_EQ_U32(size, 0);
    return true;
}

static const test_entry_t rtl_nlscase_entries[] = {
    {"upcase_unicode_char", t_upcase_unicode_char},
    {"downcase_unicode_char", t_downcase_unicode_char},
    {"lower_char", t_lower_char},
    {"downcase_unicode_string", t_downcase_unicode_string},
    {"multibyte_to_unicode_size", t_multibyte_to_unicode_size},
};

DEFINE_GROUP(rtl_nlscase, "rtl/nlscase");
