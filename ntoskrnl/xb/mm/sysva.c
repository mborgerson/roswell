/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     System-memory aperture (0xD0000000+).
 *
 * Retail Xbox MmAllocateSystemMemory hands out VAs in the documented
 * system-VA window 0xD0000000-0xEFFFFFFF.  We demand-map that window
 * directly from the page supply: each allocation takes fresh frames and
 * paints them into the aperture, and the PTEs are the only occupancy
 * state we keep -- free walks them and hands the frames back.  (Earlier
 * this layer round-tripped through NT's NP pool and aliased the PAs in,
 * tracked by a 256-slot table; the supply makes all of that vestigial.)
 *
 * PDIs 0x340..0x343 are nominally claimed by the unused system-cache VA
 * range (MmInitializeSystemCache is commented out in ARM3/mminit.c) --
 * empty at boot, free to overlay with our own demand-mapped PT pages.
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>
#include <xb-debug.h>

#include "mm.h"

/* APERTURE STATE ***********************************************************/

static KSPIN_LOCK NxSysVaLock;
static LONG       NxSysVaReady = 0;

/* Page supply (xb/mm/pagesupply.c).  Both require the PFN lock. */
extern PFN_NUMBER NTAPI NxkPageSupplyTakeZero(ULONG Color);
extern VOID NTAPI NxkPageSupplyReturn(PFN_NUMBER Page);

/* INIT *********************************************************************/

VOID
NxkMmInitSystemVa(VOID)
{
    if (InterlockedCompareExchange(&NxSysVaReady, 1, 0) != 0)
        return;

    KeInitializeSpinLock(&NxSysVaLock);

    /* Page tables and backing frames are taken from the page supply
     * lazily in NxkSysVaAllocate (one PT per 4 MB stride on first use):
     * most titles never call MmAllocateSystemMemory, and an eager paint
     * would cost NXK_SYSMEM_PT_COUNT pages up front. */
    DPRINT1("system-VA aperture at 0x%08lx-0x%08lx (demand-mapped)\n",
            (ULONG)NXK_SYSMEM_BASE, (ULONG)(NXK_SYSMEM_LIMIT - 1));
}

/* VA ALLOCATION ************************************************************/

/* First-fit search for a hole of `Pages` consecutive 4 KB VAs in the
 * aperture, using the PTEs as occupancy state: an unbacked PT (PDE not
 * present) is an all-free 4 MB block, a zero PTE is a free page.  Returns
 * the base VA or 0.  Caller holds NxSysVaLock. */
static
ULONG_PTR
NxpSysVaFindHoleLocked(ULONG Pages)
{
    volatile ULONG *Pde = (volatile ULONG *)NXK_PDE_BASE;
    volatile ULONG *Pte = (volatile ULONG *)NXK_PTE_BASE;
    ULONG_PTR Va = NXK_SYSMEM_BASE;
    ULONG_PTR RunStart = 0;
    ULONG Run = 0;

    while (Va < NXK_SYSMEM_LIMIT)
    {
        BOOLEAN Free;

        if (!(Pde[Va >> 22] & 1))
            Free = TRUE;                       /* whole 4 MB unbacked */
        else
            Free = (Pte[Va >> PAGE_SHIFT] == 0);

        if (Free)
        {
            if (Run == 0) RunStart = Va;
            if (++Run == Pages) return RunStart;
        }
        else
        {
            Run = 0;
        }
        Va += PAGE_SIZE;
    }
    return 0;
}

/*
 * Demand-map `Size` bytes into the 0xD0000000+ aperture.  Backing frames
 * (and any covering page tables) come straight from the page supply.
 * Returns NULL if the aperture is full or the supply is exhausted.
 */
