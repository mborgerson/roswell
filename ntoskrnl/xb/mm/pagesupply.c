/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NxkPageSupply -- the kernel's physical page database.
 *
 * Every physical-page allocate/free routes through this interface: single
 * pages (pool pages, VM commits, kernel stacks, page tables) and
 * contiguous runs (the Xbox MmAllocateContiguousMemory* ordinals, via
 * xb/mm/contig.c).  Like the retail kernel, there is ONE pool of free
 * pages -- contiguous and single-page requests compete for the same RAM.
 *
 * Contract, inherited from the routines underneath: callers hold the PFN
 * lock; Take* returns 0 when nothing is available; pages passed to
 * Return must have no remaining references.
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include <mm/ARM3/miarm.h>

#include "mm.h"

/* PAGE SUPPLY **************************************************************/

#ifdef NXK_MM_PHYS

/*
 * One ULONG entry per page, boot-allocated at exactly (highest PFN + 1)
 * entries -- like retail, the per-page array lives in boot pages, not
 * the kernel image.  Free pages form a doubly-linked list threaded
 * through the entries (15-bit prev/next indices cover PFNs through
 * 128 MB; 0 terminates -- PFN 0 is never supplied), so targeted unlink
 * for run allocation is O(1) per page.  An in-use page's entry is 0.
 *
 * Free pages file onto one of two lists by PA class -- a pure function
 * of the PFN, so unlink needs no extra state.  Kernel-transient pages
 * (pool, VM commits, page tables, stacks) pop the kernel-preferred
 * list first and only spill into title-window RAM when it runs dry,
 * which is the drain order retail exhibits: a title's contiguous pin
 * window stays free across arbitrary kernel churn.  On the retail
 * split layout the kernel-preferred class is the high zone's NT carve;
 * on the 128 MB devkit it is the upper 64 MB, which has no
 * title-visible alias at all.
 *
 * Spill takes are lowest-PA-first (an ascending scan cursor, lowered
 * when a lower page is returned) -- measured retail behavior: single
 * VM-commit and pool pages land in ascending low PAs just above the
 * pool floor, while contiguous runs come from the top, so the FB zone
 * under the GPU reserve stays unfragmented for the title's large
 * surface allocations.
 *
 * Pages are zeroed on demand (MiZeroPhysicalPage maps through
 * hyperspace), so there is no zeroed pool and no zero thread to feed.
 *
 * The MMPFN database still exists for the residual ARM3 bookkeeping
 * (pool page init, kernel stacks, MDL probes); the supply neither
 * reads nor writes it.  Free pages keep whatever MMPFN state their
 * last owner left -- deliberately never PageLocation==Free, so the
 * ARM3 contiguous-scan fallback finds nothing instead of walking
 * never-initialized list links.
 */

#define NXP_FREE        0x80000000UL
#define NXP_PFN_MASK    0x00007FFFUL
#define NXP_PREV_SHIFT  15

/* One ULONG per frame, meaning chosen by state: free frames carry the
 * doubly-linked free-list entry (NXP_FREE | prev | next); in-use frames
 * carry the owning PTE's self-map index ((PteVa - 0xC0000000) >> 2,
 * 20 bits, 0 = unowned).  The states are disjoint, so links and owners
 * share the array -- the owner form never has NXP_FREE set because all
 * owner PTE VAs sit in [0xC0000000, 0xC0400000). */
#define NXP_OWNER_BASE  0xC0000000UL
#define NXP_OWNER_LIMIT 0xC0400000UL

/* Pages below 64 KiB are pin-only (retail-probed: at full exhaustion
 * retail still satisfies exact-window pins at 0x1000-0xE000 and its
 * general allocator never reaches below PA 0xB2000): generic single
 * and run scans skip them; only windows entirely below the floor --
 * explicit pins -- may take them.  PA 0 is GPU-written and never
 * supplied at all. */
#define NXP_GENERIC_FLOOR_PFN  0x10

