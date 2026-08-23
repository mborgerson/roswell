/*
 * XeLoadSection / XeUnloadSection -- section refcounts, reload-from-disk
 * semantics, and the shared head/tail page counters.
 *
 * XTEST is this XBE's own dedicated payload section (below): writable,
 * preloaded (cxbe flags every section PRELOAD), three pages with a
 * zero-fill tail (cxbe trims trailing zeros from the raw image).  The
 * observable contract under test: the last unload gives the pages back,
 * and the next load streams the raw data fresh from disk -- so in-memory
 * mutations survive load/unload only while the reference count stays
 * above zero.
 */
#include "../harness.h"

#define XTEST_SIZE (2 * 4096 + 700)

__attribute__((section("XTEST"), used))
unsigned char xe_test_payload[XTEST_SIZE] = {
    [0] = 0x11,
    [100] = 0x22,
    [5000] = 0x33,
    [8000] = 0x44,   /* last non-zero byte: raw size ends here */
};

/* XBE header fields (image base is fixed at 0x10000). */
#define XBE_BASE            ((unsigned char *)0x00010000)
#define XBE_NUM_SECTIONS    (*(DWORD *)(XBE_BASE + 0x11C))
#define XBE_SECTION_TABLE   (*(PXBE_SECTION_HEADER *)(XBE_BASE + 0x120))

#define XBE_SECTION_FLAG_WRITABLE 0x00000001
#define XBE_SECTION_FLAG_PRELOAD  0x00000002

static PXBE_SECTION_HEADER xtest_section(void)
{
    DWORD i;
    for (i = 0; i < XBE_NUM_SECTIONS; i++) {
        PXBE_SECTION_HEADER s = &XBE_SECTION_TABLE[i];
        const char *n = (const char *)s->SectionName;
        if (n && n[0] == 'X' && n[1] == 'T' && n[2] == 'E' && n[3] == 'S' &&
            n[4] == 'T' && n[5] == '\0')
            return s;
    }
    return NULL;
}

static bool test_find_section(void)
{
    PXBE_SECTION_HEADER s = xtest_section();
    ASSERT_TRUE(s != NULL);
    ASSERT_TRUE(s->Flags & XBE_SECTION_FLAG_WRITABLE);
    ASSERT_TRUE(s->Flags & XBE_SECTION_FLAG_PRELOAD);
    ASSERT_TRUE((unsigned char *)s->VirtualAddress <= xe_test_payload);
    ASSERT_TRUE(s->VirtualSize >= XTEST_SIZE);
    ASSERT_TRUE(s->FileSize >= 8001);          /* raw covers last initializer */
    ASSERT_TRUE(s->FileSize <= s->VirtualSize);
    return true;
}

static bool test_boot_state(void)
{
    PXBE_SECTION_HEADER s = xtest_section();
    ASSERT_TRUE(s != NULL);
    /* Preloaded at boot: one reference, shared-page counters live. */
    ASSERT_EQ_U32(s->SectionReferenceCount, 1);
    ASSERT_TRUE(*s->HeadReferenceCount >= 1);
    ASSERT_TRUE(*s->TailReferenceCount >= 1);
    /* Initializer content is in place. */
    ASSERT_EQ_U32(xe_test_payload[0], 0x11);
    ASSERT_EQ_U32(xe_test_payload[100], 0x22);
    ASSERT_EQ_U32(xe_test_payload[5000], 0x33);
    ASSERT_EQ_U32(xe_test_payload[8000], 0x44);
    ASSERT_EQ_U32(xe_test_payload[XTEST_SIZE - 1], 0);
    return true;
}

static bool test_reload_restores_content(void)
{
    PXBE_SECTION_HEADER s = xtest_section();
    ASSERT_TRUE(s != NULL);

    xe_test_payload[0] = 0xFF;
    xe_test_payload[5000] = 0xFF;
    xe_test_payload[XTEST_SIZE - 1] = 0xFF;    /* dirty the zero-fill tail too */

    ASSERT_NTSTATUS(XeUnloadSection(s), STATUS_SUCCESS);
    ASSERT_EQ_U32(s->SectionReferenceCount, 0);
    /* Section memory is invalid here -- do not touch the payload. */

    ASSERT_NTSTATUS(XeLoadSection(s), STATUS_SUCCESS);
    ASSERT_EQ_U32(s->SectionReferenceCount, 1);

    /* Raw data streamed back from disk; tail zero-filled again. */
    ASSERT_EQ_U32(xe_test_payload[0], 0x11);
    ASSERT_EQ_U32(xe_test_payload[100], 0x22);
    ASSERT_EQ_U32(xe_test_payload[5000], 0x33);
    ASSERT_EQ_U32(xe_test_payload[8000], 0x44);
    ASSERT_EQ_U32(xe_test_payload[XTEST_SIZE - 1], 0);
    return true;
}

static bool test_refcount_nesting(void)
{
    PXBE_SECTION_HEADER s = xtest_section();
    ASSERT_TRUE(s != NULL);

    /* Second load: no reload happens, just the count. */
    ASSERT_NTSTATUS(XeLoadSection(s), STATUS_SUCCESS);
    ASSERT_EQ_U32(s->SectionReferenceCount, 2);

    xe_test_payload[0] = 0xEE;

    /* Drop to one reference: still mapped, mutation intact. */
    ASSERT_NTSTATUS(XeUnloadSection(s), STATUS_SUCCESS);
    ASSERT_EQ_U32(s->SectionReferenceCount, 1);
    ASSERT_EQ_U32(xe_test_payload[0], 0xEE);

    /* Last unload, then a fresh load: content restored from disk. */
    ASSERT_NTSTATUS(XeUnloadSection(s), STATUS_SUCCESS);
    ASSERT_EQ_U32(s->SectionReferenceCount, 0);
    ASSERT_NTSTATUS(XeLoadSection(s), STATUS_SUCCESS);
    ASSERT_EQ_U32(s->SectionReferenceCount, 1);
    ASSERT_EQ_U32(xe_test_payload[0], 0x11);
    return true;
}

static bool test_shared_page_counters(void)
{
    PXBE_SECTION_HEADER s = xtest_section();
    WORD headLoaded, tailLoaded;
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ_U32(s->SectionReferenceCount, 1);

    headLoaded = *s->HeadReferenceCount;
    tailLoaded = *s->TailReferenceCount;

    ASSERT_NTSTATUS(XeUnloadSection(s), STATUS_SUCCESS);
    ASSERT_EQ_U32(*s->HeadReferenceCount, (DWORD)(headLoaded - 1));
    ASSERT_EQ_U32(*s->TailReferenceCount, (DWORD)(tailLoaded - 1));

    ASSERT_NTSTATUS(XeLoadSection(s), STATUS_SUCCESS);
    ASSERT_EQ_U32(*s->HeadReferenceCount, headLoaded);
    ASSERT_EQ_U32(*s->TailReferenceCount, tailLoaded);
    return true;
}

static const test_entry_t ENTRIES[] = {
    { "find_section", test_find_section, NULL },
    { "boot_state", test_boot_state, NULL },
    { "reload_restores_content", test_reload_restores_content, NULL },
    { "refcount_nesting", test_refcount_nesting, NULL },
    { "shared_page_counters", test_shared_page_counters, NULL },
};

const test_group_t g_group_xe_sections = {
    "xe/section", ENTRIES, sizeof(ENTRIES) / sizeof(ENTRIES[0])
};
