/*
 * RtlInit/Equal/Compare/Copy for ANSI + Unicode strings, plus the
 * ANSI<->Unicode bridge titles use to build paths for NtCreateFile.
 */

#include "../harness.h"
#include <string.h>

static bool t_init_ansi(void)
{
    ANSI_STRING s;
    RtlInitAnsiString(&s, "hello");
    ASSERT_EQ_U32(s.Length, 5);
    ASSERT_EQ_U32(s.MaximumLength, 6);
    ASSERT_TRUE(s.Buffer != NULL);
    ASSERT_TRUE(strcmp(s.Buffer, "hello") == 0);
    return true;
}

static bool t_init_ansi_null(void)
{
    ANSI_STRING s;
    RtlInitAnsiString(&s, NULL);
    ASSERT_EQ_U32(s.Length, 0);
    ASSERT_EQ_U32(s.MaximumLength, 0);
    return true;
}

static bool t_init_unicode(void)
{
    UNICODE_STRING u;
    RtlInitUnicodeString(&u, L"world");
    ASSERT_EQ_U32(u.Length, 10);
    ASSERT_EQ_U32(u.MaximumLength, 12);
    return true;
}

static bool t_equal_ansi(void)
{
    ANSI_STRING a, b, c;
    RtlInitAnsiString(&a, "Foo");
    RtlInitAnsiString(&b, "Foo");
    RtlInitAnsiString(&c, "Bar");
    ASSERT_TRUE(RtlEqualString(&a, &b, FALSE));
    ASSERT_TRUE(!RtlEqualString(&a, &c, FALSE));
    return true;
}

static bool t_equal_ansi_case(void)
{
    ANSI_STRING a, b;
    RtlInitAnsiString(&a, "Foo");
    RtlInitAnsiString(&b, "foo");
    ASSERT_TRUE(RtlEqualString(&a, &b, TRUE));
    ASSERT_TRUE(!RtlEqualString(&a, &b, FALSE));
    return true;
}

static bool t_equal_unicode(void)
{
    UNICODE_STRING a, b, c;
    RtlInitUnicodeString(&a, L"Foo");
    RtlInitUnicodeString(&b, L"Foo");
    RtlInitUnicodeString(&c, L"Bar");
    ASSERT_TRUE(RtlEqualUnicodeString(&a, &b, FALSE));
    ASSERT_TRUE(!RtlEqualUnicodeString(&a, &c, FALSE));
    return true;
}

static bool t_compare_unicode(void)
{
    UNICODE_STRING a, b;
    RtlInitUnicodeString(&a, L"abc");
    RtlInitUnicodeString(&b, L"abd");
    ASSERT_TRUE(RtlCompareUnicodeString(&a, &b, FALSE) < 0);
    ASSERT_TRUE(RtlCompareUnicodeString(&b, &a, FALSE) > 0);
    ASSERT_TRUE(RtlCompareUnicodeString(&a, &a, FALSE) == 0);
    return true;
}

static bool t_ansi_to_unicode(void)
{
    ANSI_STRING a;
    UNICODE_STRING u;
    RtlInitAnsiString(&a, "ABC");
    NTSTATUS s = RtlAnsiStringToUnicodeString(&u, &a, TRUE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(u.Length, 6);
    ASSERT_TRUE(u.Buffer[0] == L'A' && u.Buffer[1] == L'B' && u.Buffer[2] == L'C');
    RtlFreeUnicodeString(&u);
    return true;
}

static bool t_compare_memory_ulong(void)
{
    static const ULONG buf[4] = { 0xAAAAAAAA, 0xAAAAAAAA, 0xBBBBBBBB, 0xAAAAAAAA };
    ASSERT_EQ_U32(RtlCompareMemoryUlong((PVOID)buf, sizeof(buf), 0xAAAAAAAA), 8);
    ASSERT_EQ_U32(RtlCompareMemoryUlong((PVOID)buf, sizeof(buf), 0xBBBBBBBB), 0);
    return true;
}

static const test_entry_t rtl_string_entries[] = {
    {"init_ansi",          t_init_ansi},
    {"init_unicode",       t_init_unicode},
    {"equal_ansi",         t_equal_ansi},
    {"equal_ansi_case",    t_equal_ansi_case},
    {"equal_unicode",      t_equal_unicode},
    {"compare_unicode",    t_compare_unicode},
    {"ansi_to_unicode",    t_ansi_to_unicode},
    {"compare_memory_ulong", t_compare_memory_ulong},
    /* Last: nxkrnl currently bugchecks on RtlInitAnsiString(NULL); keep
     * this test at the end so the rest of the suite still runs to
     * completion. Retail handles NULL gracefully. */
    {"init_ansi_null",     t_init_ansi_null},
};

DEFINE_GROUP(rtl_string, "rtl/string");