static PULONG NxpLinks;
static PFN_NUMBER NxpHighestPfn;
static ULONG NxpFreeHeadKernel;         /* 0 = empty */
static ULONG NxpFreeHeadTitle;
static PFN_NUMBER NxpKernelLowPfn;      /* kernel-preferred class bounds */
static PFN_NUMBER NxpKernelHighPfn;
static PFN_NUMBER NxpSpillHint;         /* <= lowest free title page */
/* Free pages at/above the generic floor -- i.e. usable for generic allocation
 * and page relocation.  Free pages below the floor are pin-only (titles pin
 * specific low PAs) and TakeAny/TakeAnyOutside never hand them out, so the
 * contig pre-flight must use this, not MmAvailablePages (which counts them),
 * or it over-commits to windows it can't rehome and leaks pages partway. */
static PFN_NUMBER NxpGenericAvail;

#define NXP_NEXT(e) ((e) & NXP_PFN_MASK)
#define NXP_PREV(e) (((e) >> NXP_PREV_SHIFT) & NXP_PFN_MASK)

static
PULONG
NxpHeadFor(PFN_NUMBER Page)
{
    return (Page >= NxpKernelLowPfn && Page <= NxpKernelHighPfn)
               ? &NxpFreeHeadKernel : &NxpFreeHeadTitle;
}

static
VOID
NxpPushHead(PFN_NUMBER Page)
{
    PULONG HeadPtr = NxpHeadFor(Page);
    ULONG Head = *HeadPtr;

    NxpLinks[Page] = NXP_FREE | Head;   /* prev = 0 (head) */
    if (Head != 0)
    {
        NxpLinks[Head] = NXP_FREE | ((ULONG)Page << NXP_PREV_SHIFT) |
                         NXP_NEXT(NxpLinks[Head]);
    }
    *HeadPtr = (ULONG)Page;
    MmAvailablePages++;
    if (Page >= NXP_GENERIC_FLOOR_PFN) NxpGenericAvail++;
}

static
VOID
NxpUnlink(PFN_NUMBER Page)
{
    ULONG Entry = NxpLinks[Page];
    ULONG Prev = NXP_PREV(Entry);
    ULONG Next = NXP_NEXT(Entry);

    if (Prev != 0)
        NxpLinks[Prev] = NXP_FREE | (NXP_PREV(NxpLinks[Prev]) << NXP_PREV_SHIFT) | Next;
    else
        *NxpHeadFor(Page) = Next;
    if (Next != 0)
        NxpLinks[Next] = NXP_FREE | (Prev << NXP_PREV_SHIFT) | NXP_NEXT(NxpLinks[Next]);
    NxpLinks[Page] = 0;
    MmAvailablePages--;
    if (Page >= NXP_GENERIC_FLOOR_PFN) NxpGenericAvail--;
}

/* Called from MM init before the boot population, with an array sized
 * and zeroed by the caller from boot pages. */
VOID
NxkPageSupplyInitialize(
    IN PFN_NUMBER HighestPfn,
    IN PVOID LinksArray)
{
    ASSERT(HighestPfn <= NXP_PFN_MASK);
    NxpLinks = LinksArray;
    NxpHighestPfn = HighestPfn;
    RtlZeroMemory(LinksArray, (HighestPfn + 1) * sizeof(ULONG));

    if (NxkMmRetailShapedMap())
    {
        NxpKernelLowPfn = NXK_HIGH_ZONE_BASE >> PAGE_SHIFT;
        NxpKernelHighPfn = (NXK_HIGH_ZONE_LIMIT >> PAGE_SHIFT) - 1;
    }
    else
    {
        NxpKernelLowPfn = NXK_KSEG0_RAM_SIZE >> PAGE_SHIFT;
        NxpKernelHighPfn = HighestPfn;
    }
    NxpSpillHint = 1;
}

