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

static const test_entry_t rtl_nlscase_entries[] = {
    {"upcase_unicode_char", t_upcase_unicode_char},
    {"downcase_unicode_char", t_downcase_unicode_char},
    {"lower_char", t_lower_char},
};

DEFINE_GROUP(rtl_nlscase, "rtl/nlscase");
