/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Xbox contiguous-memory ordinals.
 *
 * With the nxmm physical allocator (NXK_MM_PHYS), contiguous requests
 * are served straight from the one shared page database: a top-down run
 * scan over NxkPageSupply within the caller's [Lowest, Highest] window,
 * like retail, where contiguous and single-page allocations compete for
 * the same free RAM.  The NxContigTable of {Alias, Pa, PaEnd, Size}
 * entries only records live allocations so free/query can recover the
 * page run and the original request size.
 *
 * The pre-unification arena (NxHeapRegions carved off MxFreeDescriptor
 * at boot, with hole-scanning between table entries and an NT slow-path
 * fallback) remains below under !NXK_MM_PHYS as the compile-time revert
 * path.
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>
#include <xb-debug.h>

#include <mm/ARM3/miarm.h>

#include "mm.h"

/* HEAP-REGION STATE ********************************************************/

#ifndef NXK_MM_PHYS
/* Populated by NxpAddHeapPagesCoalesced during boot reclaim; read by both
 * init.c (debug dump) and the heap allocator below. */
NXK_HEAP_REGION NxHeapRegions[NXK_MAX_HEAP_REGIONS];
ULONG           NxHeapRegionCount;
#endif

#ifndef NXK_MM_PHYS
VOID
NxpAddHeapPagesCoalesced(ULONG_PTR Pa, ULONG Pages)
{
    if (Pages == 0) return;

    /* Try to extend the previous region if it abuts Pa. */
    if (NxHeapRegionCount > 0)
    {
        NXK_HEAP_REGION *Prev = &NxHeapRegions[NxHeapRegionCount - 1];
        if (Prev->Pa + ((ULONG_PTR)Prev->Pages << PAGE_SHIFT) == Pa)
        {
            Prev->Pages += Pages;
            return;
        }
    }
    if (NxHeapRegionCount >= NXK_MAX_HEAP_REGIONS)
    {
        DPRINT1("heap region table full, dropping PA=%08lx pages=%lu\n",
                (ULONG)Pa, Pages);
        return;
    }
    NxHeapRegions[NxHeapRegionCount].Pa = Pa;
    NxHeapRegions[NxHeapRegionCount].Pages = Pages;
    NxHeapRegionCount++;
}
#endif /* !NXK_MM_PHYS */

/* CONTIGUOUS-ALLOCATION TRACKING TABLE *************************************/

/*
 * Tracking: a fixed-size array of {Alias, Pa, PaEnd, Size} entries sorted
 * implicitly by walk order.  Alloc walks for a hole between entries, free
 * just clears its slot -- coalescing happens naturally because the next
 * allocation re-scans gaps.  64 slots is enough for any title's contig
 * working set (typically <10 live allocations).
 */
typedef struct _NXK_CONTIG_ENTRY
{
    PVOID     Alias;        /* KSEG0 pointer returned to the title (0 = free slot) */
    ULONG_PTR Pa;           /* page-aligned PA inside our carve-out */
    ULONG_PTR PaEnd;        /* PA past the last byte (Pa + AllocBytes) */
    SIZE_T    Size;         /* original request size, for MmQueryAllocationSize */
} NXK_CONTIG_ENTRY;

#define NXK_CONTIG_SLOTS    64

static NXK_CONTIG_ENTRY NxContigTable[NXK_CONTIG_SLOTS];
static KSPIN_LOCK NxContigLock;

/* POST-BOOT REGION DONATION ************************************************/

/*
 * Insert [Pa, Pa + Pages<<SHIFT) into the region table, keeping it sorted
 * by PA and coalescing with abutting neighbors.  The boot-reclaim path
 * (NxpAddHeapPagesCoalesced) only ever appends in ascending PA order;
 * donated kernel-image tail pages arrive after the table is built and can
 * land below existing regions, so this does a full sorted insert.
 * LowReserve regions keep their boundary (no coalesce across the flag).
 * Takes NxContigLock -- by donation time (Phase 1) other threads are live.
 */
