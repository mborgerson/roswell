/*
 * NtAllocateVirtualMemory / NtFreeVirtualMemory / NtQueryVirtualMemory /
 * MmQuery(Set)AddressProtect / MmQueryStatistics contract pins.
 *
 * Pins the semantics a VM rewrite must preserve: reserve/commit/free
 * state transitions and their status codes, size rounding, region
 * reporting across a partially committed reservation, protection
 * round-trips, and which MmQueryStatistics fields move when committing.
 *
 * Where eager-commit and demand-zero kernels legitimately diverge
 * (when AvailablePages drops relative to the commit call), the test
 * records the observation with a TAP comment instead of asserting.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_FREE_VM_NOT_AT_BASE
#define STATUS_FREE_VM_NOT_AT_BASE  ((NTSTATUS)0xC000009FL)
#endif
#ifndef STATUS_MEMORY_NOT_ALLOCATED
#define STATUS_MEMORY_NOT_ALLOCATED ((NTSTATUS)0xC00000A0L)
#endif

#define KB(n) ((SIZE_T)(n) * 1024)
#define MB(n) ((SIZE_T)(n) * 1024 * 1024)

static NTSTATUS query(PVOID at, MEMORY_BASIC_INFORMATION *mbi)
{
    memset(mbi, 0, sizeof(*mbi));
    return NtQueryVirtualMemory(at, mbi);
}

static void release(PVOID base)
{
    PVOID b = base;
    SIZE_T sz = 0;
    NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
}

/* Reserve 1 MB, commit 256 KB in the middle: the three sub-ranges
 * report distinct State/RegionSize runs under one AllocationBase. */
