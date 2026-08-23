/*
 * NtAllocateVirtualMemory / NtFreeVirtualMemory argument-space coverage.
 *
 * Regression coverage for the VM dispatch-arm trim:
 * MEM_DECOMMIT returned STATUS_NOT_IMPLEMENTED and a
 * re-commit over committed pages silently skipped its work.  XAPI's
 * heap decommits free blocks, so the suite needs these paths pinned.
 */

#include "../harness.h"
#include <string.h>

static bool t_decommit_then_recommit(void)
{
    PVOID base = NULL;
    SIZE_T size = 64 * 1024;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(base);

    memset(base, 0x77, size);

    /* Decommit keeps the reservation but drops the pages. */
    {
        PVOID b = base;
        SIZE_T sz = size;
        s = NtFreeVirtualMemory(&b, &sz, MEM_DECOMMIT);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }

    /* Re-commit: fresh pages must read back zero, not 0x77. */
    {
        PVOID b = base;
        SIZE_T sz = size;
        s = NtAllocateVirtualMemory(&b, 0, &sz, MEM_COMMIT, PAGE_READWRITE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }
    {
        const unsigned char *p = (const unsigned char *)base;
        SIZE_T i;
        for (i = 0; i < size; i += 4096) {
            if (p[i] != 0) {
                PVOID b = base; SIZE_T sz = 0;
                NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
                FAIL_AND_RETURN("recommitted page +%u not zeroed (0x%02x)",
                                (unsigned)i, p[i]);
            }
        }
    }

    {
        PVOID b = base;
        SIZE_T sz = 0;
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }
    return true;
}

static bool t_partial_decommit(void)
{
    PVOID base = NULL;
    SIZE_T size = 64 * 1024;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Decommit one page in the middle; neighbours stay usable. */
    {
        PVOID b = (PUCHAR)base + 4096;
        SIZE_T sz = 4096;
        s = NtFreeVirtualMemory(&b, &sz, MEM_DECOMMIT);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }
    ((volatile unsigned char *)base)[0] = 0x11;
    ((volatile unsigned char *)base)[2 * 4096] = 0x22;
    ASSERT_TRUE(((unsigned char *)base)[0] == 0x11);
    ASSERT_TRUE(((unsigned char *)base)[2 * 4096] == 0x22);

    {
        PVOID b = base;
        SIZE_T sz = 0;
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }
    return true;
}

static bool t_query_after_decommit(void)
{
    PVOID base = NULL;
    SIZE_T size = 16 * 4096;
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    {
        PVOID b = base;
        SIZE_T sz = size;
        s = NtFreeVirtualMemory(&b, &sz, MEM_DECOMMIT);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }

    memset(&mbi, 0, sizeof(mbi));
    s = NtQueryVirtualMemory(base, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_RESERVE);

    {
        PVOID b = base;
        SIZE_T sz = 0;
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }
    return true;
}

/*
 * Re-commit over already-committed pages with a different protection.
 * Retail-validated: the second MEM_COMMIT succeeds, the new
 * protection is what NtQueryVirtualMemory reports afterwards, and the
 * page contents survive (re-commit of committed pages does not rezero).
 * This is the path the ChangeProtection gate in NtAllocateVirtualMemory
 * would skip, so it pins that arm as load-bearing.
 */
static bool t_recommit_changes_protection(void)
{
    PVOID base = NULL;
    SIZE_T size = 8 * 4096;
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(base);

    memset(base, 0x5A, size);

    memset(&mbi, 0, sizeof(mbi));
    s = NtQueryVirtualMemory(base, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_COMMIT);
    ASSERT_EQ_U32(mbi.Protect, PAGE_READWRITE);

    /* Re-commit the same range read-only; no MEM_RESERVE this time. */
    {
        PVOID b = base;
        SIZE_T sz = size;
        s = NtAllocateVirtualMemory(&b, 0, &sz, MEM_COMMIT, PAGE_READONLY);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }

    memset(&mbi, 0, sizeof(mbi));
    s = NtQueryVirtualMemory(base, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_COMMIT);
    ASSERT_EQ_U32(mbi.Protect, PAGE_READONLY);

    /* Contents survive a re-commit; this is not a fresh-zero path. */
    {
        const unsigned char *p = (const unsigned char *)base;
        SIZE_T i;
        for (i = 0; i < size; i += 4096) {
            if (p[i] != 0x5A) {
                PVOID b = base; SIZE_T sz = 0;
                NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
                FAIL_AND_RETURN("recommit rezeroed page +%u (0x%02x)",
                                (unsigned)i, p[i]);
            }
        }
    }

    /* Flip back to read-write and prove the pages take stores again. */
    {
        PVOID b = base;
        SIZE_T sz = size;
        s = NtAllocateVirtualMemory(&b, 0, &sz, MEM_COMMIT, PAGE_READWRITE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }
    memset(&mbi, 0, sizeof(mbi));
    s = NtQueryVirtualMemory(base, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.Protect, PAGE_READWRITE);
    ((volatile unsigned char *)base)[0] = 0xA5;
    ASSERT_TRUE(((unsigned char *)base)[0] == 0xA5);

    {
        PVOID b = base;
        SIZE_T sz = 0;
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }
    return true;
}

static const test_entry_t mm_virtual_entries[] = {
    {"decommit_then_recommit",      t_decommit_then_recommit},
    {"partial_decommit",            t_partial_decommit},
    {"query_after_decommit",        t_query_after_decommit},
    {"recommit_changes_protection", t_recommit_changes_protection},
};

DEFINE_GROUP(mm_virtual, "mm/virtual");