#ifdef NXK_MM_PHYS
/* One pool: donated pages simply become free pages. */
VOID
NxkMmDonateHeapPages(ULONG_PTR Pa, ULONG Pages)
{
    KIRQL OldIrql = MiAcquirePfnLock();
    ULONG i;
    for (i = 0; i < Pages; i++)
        NxkPageSupplyReturn((Pa >> PAGE_SHIFT) + i);
    MiReleasePfnLock(OldIrql);
}
#else
VOID
NxkMmDonateHeapPages(ULONG_PTR Pa, ULONG Pages)
{
    ULONG_PTR PaEnd = Pa + ((ULONG_PTR)Pages << PAGE_SHIFT);
    KIRQL OldIrql;
    ULONG i, j;

    if (Pages == 0) return;

    KeAcquireSpinLock(&NxContigLock, &OldIrql);

    /* Find the first region above the donated range. */
    for (i = 0; i < NxHeapRegionCount; i++)
        if (NxHeapRegions[i].Pa > Pa) break;

    /* Extend the region below if it abuts. */
    if (i > 0 && !NxHeapRegions[i - 1].LowReserve &&
        NxHeapRegions[i - 1].Pa +
            ((ULONG_PTR)NxHeapRegions[i - 1].Pages << PAGE_SHIFT) == Pa)
    {
        NxHeapRegions[i - 1].Pages += Pages;

        /* The grown region may now also abut the one above. */
        if (i < NxHeapRegionCount && !NxHeapRegions[i].LowReserve &&
            NxHeapRegions[i].Pa == PaEnd)
        {
            NxHeapRegions[i - 1].Pages += NxHeapRegions[i].Pages;
            for (j = i; j + 1 < NxHeapRegionCount; j++)
                NxHeapRegions[j] = NxHeapRegions[j + 1];
            NxHeapRegionCount--;
            NxHeapRegions[NxHeapRegionCount].Pa = 0;
            NxHeapRegions[NxHeapRegionCount].Pages = 0;
            NxHeapRegions[NxHeapRegionCount].LowReserve = FALSE;
        }
        goto done;
    }

    /* Extend the region above if it abuts. */
    if (i < NxHeapRegionCount && !NxHeapRegions[i].LowReserve &&
        NxHeapRegions[i].Pa == PaEnd)
    {
        NxHeapRegions[i].Pa = Pa;
        NxHeapRegions[i].Pages += Pages;
        goto done;
    }

    /* Standalone region: shift the table up and insert at i. */
    if (NxHeapRegionCount >= NXK_MAX_HEAP_REGIONS)
    {
        DPRINT1("heap region table full, dropping donated PA=%08lx pages=%lu\n",
                (ULONG)Pa, Pages);
        goto done;
    }
    for (j = NxHeapRegionCount; j > i; j--)
        NxHeapRegions[j] = NxHeapRegions[j - 1];
    NxHeapRegions[i].Pa = Pa;
    NxHeapRegions[i].Pages = Pages;
    NxHeapRegions[i].LowReserve = FALSE;
    NxHeapRegionCount++;

done:
    KeReleaseSpinLock(&NxContigLock, OldIrql);
}
#endif /* !NXK_MM_PHYS */

/* HEAP ALLOCATOR ***********************************************************/

/*
 * First-fit alloc from one heap region [Region.Pa, Region.Pa + Pages<<SHIFT),
 * walking top-down to match the retail Xbox kernel: contig requests land at
 * the highest free PA so low PAs stay available for HighestAcceptableAddress
 * < 16 MB-constrained allocs and so titles that pin specific high-PA
 * regions don't trip the heap into fragmentation.
 *
 * Each pass picks the highest tracked entry with PaEnd <= NextEnd that
 * overlaps this region; the candidate hole is [that->PaEnd, NextEnd).
 * Returns the PA of the chunk plus the slot it occupies, or 0 if this
 * region can't satisfy.  Caller must hold NxContigLock.
 */