PFN_NUMBER
NTAPI
NxkPageSupplyTakeAny(ULONG Color)
{
    PFN_NUMBER Pfn;

    UNREFERENCED_PARAMETER(Color);
    MI_ASSERT_PFN_LOCK_HELD();

    ASSERT(NxpLinks != NULL);
    Pfn = NxpFreeHeadKernel;
    if (Pfn == 0)
    {
        /* Kernel class dry: spill to the lowest free page (measured
         * retail order).  Only title pages can be free here, so the
         * scan needs no class check. */
        if (NxpFreeHeadTitle == 0) return 0;
        Pfn = NxpSpillHint;
        if (Pfn < NXP_GENERIC_FLOOR_PFN) Pfn = NXP_GENERIC_FLOOR_PFN;
        while (!((NxpLinks[Pfn] & NXP_FREE)))
        {
            /* Only pin-only low pages may remain free. */
            if (Pfn >= NxpHighestPfn) return 0;
            Pfn++;
        }
        NxpSpillHint = Pfn;
    }
    NxpUnlink(Pfn);
    return Pfn;
}

PFN_NUMBER
NTAPI
NxkPageSupplyTakeZero(ULONG Color)
{
    PFN_NUMBER Pfn = NxkPageSupplyTakeAny(Color);
    if (Pfn != 0) MiZeroPhysicalPage(Pfn);
    return Pfn;
}

/* No ready-zeroed pool exists; callers fall back to TakeAny plus their
 * own zeroing, which is the contract. */
PFN_NUMBER
NTAPI
NxkPageSupplyTakeZeroIfReady(ULONG Color)
{
    UNREFERENCED_PARAMETER(Color);
    return 0;
}

VOID
NTAPI
NxkPageSupplyReturn(PFN_NUMBER Page)
{
    MI_ASSERT_PFN_LOCK_HELD();
    ASSERT(NxpLinks != NULL);
    ASSERT(Page > 0 && Page <= NxpHighestPfn);
    /* A free entry here is a double free; a stale owner index is fine
     * (PushHead overwrites it). */
    ASSERT(!(NxpLinks[Page] & NXP_FREE));

    NxpPushHead(Page);
    if (Page < NxpSpillHint) NxpSpillHint = Page;
}

/*
 * Frame ownership: the PTE (self-map VA) that maps a relocatable frame.
 * Maintained by nxvm on map/unmap; consumed by the contiguous
 * allocator's relocation pass.  Frames with no owner (kernel pool,
 * contiguous runs, page tables) are immovable.
 */
VOID
NxkPageSupplySetOwner(
    IN PFN_NUMBER Page,
    IN ULONG_PTR PteVa)
{
    if (NxpLinks != NULL && Page > 0 && Page <= NxpHighestPfn &&
        !(NxpLinks[Page] & NXP_FREE))
    {
        ASSERT(PteVa == 0 ||
               (PteVa >= NXP_OWNER_BASE && PteVa < NXP_OWNER_LIMIT));
        NxpLinks[Page] = PteVa ? (ULONG)((PteVa - NXP_OWNER_BASE) >> 2) : 0;
    }
}

ULONG_PTR
NxkPageSupplyGetOwner(
    IN PFN_NUMBER Page)
{
    ULONG Entry;
    if (NxpLinks == NULL || Page == 0 || Page > NxpHighestPfn)
        return 0;
    Entry = NxpLinks[Page];
    if ((Entry & NXP_FREE) || Entry == 0)
        return 0;
    return NXP_OWNER_BASE + ((ULONG_PTR)Entry << 2);
}

/* Take any free page strictly outside [LowPfn, HighPfn] -- relocation
 * destinations must not land inside the window being assembled. */
