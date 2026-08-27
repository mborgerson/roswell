/*
 * NtProtectVirtualMemory: the page-protection change titles apply to
 * their own allocations.
 *
 * The console's routine drops NT's process handle -- there is only one
 * address space -- and reports the old protection of the first page.
 * A write to a page turned read-only must fault, so one case runs the
 * store under a hand-rolled SEH frame on fs:[0]; see ke/exceptions for
 * the full treatment of that machinery.
 */

#include "../harness.h"
#include <setjmp.h>
#include <string.h>

#ifndef STATUS_ACCESS_VIOLATION
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xC0000005L)
#endif

#ifndef STATUS_NOT_COMMITTED
#define STATUS_NOT_COMMITTED ((NTSTATUS)0xC000002DL)
#endif
#ifndef STATUS_CONFLICTING_ADDRESSES
#define STATUS_CONFLICTING_ADDRESSES ((NTSTATUS)0xC0000018L)
#endif

#define XD_CONTINUE_SEARCH 1

typedef struct seh_frame {
    struct seh_frame *next;
    void *handler;
} seh_frame;

static jmp_buf g_jb;
static volatile NTSTATUS g_code;

static void push_frame(seh_frame *f, void *handler)
{
    f->handler = handler;
    __asm__ volatile(
        "movl %%fs:0, %%eax\n\t"
        "movl %%eax, %0\n\t"
        "movl %1, %%fs:0"
        : "=m"(f->next) : "r"(f) : "eax", "memory");
}

static void pop_frame(seh_frame *f)
{
    __asm__ volatile(
        "movl %0, %%eax\n\t"
        "movl %%eax, %%fs:0"
        :: "m"(f->next) : "eax", "memory");
}

static int __cdecl h_record_unwind(EXCEPTION_RECORD *rec, void *frame,
                                   CONTEXT *ctx, void *disp)
{
    (void)ctx; (void)disp;
    if (rec->ExceptionFlags & EXCEPTION_UNWIND)
        return XD_CONTINUE_SEARCH;
    g_code = rec->ExceptionCode;
    RtlUnwind(frame, NULL, rec, 0);
    longjmp(g_jb, 1);
}

/* Reserve+commit one region for a case to protect; freed by free_pages. */
static NTSTATUS alloc_pages(PVOID *base, SIZE_T bytes)
{
    *base = NULL;
    return NtAllocateVirtualMemory(base, 0, &bytes, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_READWRITE);
}

static void free_pages(PVOID base)
{
    SIZE_T size = 0;
    NtFreeVirtualMemory(&base, &size, MEM_RELEASE);
}

static bool t_readonly_then_back(void)
{
    PVOID base;
    PVOID p;
    SIZE_T size;
    ULONG old = 0x5A5A5A5A;
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS s;

    s = alloc_pages(&base, 0x1000);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    p = base;
    size = 0x1000;
    s = NtProtectVirtualMemory(&p, &size, PAGE_READONLY, &old);
    if (!NT_SUCCESS(s)) {
        free_pages(base);
        FAIL_AND_RETURN("protect -> 0x%08x", (unsigned)s);
    }
    if (old != PAGE_READWRITE) {
        free_pages(base);
        FAIL_AND_RETURN("old protect = 0x%08x", (unsigned)old);
    }

    memset(&mbi, 0, sizeof(mbi));
    s = NtQueryVirtualMemory(base, &mbi);
    if (!NT_SUCCESS(s) || mbi.Protect != PAGE_READONLY) {
        free_pages(base);
        FAIL_AND_RETURN("query -> 0x%08x protect 0x%08x",
                        (unsigned)s, (unsigned)mbi.Protect);
    }

    /* And back again: the old protection is now the read-only one. */
    p = base;
    size = 0x1000;
    old = 0x5A5A5A5A;
    s = NtProtectVirtualMemory(&p, &size, PAGE_READWRITE, &old);
    free_pages(base);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(old, PAGE_READONLY);
    return true;
}

/* Base rounds down to its page and size rounds up to cover the request;
 * both are written back. */