#ifndef NXK_MM_PHYS
static
ULONG_PTR
NxpHeapAllocFromRegionLocked(const NXK_HEAP_REGION *Region,
                             SIZE_T AllocBytes, ULONG_PTR Alignment,
                             ULONG_PTR Low, ULONG_PTR High, ULONG Slot)
{
    ULONG_PTR HeapStart = Region->Pa;
    ULONG_PTR HeapEnd   = Region->Pa + ((ULONG_PTR)Region->Pages << PAGE_SHIFT);
    ULONG_PTR ClampLow, ClampHigh, NextEnd;

    if (Region->Pages == 0) return 0;

    ClampLow  = (Low  > HeapStart) ? Low  : HeapStart;
    if (High < HeapEnd - 1) ClampHigh = High + 1;
    else                    ClampHigh = HeapEnd;
    if (ClampLow >= ClampHigh) return 0;

    NextEnd = ClampHigh;
    for (;;)
    {
        ULONG_PTR PrevEnd = ClampLow;
        ULONG j, picked = NXK_CONTIG_SLOTS;

        for (j = 0; j < NXK_CONTIG_SLOTS; j++)
        {
            if (NxContigTable[j].Alias == NULL) continue;
            if (NxContigTable[j].Pa < HeapStart) continue;       /* other region */
            if (NxContigTable[j].Pa >= HeapEnd)  continue;       /* other region */
            if (NxContigTable[j].PaEnd > NextEnd) continue;
            if (picked == NXK_CONTIG_SLOTS ||
                NxContigTable[j].PaEnd > NxContigTable[picked].PaEnd)
                picked = j;
        }
        if (picked != NXK_CONTIG_SLOTS && NxContigTable[picked].PaEnd > PrevEnd)
            PrevEnd = NxContigTable[picked].PaEnd;

        /* Try fitting against the top of [PrevEnd, NextEnd).  Round the
         * candidate base DOWN to the alignment boundary so the result is
         * aligned without overshooting the hole. */
        if (AllocBytes <= NextEnd - PrevEnd)
        {
            ULONG_PTR Try = (NextEnd - AllocBytes) & ~(Alignment - 1);
            if (Try >= PrevEnd)
            {
                UNREFERENCED_PARAMETER(Slot);
                return Try;
            }
        }

        if (picked == NXK_CONTIG_SLOTS) return 0;
        NextEnd = NxContigTable[picked].Pa;
        if (NextEnd <= ClampLow) return 0;
    }
}

/*
 * First-fit alloc across all heap regions, top-down to match retail.
 * Pass 1 walks non-LowReserve regions from highest PA to lowest so the
 * topmost region (typically the upper 3.875 MB reclaimed from the
 * framebuffer-reservation block) wins first.  Pass 2 falls back to
 * LowReserve regions but ONLY for requests with High < NXK_LOW_PA_THRESHOLD
 * (anything that doesn't need the low constraint should never end up
 * there).  Returns 0 if no region can satisfy the request.  Caller must
 * hold NxContigLock.
 */
static
ULONG_PTR
NxpHeapAllocLocked(SIZE_T AllocBytes, ULONG_PTR Alignment, ULONG_PTR Low,
                   ULONG_PTR High, ULONG *OutSlot)
{
    ULONG slot;
    LONG i;

    if (AllocBytes == 0) return 0;
    if (Alignment < PAGE_SIZE) Alignment = PAGE_SIZE;

    /* Find a free table slot first; refuse the alloc if we can't track it. */
    for (slot = 0; slot < NXK_CONTIG_SLOTS; slot++)
        if (NxContigTable[slot].Alias == NULL) break;
    if (slot == NXK_CONTIG_SLOTS) return 0;

    /* Pass 1: non-reserve regions, highest-PA region first. */
    for (i = (LONG)NxHeapRegionCount - 1; i >= 0; i--)
    {
        ULONG_PTR Pa;
        if (NxHeapRegions[i].LowReserve) continue;
        Pa = NxpHeapAllocFromRegionLocked(&NxHeapRegions[i],
                                          AllocBytes, Alignment,
                                          Low, High, slot);
        if (Pa != 0)
        {
            *OutSlot = slot;
            return Pa;
        }
    }
    /* Pass 2: low-reserve regions, only for constrained-low requests. */
    if (High < NXK_LOW_PA_THRESHOLD)
    {
        for (i = (LONG)NxHeapRegionCount - 1; i >= 0; i--)
        {
            ULONG_PTR Pa;
            if (!NxHeapRegions[i].LowReserve) continue;
            Pa = NxpHeapAllocFromRegionLocked(&NxHeapRegions[i],
                                              AllocBytes, Alignment,
                                              Low, High, slot);
            if (Pa != 0)
            {
                *OutSlot = slot;
                return Pa;
            }
        }
    }
    return 0;
}
#endif /* !NXK_MM_PHYS */