PFN_NUMBER
NxkPageSupplyTakeAnyOutside(
    IN PFN_NUMBER LowPfn,
    IN PFN_NUMBER HighPfn)
{
    PFN_NUMBER Pfn;

    MI_ASSERT_PFN_LOCK_HELD();

    Pfn = NxpFreeHeadKernel;
    if (Pfn != 0 && !(Pfn >= LowPfn && Pfn <= HighPfn))
    {
        NxpUnlink(Pfn);
        return Pfn;
    }
    for (Pfn = NxpSpillHint; Pfn <= NxpHighestPfn; Pfn++)
    {
        if (!(NxpLinks[Pfn] & NXP_FREE)) continue;
        if (Pfn >= LowPfn && Pfn <= HighPfn) continue;
        NxpUnlink(Pfn);
        return Pfn;
    }
    /* The kernel class may still have pages inside the window's shadow;
     * walk it as a last resort. */
    for (Pfn = NxpFreeHeadKernel; Pfn != 0; Pfn = NXP_NEXT(NxpLinks[Pfn]))
    {
        if (Pfn >= LowPfn && Pfn <= HighPfn) continue;
        NxpUnlink(Pfn);
        return Pfn;
    }
    /* Generic title pages below the spill hint are invisible to the upward
     * scan (the hint only tracks generic allocation; pin-only and early-freed
     * low pages sit beneath it).  Walk the title free list so every
     * generic-allocatable free page is reachable for relocation -- skipping
     * pin-only frames below the floor (reserved for explicit low pins).  This
     * keeps the reachable set in step with NxpGenericAvail, which the contig
     * pre-flight commits against. */
    for (Pfn = NxpFreeHeadTitle; Pfn != 0; Pfn = NXP_NEXT(NxpLinks[Pfn]))
    {
        if (Pfn < NXP_GENERIC_FLOOR_PFN) continue;
        if (Pfn >= LowPfn && Pfn <= HighPfn) continue;
        NxpUnlink(Pfn);
        return Pfn;
    }
    return 0;
}

/* The contiguous paths' view: free-membership test and targeted take. */
BOOLEAN
NxkPageSupplyIsFree(PFN_NUMBER Page)
{
    if (NxpLinks == NULL || Page == 0 || Page > NxpHighestPfn)
        return FALSE;
    return (NxpLinks[Page] & NXP_FREE) != 0;
}

BOOLEAN
NxkPageSupplyTakeSpecific(PFN_NUMBER Page)
{
    MI_ASSERT_PFN_LOCK_HELD();
    if (!NxkPageSupplyIsFree(Page)) return FALSE;
    NxpUnlink(Page);
    return TRUE;
}

/*
 * Take a contiguous run: RunPages pages, base aligned to AlignPages,
 * entirely within [LowPfn, HighPfn].  Scans top-down so the highest
 * free run wins -- the placement the retail allocator exhibits (FB
 * allocations land just below the GPU reserve; low PAs stay open for
 * HighestAcceptableAddress-constrained requests).  Returns the base
 * PFN, or 0 if no run fits.
 */
PFN_NUMBER
NxkPageSupplyTakeRun(
    IN PFN_NUMBER RunPages,
    IN PFN_NUMBER LowPfn,
    IN PFN_NUMBER HighPfn,
    IN PFN_NUMBER AlignPages)
{
    PFN_NUMBER Base, i;

    MI_ASSERT_PFN_LOCK_HELD();
    ASSERT(NxpLinks != NULL);

    if (RunPages == 0) return 0;
    if (AlignPages == 0) AlignPages = 1;
    if (HighPfn > NxpHighestPfn) HighPfn = NxpHighestPfn;
    if (LowPfn == 0) LowPfn = 1;
    if (HighPfn < LowPfn || HighPfn - LowPfn + 1 < RunPages) return 0;

    Base = (HighPfn - RunPages + 1) & ~(AlignPages - 1);
    while (Base >= LowPfn)
    {
        PFN_NUMBER Failed = 0;
        BOOLEAN Fits = TRUE;

        /* Check high-to-low so a miss tells us how far to skip. */
        for (i = RunPages; i-- > 0;)
        {
            if (!NxkPageSupplyIsFree(Base + i))
            {
                Failed = Base + i;
                Fits = FALSE;
                break;
            }
        }
        if (Fits)
        {
            for (i = 0; i < RunPages; i++)
                NxpUnlink(Base + i);
            return Base;
        }

        /* Next candidate: highest aligned base whose run ends below the
         * page that failed. */
        if (Failed < RunPages) break;
        Base = (Failed - RunPages) & ~(AlignPages - 1);
    }
    return 0;
}