static bool t_base_and_size_are_rounded(void)
{
    PVOID base;
    PVOID p;
    SIZE_T size;
    ULONG old = 0;
    NTSTATUS s;

    s = alloc_pages(&base, 0x3000);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    p = (PVOID)((ULONG_PTR)base + 0x800);
    size = 0x900;                       /* straddles two pages */
    s = NtProtectVirtualMemory(&p, &size, PAGE_READONLY, &old);
    if (!NT_SUCCESS(s)) {
        free_pages(base);
        FAIL_AND_RETURN("protect -> 0x%08x", (unsigned)s);
    }
    if (p != base || size != 0x2000) {
        free_pages(base);
        FAIL_AND_RETURN("write-back base=%p (want %p) size=0x%x (want 0x2000)",
                        p, base, (unsigned)size);
    }

    /* The third page kept its protection. */
    {
        MEMORY_BASIC_INFORMATION mbi;
        memset(&mbi, 0, sizeof(mbi));
        s = NtQueryVirtualMemory((PVOID)((ULONG_PTR)base + 0x2000), &mbi);
        if (!NT_SUCCESS(s) || mbi.Protect != PAGE_READWRITE) {
            free_pages(base);
            FAIL_AND_RETURN("third page protect 0x%08x", (unsigned)mbi.Protect);
        }
    }
    free_pages(base);
    return true;
}

static bool t_writing_a_readonly_page_faults(void)
{
    volatile unsigned char *cell;
    seh_frame f;
    PVOID base;
    PVOID p;
    SIZE_T size;
    ULONG old = 0;
    NTSTATUS s;
    bool faulted;

    s = alloc_pages(&base, 0x1000);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    cell = (volatile unsigned char *)base;
    *cell = 0x11;

    p = base;
    size = 0x1000;
    s = NtProtectVirtualMemory(&p, &size, PAGE_READONLY, &old);
    if (!NT_SUCCESS(s)) {
        free_pages(base);
        FAIL_AND_RETURN("protect -> 0x%08x", (unsigned)s);
    }

    g_code = 0;
    faulted = false;
    if (setjmp(g_jb) == 0) {
        push_frame(&f, h_record_unwind);
        *cell = 0x22;
        pop_frame(&f);
    } else {
        pop_frame(&f);
        faulted = true;
    }

    /* Reads still work, and the store never landed. */
    {
        unsigned char seen = *cell;
        p = base;
        size = 0x1000;
        NtProtectVirtualMemory(&p, &size, PAGE_READWRITE, &old);
        free_pages(base);
        if (!faulted) FAIL_AND_RETURN("the store did not fault");
        ASSERT_NTSTATUS(g_code, STATUS_ACCESS_VIOLATION);
        ASSERT_EQ_U32(seen, 0x11);
    }
    return true;
}

/* A range inside a reservation but with no pages behind it is refused,
 * and an address that was never reserved conflicts.  Neither touches
 * OldProtect. */
static bool t_uncommitted_and_unreserved_are_refused(void)
{
    PVOID base = NULL;
    PVOID p;
    SIZE_T size = 0x1000;
    ULONG old = 0x5A5A5A5A;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    p = base;
    size = 0x1000;
    s = NtProtectVirtualMemory(&p, &size, PAGE_READONLY, &old);
    free_pages(base);
    ASSERT_NTSTATUS(s, STATUS_NOT_COMMITTED);
    ASSERT_EQ_U32(old, 0x5A5A5A5A);

    /* Mapped in neither kernel: above any title allocation. */
    p = (PVOID)0x60000000;
    size = 0x1000;
    s = NtProtectVirtualMemory(&p, &size, PAGE_READONLY, &old);
    ASSERT_NTSTATUS(s, STATUS_CONFLICTING_ADDRESSES);
    ASSERT_EQ_U32(old, 0x5A5A5A5A);
    return true;
}

/* PAGE_NOACCESS applies and comes back off again -- unlike the Mm-level
 * MmSetAddressProtect, which refuses it (see mm/vmcontract) -- and the
 * cacheability bits ride along with the access bits. */