static bool t_reserve_commit_regions(void)
{
    PVOID base = NULL;
    SIZE_T size = MB(1);
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NOT_NULL(base);
    ASSERT_EQ_U32(size, MB(1));

    s = query(base, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_RESERVE);
    ASSERT_EQ_PTR(mbi.AllocationBase, base);
    ASSERT_EQ_U32(mbi.RegionSize, MB(1));
    ASSERT_EQ_U32(mbi.AllocationProtect, PAGE_READWRITE);

    {
        PVOID b = (PUCHAR)base + KB(256);
        SIZE_T sz = KB(256);
        s = NtAllocateVirtualMemory(&b, 0, &sz, MEM_COMMIT, PAGE_READWRITE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        ASSERT_EQ_PTR(b, (PUCHAR)base + KB(256));
        ASSERT_EQ_U32(sz, KB(256));
    }

    /* Leading reserved run stops at the committed range. */
    s = query(base, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_RESERVE);
    ASSERT_EQ_PTR(mbi.AllocationBase, base);
    ASSERT_EQ_U32(mbi.RegionSize, KB(256));

    /* Committed run, queried mid-range: BaseAddress rounds to the page,
     * the run spans the commit, and Protect is live. */
    s = query((PUCHAR)base + KB(256) + 12, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_COMMIT);
    ASSERT_EQ_PTR(mbi.BaseAddress, (PUCHAR)base + KB(256));
    ASSERT_EQ_PTR(mbi.AllocationBase, base);
    ASSERT_EQ_U32(mbi.RegionSize, KB(256));
    ASSERT_EQ_U32(mbi.Protect, PAGE_READWRITE);

    /* Trailing reserved run goes to the end of the reservation. */
    s = query((PUCHAR)base + KB(512), &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_RESERVE);
    ASSERT_EQ_PTR(mbi.AllocationBase, base);
    ASSERT_EQ_U32(mbi.RegionSize, KB(512));

    /* Committed pages inside a reservation are usable. */
    memset((PUCHAR)base + KB(256), 0x3C, KB(256));
    ASSERT_TRUE(((unsigned char *)base)[KB(256)] == 0x3C);

    release(base);

    s = query(base, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_FREE);
    return true;
}

/* Sizes round up to page granularity; the chosen base is recorded for
 * the allocation-granularity question. */
static bool t_size_rounding(void)
{
    PVOID base = NULL;
    SIZE_T size = 5000;
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(size, KB(8));
    /* Retail-validated: reservations take 64 KB allocation granularity. */
    ASSERT_TRUE(((ULONG_PTR)base & 0xFFFF) == 0);

    s = query(base, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.RegionSize, KB(8));

    release(base);
    return true;
}

/* Free-path status codes: release not at base, double release. */
static bool t_free_status_codes(void)
{
    PVOID base = NULL;
    SIZE_T size = KB(64);
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* MEM_RELEASE with size 0 demands the allocation base. */
    {
        PVOID b = (PUCHAR)base + KB(4);
        SIZE_T sz = 0;
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
        ASSERT_NTSTATUS(s, STATUS_FREE_VM_NOT_AT_BASE);
    }

    {
        PVOID b = base;
        SIZE_T sz = 0;
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }

    /* The region is gone; a second release must fail cleanly. */
    {
        PVOID b = base;
        SIZE_T sz = 0;
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
        ASSERT_NTSTATUS(s, STATUS_MEMORY_NOT_ALLOCATED);
    }
    return true;
}

/* MmSetAddressProtect/MmQueryAddressProtect round-trip on committed
 * pages, including a NOACCESS excursion. */
static bool t_protect_roundtrip(void)
{
    PVOID base = NULL;
    SIZE_T size = KB(64);
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    memset(base, 0x77, size);
    ASSERT_EQ_U32(MmQueryAddressProtect(base), PAGE_READWRITE);

    MmSetAddressProtect(base, size, PAGE_READONLY);
    ASSERT_EQ_U32(MmQueryAddressProtect(base), PAGE_READONLY);

    /* Retail-validated: PAGE_NOACCESS is not accepted here; the
     * protection stays at its previous value. */
    MmSetAddressProtect(base, size, PAGE_NOACCESS);
    ASSERT_EQ_U32(MmQueryAddressProtect(base), PAGE_READONLY);

    MmSetAddressProtect(base, size, PAGE_READWRITE);
    ASSERT_EQ_U32(MmQueryAddressProtect(base), PAGE_READWRITE);

    /* Contents survived the protection excursion and stores work. */
    ASSERT_TRUE(((unsigned char *)base)[0] == 0x77);
    ((volatile unsigned char *)base)[0] = 0xA5;
    ASSERT_TRUE(((unsigned char *)base)[0] == 0xA5);

    release(base);
    return true;
}

/* MmQueryStatistics field movement across a 4 MB commit+touch+free
 * cycle.  Committed bytes must track the commit exactly; available
 * pages must be consumed by the time pages are touched and recovered
 * after release (slack absorbs unrelated kernel activity). */
static bool t_statistics_track_commit(void)
{
    MM_STATISTICS pre = { .Length = sizeof(MM_STATISTICS) };
    MM_STATISTICS mid = { .Length = sizeof(MM_STATISTICS) };
    MM_STATISTICS post = { .Length = sizeof(MM_STATISTICS) };
    MM_STATISTICS end = { .Length = sizeof(MM_STATISTICS) };
    PVOID base = NULL;
    SIZE_T size = MB(4);
    NTSTATUS s;

    s = MmQueryStatistics(&pre);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = MmQueryStatistics(&mid);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mid.VirtualMemoryBytesCommitted - pre.VirtualMemoryBytesCommitted,
                  MB(4));
    /* Eager-commit kernels consume pages here; demand-zero kernels
     * don't until the touch below.  Recorded, not asserted. */
    tap_comment("vmcontract: commit-time AvailablePages delta = %ld",
                (long)pre.AvailablePages - (long)mid.AvailablePages);

    memset(base, 0xCD, MB(4));

    s = MmQueryStatistics(&post);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(pre.AvailablePages - post.AvailablePages >= 1024);

    release(base);

    s = MmQueryStatistics(&end);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(end.VirtualMemoryBytesCommitted - pre.VirtualMemoryBytesCommitted, 0);
    /* All 1024 pages come back, modulo unrelated kernel churn. */
    ASSERT_TRUE((long)pre.AvailablePages - (long)end.AvailablePages < 256);
    return true;
}