static
VOID
NxpRecordHeapContigLocked(ULONG Slot, PVOID Alias, ULONG_PTR Pa,
                          SIZE_T AllocBytes, SIZE_T RequestSize)
{
    NxContigTable[Slot].Alias = Alias;
    NxContigTable[Slot].Pa    = Pa;
    NxContigTable[Slot].PaEnd = Pa + AllocBytes;
    NxContigTable[Slot].Size  = RequestSize;
}

#ifdef NXK_MM_PHYS
static
BOOLEAN
NxpHeapFree(PVOID Alias, SIZE_T *OutSize)
{
    KIRQL OldIrql;
    ULONG_PTR Pa = 0, PaEnd = 0;
    BOOLEAN Found = FALSE;
    ULONG i;

    if (OutSize) *OutSize = 0;
    KeAcquireSpinLock(&NxContigLock, &OldIrql);
    for (i = 0; i < NXK_CONTIG_SLOTS; i++)
    {
        if (NxContigTable[i].Alias == Alias)
        {
            if (OutSize) *OutSize = NxContigTable[i].Size;
            Pa    = NxContigTable[i].Pa;
            PaEnd = NxContigTable[i].PaEnd;
            NxContigTable[i].Alias = NULL;
            NxContigTable[i].Pa    = 0;
            NxContigTable[i].PaEnd = 0;
            NxContigTable[i].Size  = 0;
            Found = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&NxContigLock, OldIrql);

    if (Found && PaEnd > Pa)
    {
        OldIrql = MiAcquirePfnLock();
        while (Pa < PaEnd)
        {
            NxkPageSupplyReturn(Pa >> PAGE_SHIFT);
            Pa += PAGE_SIZE;
        }
        MiReleasePfnLock(OldIrql);
    }
    return Found;
}
#else
static
BOOLEAN
NxpHeapFree(PVOID Alias, SIZE_T *OutSize)
{
    KIRQL OldIrql;
    BOOLEAN Found = FALSE;
    PVOID NtSystemVa = NULL;
    ULONG i;

    if (OutSize) *OutSize = 0;
    KeAcquireSpinLock(&NxContigLock, &OldIrql);
    for (i = 0; i < NXK_CONTIG_SLOTS; i++)
    {
        if (NxContigTable[i].Alias == Alias)
        {
            if (OutSize) *OutSize = NxContigTable[i].Size;
            /* NT-allocator slow-path entries have Pa==0 and stash the
             * SystemVa in PaEnd so the underlying allocation can be freed. */
            if (NxContigTable[i].Pa == 0 && NxContigTable[i].PaEnd != 0)
                NtSystemVa = (PVOID)NxContigTable[i].PaEnd;
            NxContigTable[i].Alias = NULL;
            NxContigTable[i].Pa    = 0;
            NxContigTable[i].PaEnd = 0;
            NxContigTable[i].Size  = 0;
            Found = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&NxContigLock, OldIrql);

    if (NtSystemVa != NULL)
        MmFreeContiguousMemory(NtSystemVa);
    return Found;
}
#endif /* NXK_MM_PHYS */

/* XBOX ORDINALS ************************************************************/

#ifdef NXK_MM_PHYS
PVOID
NTAPI
NxMmAllocateContiguousMemoryEx(
    IN SIZE_T NumberOfBytes,
    IN ULONG_PTR LowestAcceptableAddress,
    IN ULONG_PTR HighestAcceptableAddress,
    IN ULONG_PTR Alignment,
    IN ULONG Protect)
{
    SIZE_T AllocBytes;
    PFN_NUMBER RunPages, LowPfn, HighPfn, AlignPages, Base;
    PVOID Alias;
    ULONG_PTR Pa;
    KIRQL OldIrql;
    ULONG Slot;

    UNREFERENCED_PARAMETER(Protect);    /* TODO: WC/UC PSE alias selection */

    NxkMmEnsureXboxWindows();

    AllocBytes = (NumberOfBytes + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
    if (AllocBytes == 0) return NULL;
    RunPages = AllocBytes >> PAGE_SHIFT;

    /* Contiguous memory is title-window RAM: clamp the request below the
     * 64 MB KSEG0 alias even on a 128 MB devkit, where the upper half is
     * kernel-only and has no title-visible alias. */
    if (HighestAcceptableAddress > NXK_KSEG0_RAM_SIZE - 1)
        HighestAcceptableAddress = NXK_KSEG0_RAM_SIZE - 1;

    /* Generic windows stop below the NV2A instance region (retail's
     * generic contigs land below it; only explicit pins with a low
     * bound inside the region take those pages, and a title's
     * MmClaimGpuInstanceMemory must find them reservable). */
    if (LowestAcceptableAddress < NXK_NV2A_INSTANCE_BASE &&
        HighestAcceptableAddress >= NXK_NV2A_INSTANCE_BASE)
        HighestAcceptableAddress = NXK_NV2A_INSTANCE_BASE - 1;

    LowPfn = (LowestAcceptableAddress + PAGE_SIZE - 1) >> PAGE_SHIFT;
    HighPfn = (PFN_NUMBER)
        ((((ULONGLONG)HighestAcceptableAddress + 1) >> PAGE_SHIFT) - 1);
    AlignPages = (Alignment <= PAGE_SIZE) ? 1 : (Alignment >> PAGE_SHIFT);

    /* Reserve a tracking slot, take the run, record it.  Lock order:
     * NxContigLock, then the PFN lock. */
    KeAcquireSpinLock(&NxContigLock, &OldIrql);
    for (Slot = 0; Slot < NXK_CONTIG_SLOTS; Slot++)
        if (NxContigTable[Slot].Alias == NULL) break;
    if (Slot == NXK_CONTIG_SLOTS)
    {
        KeReleaseSpinLock(&NxContigLock, OldIrql);
        DPRINT1("MmAllocContig: tracking table full\n");
        return NULL;
    }
    {
        KIRQL PfnIrql = MiAcquirePfnLock();
        Base = NxkPageSupplyTakeRun(RunPages, LowPfn, HighPfn, AlignPages);
        if (Base == 0)
        {
            /* Retail-probed: contiguous requests may displace live VM
             * pages from the window (copy + PTE retarget). */
            Base = NxkPageSupplyTakeRunRelocate(RunPages, LowPfn, HighPfn,
                                                AlignPages);
        }
        MiReleasePfnLock(PfnIrql);
    }
    if (Base == 0)
    {
        /* Empty pool bucket pages are invisible to the supply until
         * swept; reclaim and retry once before failing. */
        extern ULONG NxpReclaimEmptyBucketPages(VOID);
        KeReleaseSpinLock(&NxContigLock, OldIrql);
        if (NxpReclaimEmptyBucketPages() != 0)
        {
            KIRQL PfnIrql;
            KeAcquireSpinLock(&NxContigLock, &OldIrql);
            PfnIrql = MiAcquirePfnLock();
            Base = NxkPageSupplyTakeRun(RunPages, LowPfn, HighPfn,
                                        AlignPages);
            if (Base == 0)
                Base = NxkPageSupplyTakeRunRelocate(RunPages, LowPfn,
                                                    HighPfn, AlignPages);
            MiReleasePfnLock(PfnIrql);
            if (Base != 0)
                goto Got;
            KeReleaseSpinLock(&NxContigLock, OldIrql);
        }
        XbDbg("MmAllocateContiguousMemoryEx(%lu) -- no memory "
              "(low=%08lx high=%08lx align=%lx avail=%lu)\n",
              (ULONG)NumberOfBytes, (ULONG)LowestAcceptableAddress,
              (ULONG)HighestAcceptableAddress, (ULONG)Alignment,
              (ULONG)MmAvailablePages);
        return NULL;
    }
Got:
    Pa = (ULONG_PTR)Base << PAGE_SHIFT;
    Alias = (PVOID)(NXK_KSEG0_BASE | Pa);
    NxpRecordHeapContigLocked(Slot, Alias, Pa, AllocBytes, NumberOfBytes);
    KeReleaseSpinLock(&NxContigLock, OldIrql);

    DPRINT("MmAllocContig req=%lu got=%lu KB at PA=%08lx avail=%lu\n",
          (ULONG)NumberOfBytes, (ULONG)(AllocBytes >> 10),
          (ULONG)Pa, (ULONG)MmAvailablePages);
    return Alias;
}
#else /* !NXK_MM_PHYS */
PVOID
NTAPI
NxMmAllocateContiguousMemoryEx(
    IN SIZE_T NumberOfBytes,
    IN ULONG_PTR LowestAcceptableAddress,
    IN ULONG_PTR HighestAcceptableAddress,
    IN ULONG_PTR Alignment,
    IN ULONG Protect)
{
    SIZE_T AllocBytes;
    PVOID Alias;
    ULONG_PTR Pa;
    KIRQL OldIrql;
    ULONG Slot = 0;

    UNREFERENCED_PARAMETER(Protect);    /* TODO: WC/UC PSE alias selection */

    NxkMmEnsureXboxWindows();

    /* Page-align the request and add headroom for stricter alignment.  We
     * always carve from our reserved chunk; the retail kernel doesn't have a
     * fallback either -- if physical contig isn't available, MmAllocate*
     * returns NULL and the title fails its init.  Our carve-out is sized
     * (40 MB) to match retail-style "all of RAM minus the kernel" so titles
     * can take what they need. */
    AllocBytes = (NumberOfBytes + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);

    KeAcquireSpinLock(&NxContigLock, &OldIrql);
    Pa = NxpHeapAllocLocked(AllocBytes, Alignment, LowestAcceptableAddress,
                            HighestAcceptableAddress, &Slot);
    if (Pa != 0)
    {
        Alias = (PVOID)(NXK_KSEG0_BASE | Pa);
        NxpRecordHeapContigLocked(Slot, Alias, Pa, AllocBytes, NumberOfBytes);
        KeReleaseSpinLock(&NxContigLock, OldIrql);
        DPRINT("MmAllocContig req=%lu got=%lu KB at PA=%08lx (alias %p)\n",
                (ULONG)NumberOfBytes, (ULONG)(AllocBytes >> 10),
                (ULONG)Pa, Alias);
        return Alias;
    }
    KeReleaseSpinLock(&NxContigLock, OldIrql);

    /*
     * Heap couldn't satisfy.  Request is either too large for the carve-out
     * or constrained to a PA range outside it.  Fall through to NT's
     * MiAllocateContiguousMemory, which walks the PFN free list and can
     * satisfy low-PA requests from loader-descriptor pages we didn't claim.
     */
    {
        PHYSICAL_ADDRESS Low, High, Boundary;
        MEMORY_CACHING_TYPE CacheType;
        PVOID SystemVa;
        ULONG_PTR AlignedPa;
        SIZE_T NtBytes = AllocBytes;

        Low.QuadPart = LowestAcceptableAddress;
        High.QuadPart = (HighestAcceptableAddress < NXK_KSEG0_RAM_SIZE - 1)
                            ? HighestAcceptableAddress : NXK_KSEG0_RAM_SIZE - 1;
        Boundary.QuadPart = 0;
        CacheType = MmCached;       /* TODO: WC/UC */
        if (Alignment > PAGE_SIZE)
            NtBytes += Alignment;

        SystemVa = MmAllocateContiguousMemorySpecifyCache(NtBytes, Low, High,
                                                          Boundary, CacheType);
        DPRINT("NT fallback Mm contig req=%lu (low=%08lx high=%08lx) -> SystemVa=%p\n",
                (ULONG)NtBytes, (ULONG)Low.LowPart, (ULONG)High.LowPart, SystemVa);
        if (SystemVa != NULL)
        {
            Pa = MmGetPhysicalAddress(SystemVa).LowPart;
            AlignedPa = (Alignment > PAGE_SIZE)
                            ? ((Pa + Alignment - 1) & ~(Alignment - 1))
                            : Pa;
            Alias = (PVOID)(NXK_KSEG0_BASE | AlignedPa);
            /* Track the slow-path alloc so NxMmFreeContiguousMemory can find it.
             * We reuse NxContigTable but stash the SystemVa in PaEnd (unused
             * for NT-allocs) so MmFree can call back into NT correctly. */
            KeAcquireSpinLock(&NxContigLock, &OldIrql);
            for (Slot = 0; Slot < NXK_CONTIG_SLOTS; Slot++)
                if (NxContigTable[Slot].Alias == NULL) break;
            if (Slot < NXK_CONTIG_SLOTS)
            {
                NxContigTable[Slot].Alias = Alias;
                NxContigTable[Slot].Pa = 0;     /* 0 == "NT-allocator backing" */
                NxContigTable[Slot].PaEnd = (ULONG_PTR)SystemVa;
                NxContigTable[Slot].Size = NumberOfBytes;
            }
            KeReleaseSpinLock(&NxContigLock, OldIrql);
            DPRINT("MmAllocContig req=%lu NT-fallback PA=%08lx alias=%p "
                    "(constrained: low=%08lx high=%08lx)\n",
                    (ULONG)NumberOfBytes, (ULONG)AlignedPa, Alias,
                    (ULONG)LowestAcceptableAddress,
                    (ULONG)HighestAcceptableAddress);
            return Alias;
        }
    }

    DPRINT1("MmAllocateContiguousMemoryEx(%lu) -- no memory "
            "(low=%08lx high=%08lx align=%lx, %lu heap regions)\n",
            (ULONG)NumberOfBytes, (ULONG)LowestAcceptableAddress,
            (ULONG)HighestAcceptableAddress, (ULONG)Alignment,
            NxHeapRegionCount);
    return NULL;
}
#endif /* NXK_MM_PHYS */

/* Unbounded form: forward to the ex variant with full PA range. */
PVOID
NTAPI
NxMmAllocateContiguousMemory(IN SIZE_T NumberOfBytes)
{
    return NxMmAllocateContiguousMemoryEx(NumberOfBytes, 0, 0xFFFFFFFF, 0,
                                          PAGE_READWRITE);
}

/* The reserved-PA-range allocator has no per-PFN bookkeeping to undo --
 * clearing the table slot is enough.  The next alloc re-scans the gaps. */
VOID
NTAPI
NxMmFreeContiguousMemory(IN PVOID BaseAddress)
{
    SIZE_T Size = 0;
    if (!NxpHeapFree(BaseAddress, &Size))
        DPRINT1("MmFreeContiguousMemory(%p) -- not a contiguous alloc\n",
                BaseAddress);
}

/* The request size of a contiguous allocation, from the tracking table. */
SIZE_T
NTAPI
NxMmQueryAllocationSize(IN PVOID BaseAddress)
{
    KIRQL OldIrql;
    SIZE_T Size = 0;
    ULONG i;

    KeAcquireSpinLock(&NxContigLock, &OldIrql);
    for (i = 0; i < NXK_CONTIG_SLOTS; i++)
    {
        if (NxContigTable[i].Alias == BaseAddress)
        {
            Size = NxContigTable[i].Size;
            break;
        }
    }
    KeReleaseSpinLock(&NxContigLock, OldIrql);
    return Size;
}

/* "Persist" keeps a contiguous allocation alive across a soft reboot so a
 * newly launched title can inherit it.  Bring-up has no reboot handoff, so
 * the request is accepted as a no-op. */
VOID
NTAPI
NxMmPersistContiguousMemory(
    IN PVOID BaseAddress,
    IN SIZE_T NumberOfBytes,
    IN BOOLEAN Persist)
{
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(NumberOfBytes);
    UNREFERENCED_PARAMETER(Persist);
}

/* GPU INSTANCE MEMORY ******************************************************/

/*
 * MmClaimGpuInstanceMemory is *not* a contiguous-memory allocation.  It
 * declares how much of the NV2A's fixed instance-memory window the title
 * keeps reserved for the GPU; everything below that window is heap.
 * Three things make it distinct from MmAllocateContiguousMemory(Ex):
 *
 *  1. The region's PA is fixed by NV2A hardware (the GPU's internal MMU
 *     finds RAMHT/RAMFC/graphics-context records at the top of the title-
 *     visible address space) -- the title can't relocate it, so we pre-
 *     exclude it from the contiguous-memory heap at boot.
 *  2. It can only shrink the GPU-reserved sub-region, not grow it.  The
 *     unused bytes (NV2A_INSTANCE_BYTES - claim) go back to the title's
 *     heap so the title can use them for normal contiguous allocations.
 *  3. The return value is the title's new heap ceiling -- top of memory
 *     minus the hardware-fixed padding gap (PA 0x03FF0000-0x04000000 on
 *     a 64 MB Xbox).  Subsequent contig allocs must not extend above it.
 *
 * Written for a 64 MB layout (NXK_KSEG0_RAM_SIZE = 0x04000000) even on
 * the 128 MB devkit build: titles with the Limit64MB InitFlag
 * explicitly want the retail 64 MB layout, and the heap
 * setup already keeps the title arena below PA 0x03E00000.  When 128 MB
 * support lands the constants here will need to be derived from the
 * actual top of usable RAM.
 *
 * Layout near the top of the 64 MB physical address space:
 *   PA 0x03FE0000..0x03FF0000   NV2A instance memory (claimable, 64 KB)
 *   PA 0x03FF0000..0x04000000   topology padding (fixed, 64 KB)
 */
/* Boot state: nothing claimed -- the instance pages sit in the page
 * supply (retail-probed: exact-window pins there succeed before any
 * claim).  A claim is the reservation: the claimed tail is pulled out
 * of the supply (relocating any current users) so nothing else can
 * land where the title writes GPU state. */
static SIZE_T NxkNv2aInstanceBytes;

PVOID
NTAPI
NxMmClaimGpuInstanceMemory(
    IN SIZE_T NumberOfBytes,
    OUT SIZE_T *NumberOfPaddingBytes)
{
    SIZE_T Claim;

    if (NumberOfBytes == MAXULONG_PTR)
    {
        /* Query mode: report current claim without changing it. */
        Claim = NxkNv2aInstanceBytes;
    }
    else
    {
        Claim = (NumberOfBytes + (PAGE_SIZE - 1)) & ~((SIZE_T)PAGE_SIZE - 1);
        if (Claim > NXK_NV2A_INSTANCE_BYTES)
            Claim = NXK_NV2A_INSTANCE_BYTES;

#ifdef NXK_MM_PHYS
        {
            /* The claim grows down from the padding boundary.  Reserve
             * newly-claimed pages (relocate live users out); return
             * pages a shrinking claim no longer covers. */
            PFN_NUMBER Top = NXK_NV2A_HEAP_CEILING >> PAGE_SHIFT;
            PFN_NUMBER NewLow = Top - (Claim >> PAGE_SHIFT);
            PFN_NUMBER OldLow = Top - (NxkNv2aInstanceBytes >> PAGE_SHIFT);
            KIRQL OldIrql = MiAcquirePfnLock();

            while (NewLow < OldLow)
            {
                OldLow--;
                if (!NxkPageSupplyTakeSpecific(OldLow) &&
                    NxkPageSupplyTakeRunRelocate(1, OldLow, OldLow, 1) == 0)
                {
                    XbDbg("MmClaimGpuInstanceMemory: PFN %05lx not "
                          "reclaimable\n", (ULONG)OldLow);
                }
            }
            while (NewLow > OldLow)
            {
                NxkPageSupplyReturn(OldLow);
                OldLow++;
            }
            MiReleasePfnLock(OldIrql);
        }
#endif
        NxkNv2aInstanceBytes = Claim;
    }

    if (NumberOfPaddingBytes != NULL)
        *NumberOfPaddingBytes = NXK_NV2A_PADDING_BYTES;

    XbDbg("MmClaimGpuInstanceMemory(req=0x%lx) claim=0x%lx pad=0x%lx -> 0x%08lx\n",
          (ULONG)NumberOfBytes, (ULONG)Claim, (ULONG)NXK_NV2A_PADDING_BYTES,
          (ULONG)(NXK_KSEG0_BASE | NXK_NV2A_HEAP_CEILING));

    return (PVOID)(NXK_KSEG0_BASE | NXK_NV2A_HEAP_CEILING);
}

/* EOF */