static bool t_noaccess_and_cacheability_apply(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    PVOID base;
    PVOID p;
    SIZE_T size;
    ULONG old = 0;
    NTSTATUS s;

    s = alloc_pages(&base, 0x1000);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    p = base;
    size = 0x1000;
    s = NtProtectVirtualMemory(&p, &size, PAGE_NOACCESS, &old);
    if (!NT_SUCCESS(s) || old != PAGE_READWRITE) {
        free_pages(base);
        FAIL_AND_RETURN("noaccess -> 0x%08x old=0x%08x",
                        (unsigned)s, (unsigned)old);
    }
    memset(&mbi, 0, sizeof(mbi));
    s = NtQueryVirtualMemory(base, &mbi);
    if (!NT_SUCCESS(s) || mbi.Protect != PAGE_NOACCESS ||
        mbi.State != MEM_COMMIT) {
        free_pages(base);
        FAIL_AND_RETURN("query -> 0x%08x protect=0x%08x state=0x%08x",
                        (unsigned)s, (unsigned)mbi.Protect,
                        (unsigned)mbi.State);
    }

    /* Back to accessible, uncached, in one step. */
    p = base;
    size = 0x1000;
    s = NtProtectVirtualMemory(&p, &size, PAGE_READWRITE | PAGE_NOCACHE, &old);
    if (!NT_SUCCESS(s) || old != PAGE_NOACCESS) {
        free_pages(base);
        FAIL_AND_RETURN("rw|nocache -> 0x%08x old=0x%08x",
                        (unsigned)s, (unsigned)old);
    }
    memset(&mbi, 0, sizeof(mbi));
    NtQueryVirtualMemory(base, &mbi);
    if (mbi.Protect != (PAGE_READWRITE | PAGE_NOCACHE)) {
        free_pages(base);
        FAIL_AND_RETURN("protect after rw|nocache = 0x%08x",
                        (unsigned)mbi.Protect);
    }
    ((volatile unsigned char *)base)[0] = 0xA5;

    p = base;
    size = 0x1000;
    NtProtectVirtualMemory(&p, &size, PAGE_READWRITE, &old);
    free_pages(base);
    return true;
}

static bool t_reading_a_noaccess_page_faults(void)
{
    volatile unsigned char *cell;
    seh_frame f;
    PVOID base;
    PVOID p;
    SIZE_T size;
    ULONG old = 0;
    NTSTATUS s;
    bool faulted;

    s = alloc_pages(&base, 0x1000);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    cell = (volatile unsigned char *)base;
    *cell = 0x33;

    p = base;
    size = 0x1000;
    s = NtProtectVirtualMemory(&p, &size, PAGE_NOACCESS, &old);
    if (!NT_SUCCESS(s)) {
        free_pages(base);
        FAIL_AND_RETURN("protect -> 0x%08x", (unsigned)s);
    }

    g_code = 0;
    faulted = false;
    if (setjmp(g_jb) == 0) {
        push_frame(&f, h_record_unwind);
        (void)*cell;
        pop_frame(&f);
    } else {
        pop_frame(&f);
        faulted = true;
    }

    p = base;
    size = 0x1000;
    s = NtProtectVirtualMemory(&p, &size, PAGE_READWRITE, &old);
    if (!NT_SUCCESS(s)) {
        free_pages(base);
        FAIL_AND_RETURN("restore -> 0x%08x", (unsigned)s);
    }
    /* The frame survived the excursion. */
    {
        unsigned char seen = *cell;
        free_pages(base);
        if (!faulted) FAIL_AND_RETURN("the read did not fault");
        ASSERT_NTSTATUS(g_code, STATUS_ACCESS_VIOLATION);
        ASSERT_EQ_U32(seen, 0x33);
    }
    return true;
}

static const test_entry_t mm_protect_entries[] = {
    { "readonly_then_back",           t_readonly_then_back,           NULL },
    { "base_and_size_are_rounded",    t_base_and_size_are_rounded,    NULL },
    { "writing_a_readonly_page_faults",
      t_writing_a_readonly_page_faults, NULL },
    { "uncommitted_and_unreserved_are_refused",
      t_uncommitted_and_unreserved_are_refused, NULL },
    { "noaccess_and_cacheability_apply",
      t_noaccess_and_cacheability_apply, NULL },
    { "reading_a_noaccess_page_faults",
      t_reading_a_noaccess_page_faults, NULL },
};

DEFINE_GROUP(mm_protect, "mm/protect");