/* MEM_TOP_DOWN places the reservation high in the title range. */
static bool t_top_down(void)
{
    PVOID base = NULL;
    SIZE_T size = KB(64);
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
                                PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    tap_comment("vmcontract: TOP_DOWN base=%p", base);
    memset(base, 0x2A, size);
    ASSERT_TRUE(((unsigned char *)base)[0] == 0x2A);
    /* High half of the title range. */
    ASSERT_TRUE((ULONG_PTR)base >= 0x40000000UL);
    release(base);
    return true;
}

/* Releasing the middle of a reservation splits it: both remainders
 * stay usable and report their own runs. */
static bool t_release_middle_splits(void)
{
    PVOID base = NULL;
    SIZE_T size = KB(192);
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    memset(base, 0x77, size);

    {
        PVOID b = (PUCHAR)base + KB(64);
        SIZE_T sz = KB(64);
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
    }
    tap_comment("vmcontract: middle release -> 0x%08x", (unsigned)s);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Hole reports free; both remainders keep their contents. */
    s = query((PUCHAR)base + KB(64), &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_FREE);
    ASSERT_TRUE(((unsigned char *)base)[0] == 0x77);
    ASSERT_TRUE(((unsigned char *)base)[KB(128)] == 0x77);

    {
        PVOID b = base;
        SIZE_T sz = KB(64);
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        b = (PUCHAR)base + KB(128);
        sz = KB(64);
        s = NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }
    return true;
}

/* PAGE_NOACCESS commit succeeds and reports back; re-protecting to
 * read-write makes the pages usable. */
static bool t_noaccess_commit(void)
{
    PVOID base = NULL;
    SIZE_T size = KB(64);
    MEMORY_BASIC_INFORMATION mbi;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    tap_comment("vmcontract: NOACCESS commit -> 0x%08x", (unsigned)s);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = query(base, &mbi);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(mbi.State, MEM_COMMIT);
    ASSERT_EQ_U32(mbi.Protect, PAGE_NOACCESS);

    {
        PVOID b = base;
        SIZE_T sz = size;
        s = NtAllocateVirtualMemory(&b, 0, &sz, MEM_COMMIT, PAGE_READWRITE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }
    ((volatile unsigned char *)base)[0] = 0x99;
    ASSERT_TRUE(((unsigned char *)base)[0] == 0x99);
    release(base);
    return true;
}

/* MEM_NOZERO commits succeed and the pages are usable (the contents
 * are unspecified, so nothing is asserted about them). */
static bool t_nozero_commit(void)
{
    PVOID base = NULL;
    SIZE_T size = KB(64);
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size,
                                MEM_RESERVE | MEM_COMMIT | MEM_NOZERO,
                                PAGE_READWRITE);
    tap_comment("vmcontract: NOZERO commit -> 0x%08x", (unsigned)s);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    memset(base, 0x5C, size);
    ASSERT_TRUE(((unsigned char *)base)[KB(32)] == 0x5C);
    release(base);
    return true;
}

/* Semantics probe for the per-class committed-page counters: which of
 * Pool/Stack/Image/CachePagesCommitted moves (and by how much) for each
 * kind of allocation.  Pure observation -- every measurement is a TAP
 * comment, asserts only cover call success -- so it runs identically on
 * retail (--official) and here, and the retail run is the contract. */

static void stat_dump(const char *tag, const MM_STATISTICS *st)
{
    tap_comment("statprobe %s: total=%lu avail=%lu vmC=%lu vmR=%lu "
                "cache=%lu pool=%lu stack=%lu image=%lu",
                tag,
                (unsigned long)st->TotalPhysicalPages,
                (unsigned long)st->AvailablePages,
                (unsigned long)st->VirtualMemoryBytesCommitted,
                (unsigned long)st->VirtualMemoryBytesReserved,
                (unsigned long)st->CachePagesCommitted,
                (unsigned long)st->PoolPagesCommitted,
                (unsigned long)st->StackPagesCommitted,
                (unsigned long)st->ImagePagesCommitted);
}