PVOID
NxkSysVaAllocate(SIZE_T Size, ULONG Protect)
{
    volatile ULONG *Pde = (volatile ULONG *)NXK_PDE_BASE;
    volatile ULONG *Pte = (volatile ULONG *)NXK_PTE_BASE;
    KIRQL OldIrql, PfnIrql;
    ULONG_PTR Va;
    ULONG Pages, i;

    UNREFERENCED_PARAMETER(Protect);
    if (Size == 0) return NULL;

    Pages = (ULONG)((Size + PAGE_SIZE - 1) >> PAGE_SHIFT);

    KeAcquireSpinLock(&NxSysVaLock, &OldIrql);

    Va = NxpSysVaFindHoleLocked(Pages);
    if (Va == 0)
    {
        KeReleaseSpinLock(&NxSysVaLock, OldIrql);
        return NULL;
    }

    /* Back the covering page tables on first use (one per 4 MB).  A
     * supply page comes zeroed, so the empty PT is ready as-is. */
    for (i = (ULONG)(Va >> 22);
         i <= (ULONG)((Va + ((ULONG_PTR)Pages << PAGE_SHIFT) - 1) >> 22);
         i++)
    {
        PFN_NUMBER PtPfn;

        if (Pde[i] & 1) continue;

        PfnIrql = MiAcquirePfnLock();
        PtPfn = NxkPageSupplyTakeZero(0);
        MiReleasePfnLock(PfnIrql);
        if (PtPfn == 0)
        {
            KeReleaseSpinLock(&NxSysVaLock, OldIrql);
            return NULL;
        }
        Pde[i] = ((ULONG)PtPfn << PAGE_SHIFT) | NXK_PDE_PT;
    }

    /* Commit the data pages.  On supply exhaustion, roll back this
     * call's pages (the empty PTs are harmless -- reused next time). */
    for (i = 0; i < Pages; i++)
    {
        PFN_NUMBER Pfn;

        PfnIrql = MiAcquirePfnLock();
        Pfn = NxkPageSupplyTakeZero(0);
        MiReleasePfnLock(PfnIrql);
        if (Pfn == 0)
        {
            ULONG j;
            PfnIrql = MiAcquirePfnLock();
            for (j = 0; j < i; j++)
            {
                ULONG Idx = (ULONG)(Va >> PAGE_SHIFT) + j;
                NxkPageSupplyReturn(Pte[Idx] >> PAGE_SHIFT);
                Pte[Idx] = 0;
            }
            MiReleasePfnLock(PfnIrql);
            __writecr3(__readcr3());
            KeReleaseSpinLock(&NxSysVaLock, OldIrql);
            return NULL;
        }
        Pte[(Va >> PAGE_SHIFT) + i] =
            ((ULONG)Pfn << PAGE_SHIFT) | NXK_PTE_VALID;
    }
    __writecr3(__readcr3());

    KeReleaseSpinLock(&NxSysVaLock, OldIrql);
    return (PVOID)Va;
}

/*
 * Free a range previously returned by NxkSysVaAllocate.  Returns the
 * number of pages freed.  The caller passes the original size (retail's
 * MmFreeSystemMemory contract), so the PTEs plus Size are all we need --
 * no per-allocation bookkeeping.
 */
ULONG
NxkSysVaFree(PVOID VaPtr, SIZE_T Size)
{
    volatile ULONG *Pte = (volatile ULONG *)NXK_PTE_BASE;
    ULONG_PTR Va = (ULONG_PTR)VaPtr;
    KIRQL OldIrql, PfnIrql;
    ULONG Pages, i, Freed = 0;

    if (VaPtr == NULL) return 0;
    if (Va < NXK_SYSMEM_BASE || Va >= NXK_SYSMEM_LIMIT) return 0;

    Pages = (ULONG)((Size + PAGE_SIZE - 1) >> PAGE_SHIFT);

    KeAcquireSpinLock(&NxSysVaLock, &OldIrql);
    for (i = 0; i < Pages; i++)
    {
        ULONG Idx = (ULONG)(Va >> PAGE_SHIFT) + i;
        ULONG PteVal = Pte[Idx];

        if (!(PteVal & 1)) continue;           /* already free */

        Pte[Idx] = 0;
        PfnIrql = MiAcquirePfnLock();
        NxkPageSupplyReturn(PteVal >> PAGE_SHIFT);
        MiReleasePfnLock(PfnIrql);
        Freed++;
    }
    __writecr3(__readcr3());
    KeReleaseSpinLock(&NxSysVaLock, OldIrql);

    return Freed;
}

/* EOF */