/*
 * Run-take with relocation, the retail-probed semantic: an exact-window
 * contiguous request succeeds even when live VM pages sit inside the
 * window -- the kernel copies each one to a fresh frame outside the
 * window and retargets its owning PTE.  Only ownerless frames (pool,
 * page tables, other contiguous runs, kernel) are immovable.
 *
 * Runs at the PFN lock's IRQL on the one CPU, so no thread can touch a
 * page mid-copy; interrupt handlers never write title VM.
 */
PFN_NUMBER
NxkPageSupplyTakeRunRelocate(
    IN PFN_NUMBER RunPages,
    IN PFN_NUMBER LowPfn,
    IN PFN_NUMBER HighPfn,
    IN PFN_NUMBER AlignPages)
{
    PFN_NUMBER Base, i;

    MI_ASSERT_PFN_LOCK_HELD();
    ASSERT(NxpLinks != NULL);

    if (RunPages == 0) return 0;
    if (AlignPages == 0) AlignPages = 1;
    if (HighPfn > NxpHighestPfn) HighPfn = NxpHighestPfn;
    if (LowPfn == 0) LowPfn = 1;
    if (HighPfn < LowPfn || HighPfn - LowPfn + 1 < RunPages) return 0;

    Base = (HighPfn - RunPages + 1) & ~(AlignPages - 1);
    while (Base >= LowPfn)
    {
        PFN_NUMBER Failed = 0, FreeInWindow = 0, Movable = 0;
        BOOLEAN Fits = TRUE;

        for (i = RunPages; i-- > 0;)
        {
            PFN_NUMBER Page = Base + i;

            if (NxpLinks[Page] & NXP_FREE)
                FreeInWindow++;
            else if (NxpLinks[Page] != 0)
                Movable++;
            else
            {
                Failed = Page;
                Fits = FALSE;
                break;
            }
        }

        /* Enough free frames outside the window to rehome the movable
         * pages?  (Exact under the PFN lock.)  If not, a window lower
         * down may need fewer relocations -- step down one alignment
         * unit and keep scanning. */
        if (Fits && NxpGenericAvail - FreeInWindow < Movable)
        {
            Fits = FALSE;
            if (Base < AlignPages) break;
            Failed = Base - AlignPages + RunPages;
        }

        if (Fits)
        {
            for (i = 0; i < RunPages; i++)
            {
                PFN_NUMBER Page = Base + i;

                if (NxpLinks[Page] & NXP_FREE)
                {
                    NxpUnlink(Page);
                    continue;
                }

                {
                    ULONG_PTR PteVa = NXP_OWNER_BASE +
                                      ((ULONG_PTR)NxpLinks[Page] << 2);
                    PULONG Pte = (PULONG)PteVa;
                    PFN_NUMBER Dest;
                    ULONG Entry;

                    ASSERT(PteVa != 0);
                    Dest = NxkPageSupplyTakeAnyOutside(Base,
                                                       Base + RunPages - 1);
                    ASSERT(Dest != 0);  /* pre-flight counted */
                    if (Dest == 0) return 0;

                    /*
                     * Copy + retarget + flush with NO stack writes in
                     * between, in one asm sequence at HIGH_LEVEL.  This
                     * makes even the calling thread's own stack pages
                     * movable: the prologue spills land before the
                     * copy; rep movsd writes nothing to the stack; and
                     * the PTE swap plus TLB invalidate complete before
                     * the compiler can spill again.  ISRs are blocked
                     * (HIGH_LEVEL), so nothing else writes the source.
                     */
                    {
                        KIRQL HighIrql;
                        ULONG NewEntry;

                        KeRaiseIrql(HIGH_LEVEL, &HighIrql);
                        Entry = *Pte;
                        NewEntry = ((ULONG)Dest << PAGE_SHIFT) |
                                   (Entry & (PAGE_SIZE - 1));
                        /* rep movsl rewrites ESI/EDI/ECX: they must be
                         * declared as outputs, or the optimizer assumes
                         * the registers still hold their inputs after
                         * the asm and reuses them -- silent corruption
                         * in optimized builds. */
                        if (PteVa >= 0xC0300000UL && PteVa < 0xC0301000UL)
                        {
                            /* A whole page table moved: flush every
                             * translation under it (CR3 reload). */
                            ULONG S0, D0, C0;
                            ULONG A0 = NewEntry;
                            __asm__ __volatile__(
                                "cld\n\t"
                                "rep movsl\n\t"
                                "movl %%eax, (%%edx)\n\t"
                                "movl %%cr3, %%eax\n\t"
                                "movl %%eax, %%cr3"
                                : "=S"(S0), "=D"(D0), "=c"(C0), "+a"(A0)
                                : "0"(0x80000000UL |
                                      ((ULONG_PTR)Page << PAGE_SHIFT)),
                                  "1"(0x80000000UL |
                                      ((ULONG_PTR)Dest << PAGE_SHIFT)),
                                  "2"(PAGE_SIZE / 4),
                                  "d"(Pte)
                                : "memory", "cc");
                        }
                        else
                        {
                            ULONG_PTR Va =
                                (PteVa - 0xC0000000UL) << (PAGE_SHIFT - 2);
                            ULONG S0, D0, C0;
                            __asm__ __volatile__(
                                "cld\n\t"
                                "rep movsl\n\t"
                                "movl %%eax, (%%edx)\n\t"
                                "invlpg (%%ebx)"
                                : "=S"(S0), "=D"(D0), "=c"(C0)
                                : "0"(0x80000000UL |
                                      ((ULONG_PTR)Page << PAGE_SHIFT)),
                                  "1"(0x80000000UL |
                                      ((ULONG_PTR)Dest << PAGE_SHIFT)),
                                  "2"(PAGE_SIZE / 4),
                                  "a"(NewEntry), "d"(Pte), "b"(Va)
                                : "memory", "cc");
                        }
                        KeLowerIrql(HighIrql);
                    }
                    NxpLinks[Dest] = (ULONG)((PteVa - NXP_OWNER_BASE) >> 2);
                    NxpLinks[Page] = 0;
                    /* The old frame is ours now: it was in use (not on
                     * a free list), so no unlink is needed. */
                }
            }
            return Base;
        }

        if (Failed < RunPages) break;
        Base = (Failed - RunPages) & ~(AlignPages - 1);
    }
    DPRINT1("no candidate for %lu pages in [%05lx,%05lx] "
            "(avail=%lu)\n",
            (ULONG)RunPages, (ULONG)LowPfn, (ULONG)HighPfn,
            (ULONG)MmAvailablePages);
    return 0;
}



