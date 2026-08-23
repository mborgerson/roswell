/*
 * MmAllocateContiguousMemoryEx + MmFreeContiguousMemory + MmQueryStatistics +
 * MmGetPhysicalAddress.
 */

#include "../harness.h"
#include <stdio.h>
#include <string.h>

static bool t_alloc_one_page(void)
{
    PVOID p = MmAllocateContiguousMemoryEx(0x1000, 0, 0x03FFFFFF, 0,
                                           PAGE_READWRITE);
    ASSERT_NOT_NULL(p);
    /* Touch the page so we know it's mapped read-write. */
    *(volatile ULONG *)p = 0xDEADBEEF;
    if (*(volatile ULONG *)p != 0xDEADBEEF) {
        MmFreeContiguousMemory(p);
        FAIL_AND_RETURN("readback mismatch");
    }
    MmFreeContiguousMemory(p);
    return true;
}

static bool t_alloc_aligned(void)
{
    PVOID p = MmAllocateContiguousMemoryEx(0x4000, 0, 0x03FFFFFF, 0x4000,
                                           PAGE_READWRITE);
    ASSERT_NOT_NULL(p);
    if ((ULONG_PTR)p & 0x3FFF) {
        MmFreeContiguousMemory(p);
        FAIL_AND_RETURN("alignment violation: %p", p);
    }
    MmFreeContiguousMemory(p);
    return true;
}

static bool t_physical_in_range(void)
{
    PVOID p = MmAllocateContiguousMemoryEx(0x1000, 0, 0x03FFFFFF, 0,
                                           PAGE_READWRITE);
    ASSERT_NOT_NULL(p);
    ULONG_PTR phys = MmGetPhysicalAddress(p);
    if (phys == 0 || phys > 0x03FFFFFF) {
        MmFreeContiguousMemory(p);
        FAIL_AND_RETURN("phys 0x%lx out of [0, 0x3FFFFFF]", (unsigned long)phys);
    }
    MmFreeContiguousMemory(p);
    return true;
}

static bool t_query_statistics(void)
{
    MM_STATISTICS stats = { 0 };
    stats.Length = sizeof(stats);
    NTSTATUS s = MmQueryStatistics(&stats);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(stats.TotalPhysicalPages > 0);
    /* 64 MB retail (16384 pages) or 128 MB devkit (32768 pages); allow
     * either, minus up to 1 MB the kernel may reserve for hardware
     * windows / framebuffer / firmware. */
    BOOL ok_64mb  = (stats.TotalPhysicalPages >= 16384 - 256 && stats.TotalPhysicalPages <= 16384);
    BOOL ok_128mb = (stats.TotalPhysicalPages >= 32768 - 1024 && stats.TotalPhysicalPages <= 32768);
    if (!ok_64mb && !ok_128mb) {
        FAIL_AND_RETURN("unexpected TotalPhysicalPages: %lu",
                        (unsigned long)stats.TotalPhysicalPages);
    }
    ASSERT_TRUE(stats.AvailablePages > 0);
    ASSERT_TRUE(stats.AvailablePages <= stats.TotalPhysicalPages);
    return true;
}

/*
 * Physical-placement probe: record (never assert) where the allocator
 * puts single VM-commit pages, pool pages, and contiguous runs, before
 * and after churn.  Run against retail (--official) this pins the
 * placement policy the one-pool allocator must match -- in particular
 * whether kernel-transient singles drain toward low or high PAs, and
 * whether the FB zone at the top stays clean for contiguous requests.
 */
static ULONG probe_pa(PVOID va)
{
    return (ULONG)MmGetPhysicalAddress(va);
}