static void stat_delta(const char *tag, const MM_STATISTICS *from,
                       const MM_STATISTICS *to)
{
    tap_comment("statprobe %s: dAvail=%ld dVmC=%ld dCache=%ld dPool=%ld "
                "dStack=%ld dImage=%ld",
                tag,
                (long)to->AvailablePages - (long)from->AvailablePages,
                (long)to->VirtualMemoryBytesCommitted -
                    (long)from->VirtualMemoryBytesCommitted,
                (long)to->CachePagesCommitted - (long)from->CachePagesCommitted,
                (long)to->PoolPagesCommitted - (long)from->PoolPagesCommitted,
                (long)to->StackPagesCommitted - (long)from->StackPagesCommitted,
                (long)to->ImagePagesCommitted - (long)from->ImagePagesCommitted);
}

static NTSTATUS stat_read(MM_STATISTICS *st)
{
    memset(st, 0, sizeof(*st));
    st->Length = sizeof(*st);
    return MmQueryStatistics(st);
}

/* XTEST is this XBE's payload section (defined in xe/sections.c); we
 * only need its header, which the XBE header table provides. */
#define SP_XBE_BASE          ((unsigned char *)0x00010000)
#define SP_XBE_NUM_SECTIONS  (*(DWORD *)(SP_XBE_BASE + 0x11C))
#define SP_XBE_SECTION_TABLE (*(PXBE_SECTION_HEADER *)(SP_XBE_BASE + 0x120))

static PXBE_SECTION_HEADER sp_xtest_section(void)
{
    DWORD i;
    for (i = 0; i < SP_XBE_NUM_SECTIONS; i++) {
        PXBE_SECTION_HEADER s = &SP_XBE_SECTION_TABLE[i];
        const char *n = (const char *)s->SectionName;
        if (n && n[0] == 'X' && n[1] == 'T' && n[2] == 'E' && n[3] == 'S' &&
            n[4] == 'T' && n[5] == '\0')
            return s;
    }
    return NULL;
}