#else /* !NXK_MM_PHYS: pass-through to the ARM3 colored lists. */

/* Take a page of any content; prefers pages that don't need zeroing. */
PFN_NUMBER
NTAPI
NxkPageSupplyTakeAny(ULONG Color)
{
    return MiRemoveAnyPage(Color);
}

/* Take a zero-filled page, zeroing one inline if none is ready. */
PFN_NUMBER
NTAPI
NxkPageSupplyTakeZero(ULONG Color)
{
    return MiRemoveZeroPage(Color);
}

/* Take a ready-zeroed page only if one is immediately available.  On 0
 * the caller falls back to TakeAny plus its own zeroing -- some contexts
 * must not run the inline zeroing path under the PFN lock. */
PFN_NUMBER
NTAPI
NxkPageSupplyTakeZeroIfReady(ULONG Color)
{
    if (MmFreePagesByColor[ZeroedPageList][Color].Flink != LIST_HEAD)
    {
        return MiRemoveZeroPage(Color);
    }
    return 0;
}

/* Return a page to the supply. */
VOID
NTAPI
NxkPageSupplyReturn(PFN_NUMBER Page)
{
    MiInsertPageInFreeList(Page);
}

#endif /* NXK_MM_PHYS */

/* Pages immediately takeable. */
PFN_NUMBER
NTAPI
NxkPageSupplyAvailable(VOID)
{
    return MmAvailablePages;
}
