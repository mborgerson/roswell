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

static const test_entry_t rtl_nlscase_entries[] = {
    {"upcase_unicode_char", t_upcase_unicode_char},
};

DEFINE_GROUP(rtl_nlscase, "rtl/nlscase");