static void NTAPI sp_system_routine(PKSTART_ROUTINE StartRoutine,
                                    PVOID StartContext)
{
    if (StartRoutine != NULL)
        StartRoutine(StartContext);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void NTAPI sp_thread(PVOID arg)
{
    KeSetEvent((PKEVENT)arg, 0, FALSE);
}

static void sp_sleep_ms(LONG ms)
{
    LARGE_INTEGER delay = { .QuadPart = -((LONGLONG)ms * 10000) };
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static bool t_statistics_committed_probe(void)
{
    MM_STATISTICS base, a, b;

    ASSERT_NTSTATUS(stat_read(&base), STATUS_SUCCESS);
    stat_dump("baseline", &base);

    /* Pool, page-sized and up: 64 KiB. */
    {
        PVOID p = ExAllocatePoolWithTag(KB(64), 'PtSp');
        ASSERT_NOT_NULL(p);
        ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
        stat_delta("pool64k-alloc", &base, &a);
        ExFreePool(p);
        ASSERT_NTSTATUS(stat_read(&b), STATUS_SUCCESS);
        stat_delta("pool64k-free", &a, &b);
    }

    /* Pool, sub-page: does a bucket allocation move the counter only
     * when it grabs a fresh page? */
    {
        PVOID p = ExAllocatePoolWithTag(64, 'PtSp');
        ASSERT_NOT_NULL(p);
        ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
        stat_delta("pool64b-alloc", &base, &a);
        ExFreePool(p);
    }

    /* Contiguous memory: pool-accounted, separate, or unaccounted? */
    {
        PVOID p = MmAllocateContiguousMemoryEx(KB(64), 0, 0x03FFFFFF, 0,
                                               PAGE_READWRITE);
        ASSERT_NOT_NULL(p);
        ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
        stat_delta("contig64k-alloc", &base, &a);
        MmFreeContiguousMemory(p);
        ASSERT_NTSTATUS(stat_read(&b), STATUS_SUCCESS);
        stat_delta("contig64k-free", &a, &b);
    }

    /* Bare kernel stack from the title-callable export. */
    {
        PVOID top = MmCreateKernelStack(KB(32), FALSE);
        ASSERT_NOT_NULL(top);
        ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
        stat_delta("kstack32k-create", &base, &a);
        MmDeleteKernelStack(top, (PVOID)((ULONG_PTR)top - KB(32)));
        ASSERT_NTSTATUS(stat_read(&b), STATUS_SUCCESS);
        stat_delta("kstack32k-delete", &a, &b);
    }

    /* Thread with an explicit 64 KiB stack: measure while suspended
     * (stack committed, thread parked), then after exit+close. */
    {
        KEVENT done;
        HANDLE h = NULL;
        NTSTATUS s;
        KeInitializeEvent(&done, NotificationEvent, FALSE);
        s = PsCreateSystemThreadEx(&h, 0, KB(64), 0, NULL, sp_thread, &done,
                                   TRUE, FALSE, sp_system_routine);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
        stat_delta("thread64k-suspended", &base, &a);
        NtResumeThread(h, NULL);
        {
            LARGE_INTEGER timeout = { .QuadPart = -((LONGLONG)2 * 1000 * 10000) };
            s = KeWaitForSingleObject(&done, Executive, KernelMode, FALSE,
                                      &timeout);
        }
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        NtClose(h);
        sp_sleep_ms(100);  /* let the reaper return the stack */
        ASSERT_NTSTATUS(stat_read(&b), STATUS_SUCCESS);
        stat_delta("thread64k-exited", &a, &b);
    }

    /* XBE section pages (XTEST: 3 pages, head/tail shared with
     * neighbors). */
    {
        PXBE_SECTION_HEADER sec = sp_xtest_section();
        ASSERT_NOT_NULL(sec);
        ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
        ASSERT_NTSTATUS(XeUnloadSection(sec), STATUS_SUCCESS);
        ASSERT_NTSTATUS(stat_read(&b), STATUS_SUCCESS);
        stat_delta("xtest-unload", &a, &b);
        ASSERT_NTSTATUS(XeLoadSection(sec), STATUS_SUCCESS);
        ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
        stat_delta("xtest-load", &b, &a);
    }

    /* VM commit must move only vmC/avail, never the class counters. */
    {
        PVOID p = NULL;
        SIZE_T size = MB(1);
        NTSTATUS s = NtAllocateVirtualMemory(&p, 0, &size,
                                             MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        memset(p, 0x5A, size);
        ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
        stat_delta("vm1m-commit", &base, &a);
        release(p);
    }

    /* Cache resize: is the cache inside PoolPagesCommitted or its own
     * class only? */
    {
        PFN_COUNT orig = FscGetCacheSize();
        NTSTATUS s;
        ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
        s = FscSetCacheSize(orig + 16);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
        ASSERT_NTSTATUS(stat_read(&b), STATUS_SUCCESS);
        stat_delta("cache+16", &a, &b);
        s = FscSetCacheSize(orig);
        ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    }

    ASSERT_NTSTATUS(stat_read(&a), STATUS_SUCCESS);
    stat_dump("final", &a);
    return true;
}

static const test_entry_t mm_vmcontract_entries[] = {
    {"reserve_commit_regions",   t_reserve_commit_regions},
    {"size_rounding",            t_size_rounding},
    {"free_status_codes",        t_free_status_codes},
    {"protect_roundtrip",        t_protect_roundtrip},
    {"statistics_track_commit",  t_statistics_track_commit},
    {"top_down",                 t_top_down},
    {"release_middle_splits",    t_release_middle_splits},
    {"noaccess_commit",          t_noaccess_commit},
    {"nozero_commit",            t_nozero_commit},
    {"statistics_committed_probe", t_statistics_committed_probe},
};

DEFINE_GROUP(mm_vmcontract, "mm/vmcontract");