static bool t_placement_probe(void)
{
    PVOID vm[8] = {0}; PVOID pool[4] = {0};
    PVOID big = NULL, contig = NULL;
    SIZE_T sz;
    NTSTATUS st;
    int i;

    /* 0a. Memory statistics as the title sees them. */
    {
        MM_STATISTICS st = { .Length = sizeof(MM_STATISTICS) };
        if (MmQueryStatistics(&st) == STATUS_SUCCESS)
            tap_comment("probe: stats total=%lu avail=%lu vmcommit=%lu "
                        "pool=%lu stack=%lu image=%lu",
                        (unsigned long)st.TotalPhysicalPages,
                        (unsigned long)st.AvailablePages,
                        (unsigned long)st.VirtualMemoryBytesCommitted,
                        (unsigned long)st.PoolPagesCommitted,
                        (unsigned long)st.StackPagesCommitted,
                        (unsigned long)st.ImagePagesCommitted);
    }

    /* 0. Where does the running XBE image itself live physically? */
    tap_comment("probe: xbe image PA @10000=%08lx @11000=%08lx @20000=%08lx @40000=%08lx",
                probe_pa((PVOID)0x10000), probe_pa((PVOID)0x11000),
                probe_pa((PVOID)0x20000), probe_pa((PVOID)0x40000));

    /* 1. Eight single-page VM commits. */
    for (i = 0; i < 8; i++) {
        sz = 0x1000; vm[i] = NULL;
        st = NtAllocateVirtualMemory(&vm[i], 0, &sz,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (st != STATUS_SUCCESS) FAIL_AND_RETURN("vm commit %d -> %08x", i, (unsigned)st);
        *(volatile UCHAR *)vm[i] = 1;
    }
    tap_comment("probe: vm singles PA: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                probe_pa(vm[0]), probe_pa(vm[1]), probe_pa(vm[2]), probe_pa(vm[3]),
                probe_pa(vm[4]), probe_pa(vm[5]), probe_pa(vm[6]), probe_pa(vm[7]));

    /* 2. Pool pages (one fresh page per 4 KB allocation). */
    for (i = 0; i < 4; i++) {
        pool[i] = ExAllocatePoolWithTag(0x1000, 'borP');
        if (pool[i] == NULL) FAIL_AND_RETURN("pool alloc %d failed", i);
    }
    tap_comment("probe: pool pages PA: %08lx %08lx %08lx %08lx",
                probe_pa(pool[0]), probe_pa(pool[1]),
                probe_pa(pool[2]), probe_pa(pool[3]));

    /* 3. A large (1 MB) pool block -- first and last page. */
    big = ExAllocatePoolWithTag(0x100000, 'borP');
    if (big != NULL)
        tap_comment("probe: 1MB pool PA first=%08lx last=%08lx",
                    probe_pa(big), probe_pa((PUCHAR)big + 0xFF000));

    /* 4. Unconstrained contig (FB-style) amid the live singles. */
    contig = MmAllocateContiguousMemoryEx(0x40000, 0, 0x03FFFFFF, 0x4000,
                                          PAGE_READWRITE);
    if (contig != NULL)
        tap_comment("probe: 256KB contig PA=%08lx", probe_pa(contig));

    /* 4b. Relocation probe: pin an exact window COVERING a live VM
     * page.  If the pin succeeds and the VM page's content survives at
     * a NEW physical address, the allocator relocates in-use pages. */
    {
        ULONG before = probe_pa(vm[0]);
        ULONG win_lo = before & ~0xFFFFFUL;          /* 1 MB-aligned window */
        PVOID pin;
        *(volatile ULONG *)vm[0] = 0x52454C4F;       /* 'OLER' */
        pin = MmAllocateContiguousMemoryEx(0x100000, win_lo,
                                           win_lo + 0xFFFFF, 0,
                                           PAGE_READWRITE);
        tap_comment("probe: reloc pin [%08lx,+1MB) over vm page %08lx -> %s",
                    win_lo, before, pin ? "OK" : "NULL");
        if (pin != NULL) {
            ULONG after = probe_pa(vm[0]);
            tap_comment("probe: vm page after pin PA=%08lx content=%s",
                        after,
                        (*(volatile ULONG *)vm[0] == 0x52454C4F) ? "intact"
                                                                 : "LOST");
            MmFreeContiguousMemory(pin);
        }
    }

    /* 4c. Same relocation probe over a live POOL page: does the
     * allocator treat kernel pool as movable too, or do pool pages
     * block exact-window pins? */
    {
        ULONG before = probe_pa(pool[0]);
        ULONG win_lo = before & ~0xFFFFFUL;
        PVOID pin;
        *(volatile ULONG *)pool[0] = 0x4C4F4F50;     /* 'POOL' */
        pin = MmAllocateContiguousMemoryEx(0x100000, win_lo,
                                           win_lo + 0xFFFFF, 0,
                                           PAGE_READWRITE);
        tap_comment("probe: reloc pin [%08lx,+1MB) over pool page %08lx -> %s",
                    win_lo, before, pin ? "OK" : "NULL");
        if (pin != NULL) {
            tap_comment("probe: pool page after pin PA=%08lx content=%s",
                        probe_pa(pool[0]),
                        (*(volatile ULONG *)pool[0] == 0x4C4F4F50) ? "intact"
                                                                   : "LOST");
            MmFreeContiguousMemory(pin);
        }
    }

    /* 5. Free a low single, commit again -- recycling order. */
    sz = 0; st = NtFreeVirtualMemory(&vm[3], &sz, MEM_RELEASE);
    {
        PVOID re = NULL; sz = 0x1000;
        st = NtAllocateVirtualMemory(&re, 0, &sz,
                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (st == STATUS_SUCCESS) {
            *(volatile UCHAR *)re = 1;
            tap_comment("probe: recommit after free PA=%08lx", probe_pa(re));
            sz = 0; NtFreeVirtualMemory(&re, &sz, MEM_RELEASE);
        }
        vm[3] = NULL;
    }

    if (contig) MmFreeContiguousMemory(contig);
    if (big) ExFreePool(big);
    for (i = 0; i < 4; i++) ExFreePool(pool[i]);
    for (i = 0; i < 8; i++) {
        if (vm[i]) { sz = 0; NtFreeVirtualMemory(&vm[i], &sz, MEM_RELEASE); }
    }
    return true;
}

/* Which fixed physical regions does the kernel leave title-allocatable?
 * Exact-window single-page pins over the low 64 KiB, the kernel slot,
 * and the top (GPU/instance) region.  Pure observation: one comment
 * line, no asserts -- run --official for the retail contract. */
static bool t_region_probe(void)
{
    static const ULONG Pas[] = {
        0x00000000, 0x00001000, 0x00005000, 0x0000E000, 0x0000F000,
        0x00010000, 0x00030000, 0x00060000,
        0x03EF0000, 0x03F00000, 0x03FA0000,
        0x03FE0000, 0x03FF0000, 0x03FFF000,
    };
    char line[160];
    int n = 0;

    for (ULONG i = 0; i < sizeof(Pas)/sizeof(Pas[0]); i++) {
        PVOID p = MmAllocateContiguousMemoryEx(0x1000, Pas[i],
                                               Pas[i] + 0xFFF, 0,
                                               PAGE_READWRITE);
        n += snprintf(line + n, sizeof(line) - n, " %05lx=%c",
                      (unsigned long)(Pas[i] >> 12), p ? 'F' : '-');
        if (p) MmFreeContiguousMemory(p);
        if (n > (int)sizeof(line) - 16) break;
    }
    line[n] = '\0';
    tap_comment("regions (F=allocatable):%s", line);
    return true;
}

/* MmAllocateSystemMemory hands out a system VA (the documented
 * 0xD0000000 window); pages are usable and MmFreeSystemMemory returns
 * the page count. */
static bool t_system_memory(void)
{
    PVOID p = MmAllocateSystemMemory(8192, PAGE_READWRITE);
    ASSERT_NOT_NULL(p);
    tap_comment("sysmem: va=%p", p);
    ASSERT_TRUE((ULONG_PTR)p >= 0xD0000000UL && (ULONG_PTR)p < 0xF0000000UL);
    memset(p, 0xA5, 8192);
    ASSERT_TRUE(((volatile UCHAR *)p)[8191] == 0xA5);
    ASSERT_EQ_U32(MmFreeSystemMemory(p, 8192), 2);
    return true;
}

/* How low does the general single-page allocator reach?  Commit pages
 * one at a time over a big reservation until exhaustion, tracking the
 * minimum physical address handed out; then try exact pins in the low
 * 64 KiB while everything else is gone.  Observation only -- the
 * retail run is the contract for whether the low pages are general-
 * purpose or pin-only. */
static bool t_low_floor_probe(void)
{
    PVOID base = NULL;
    SIZE_T size = 56 * 1024 * 1024;
    ULONG minPa = 0xFFFFFFFF;
    ULONG count = 0;
    NTSTATUS s;

    s = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    for (count = 0; count < (56 * 1024 * 1024) / 4096; count++) {
        PVOID at = (PUCHAR)base + (SIZE_T)count * 4096;
        SIZE_T one = 4096;
        ULONG pa;
        if (!NT_SUCCESS(NtAllocateVirtualMemory(&at, 0, &one, MEM_COMMIT,
                                                PAGE_READWRITE)))
            break;
        pa = (ULONG)(ULONG_PTR)MmGetPhysicalAddress(at);
        if (pa < minPa) minPa = pa;
    }

    tap_comment("low-floor: %lu pages committed, min PA %08lx",
                (unsigned long)count, (unsigned long)minPa);

    {
        char line[80];
        int n = 0;
        static const ULONG Pas[] = { 0x1000, 0x5000, 0xE000 };
        for (ULONG i = 0; i < 3; i++) {
            PVOID p = MmAllocateContiguousMemoryEx(0x1000, Pas[i],
                                                   Pas[i] + 0xFFF, 0,
                                                   PAGE_READWRITE);
            n += snprintf(line + n, sizeof(line) - n, " %05lx=%c",
                          (unsigned long)(Pas[i] >> 12), p ? 'F' : '-');
            if (p) MmFreeContiguousMemory(p);
        }
        line[n] = '\0';
        tap_comment("low-floor at exhaustion:%s", line);
    }

    {
        PVOID b = base;
        SIZE_T sz = 0;
        NtFreeVirtualMemory(&b, &sz, MEM_RELEASE);
    }
    return true;
}

/* Map the kernel-reserved regions of physical RAM.  Early in the run
 * (RAM mostly free), attempt an exact one-page pin at every PFN; pages
 * the kernel refuses are reserved (its image, PFN/page database, GPU
 * instance/padding, low hardware pages).  Reserved runs are RLE'd so
 * the layout is legible.  Run --official to see retail's map and diff
 * it against ours -- the question is where retail keeps the structures
 * we carve from general RAM. */
static bool t_phys_map_probe(void)
{
    ULONG pa;
    ULONG runStart = 0;
    bool runReserved = false;
    bool have = false;
    ULONG totalReserved = 0;

    tap_comment("physmap: reserved runs (PA range : pages) --");
    for (pa = 0x1000; pa < 0x04000000; pa += 0x1000) {
        PVOID p = MmAllocateContiguousMemoryEx(0x1000, pa, pa + 0xFFF, 0,
                                               PAGE_READWRITE);
        bool reserved = (p == NULL);
        if (p) MmFreeContiguousMemory(p);
        else   totalReserved++;

        if (!have) { runStart = pa; runReserved = reserved; have = true; }
        else if (reserved != runReserved) {
            if (runReserved)
                tap_comment("physmap:  %08lx-%08lx  %lu",
                            (unsigned long)runStart, (unsigned long)pa,
                            (unsigned long)((pa - runStart) >> 12));
            runStart = pa; runReserved = reserved;
        }
    }
    if (have && runReserved)
        tap_comment("physmap:  %08lx-%08lx  %lu",
                    (unsigned long)runStart, (unsigned long)0x04000000,
                    (unsigned long)((0x04000000 - runStart) >> 12));
    tap_comment("physmap: total reserved (incl. PFN 0) = %lu pages",
                (unsigned long)totalReserved + 1);
    return true;
}

static const test_entry_t mm_contig_entries[] = {
    {"phys_map_probe",     t_phys_map_probe},
    {"low_floor_probe",    t_low_floor_probe},
    {"system_memory",      t_system_memory},
    {"region_probe",       t_region_probe},
    {"placement_probe",    t_placement_probe},
    {"alloc_one_page",     t_alloc_one_page},
    {"alloc_aligned",      t_alloc_aligned},
    {"physical_in_range",  t_physical_in_range},
    {"query_statistics",   t_query_statistics},
};

DEFINE_GROUP(mm_contig, "mm/contig");
