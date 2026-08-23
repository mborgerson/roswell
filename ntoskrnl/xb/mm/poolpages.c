/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     nxmm pool pages -- VA-contiguous page runs for nxpool.
 *
 * Replaces the ARM3 pool page layer (MiAllocatePoolPages /
 * MiFreePoolPages) for nxpool's whole-page needs: small-class page
 * refills and big blocks.  A fixed 32 MB VA window is carved from the
 * dead system-cache range and backed page-by-page from the nxmm
 * supply; page tables come from the same supply.  Run lengths live in
 * a side table indexed by window page, so freeing needs no in-band
 * metadata and no MMPFN state -- which is the point: this removes the
 * pool layer's last structural dependency on the PFN database.
 *
 * Freed runs are unmapped and their frames returned to the supply:
 * retail-validated (mm/bigpool pins MmIsAddressValid == FALSE after a
 * whole-page free), so a release-after-free faults here exactly as it
 * would on retail instead of silently reading recycled memory.
 *
 * DISPATCH_LEVEL-safe throughout: one spinlock for the window state,
 * the PFN lock only around supply calls.
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

#include "mm.h"

/* WINDOW *******************************************************************/

/* The whole retired system-cache VA range, up to the system-memory
 * aperture: pool VA can never be the limiting factor (allocations are
 * bounded by physical RAM, and this is several times larger), so the
 * only allocation failure left is genuine supply exhaustion -- the
 * retail semantic.  Metadata lives in boot pages, not the image. */
#define NXPP_BASE        NXK_POOL_WINDOW_BASE
/* Pool can never exceed physical RAM, so a 64 MB VA window suffices;
 * metadata scales with the window and lives in title-visible boot
 * pages -- the old 240 MB window cost ~96 KB of pure overhead. */
#define NXPP_PAGES       NXK_POOL_WINDOW_PAGES    /* 16 MB of VA */
#define NXPP_END         (NXPP_BASE + ((ULONG_PTR)NXPP_PAGES << PAGE_SHIFT))

#define NXPP_PDE(Va)     (((PULONG)0xC0300000)[(ULONG_PTR)(Va) >> 22])
#define NXPP_PTE(Va)     (((PULONG)0xC0000000)[(ULONG_PTR)(Va) >> 12])
#define NXPP_PTE_BITS    0x3UL                   /* present | write */

static PUSHORT NxppRunPages;                     /* run length at start page */
static PULONG NxppUsedMap;                       /* VA allocation bitmap */
#if DBG
/* Use-after-free attribution: remember who freed each window page so a
 * later fault on it names the culprit. */
static PULONG NxppLastFreeRa;
static ULONG NxppPendingFreeRa;                  /* ExFreePool caller */

VOID
NxPoolPagesNoteFreer(
    IN PVOID Ra)
{
    NxppPendingFreeRa = (ULONG)(ULONG_PTR)Ra;
}

ULONG
NxPoolPagesLastFreeRa(
    IN PVOID Address)
{
    ULONG_PTR Va = (ULONG_PTR)Address;
    if (NxppLastFreeRa == NULL || Va < NXPP_BASE || Va >= NXPP_END)
        return 0;
    return NxppLastFreeRa[(Va - NXPP_BASE) >> PAGE_SHIFT];
}
#endif

/* Boot-time metadata, carved by MM init from boot pages (zeroed by the
 * caller's RtlZeroMemory over the whole carve). */
SIZE_T
NxPoolPagesMetadataSize(VOID)
{
    SIZE_T Size = NXPP_PAGES * sizeof(USHORT) +   /* run table */
                  (NXPP_PAGES / 32) * sizeof(ULONG); /* bitmap */
#if DBG
    Size += NXPP_PAGES * sizeof(ULONG);           /* attribution */
#endif
    return Size;
}

VOID
NxPoolPagesInitialize(
    IN PVOID Metadata)
{
    PUCHAR Cursor = Metadata;

    NxppRunPages = (PUSHORT)Cursor;
    Cursor += NXPP_PAGES * sizeof(USHORT);
    NxppUsedMap = (PULONG)Cursor;
    Cursor += (NXPP_PAGES / 32) * sizeof(ULONG);
#if DBG
    NxppLastFreeRa = (PULONG)Cursor;
#endif
}
static ULONG NxppScanHint;
static ULONG NxppCommitted;                      /* pages backing the window */
static KSPIN_LOCK NxppLock;
static BOOLEAN NxppLockInit;

/* HELPERS ******************************************************************/

static BOOLEAN
NxppTestRange(ULONG Start, ULONG Count)
{
    for (ULONG i = Start; i < Start + Count; i++)
    {
        if (NxppUsedMap[i >> 5] & (1UL << (i & 31))) return FALSE;
    }
    return TRUE;
}

static VOID
NxppSetRange(ULONG Start, ULONG Count, BOOLEAN Set)
{
    for (ULONG i = Start; i < Start + Count; i++)
    {
        if (Set) NxppUsedMap[i >> 5] |= (1UL << (i & 31));
        else NxppUsedMap[i >> 5] &= ~(1UL << (i & 31));
    }
}

/* PUBLIC *******************************************************************/

/* Ownership test for callers that route frees by address range (the
 * contiguous-memory free path). */
BOOLEAN
NxPoolPagesOwns(
    IN PVOID BaseAddress)
{
    ULONG_PTR Va = (ULONG_PTR)BaseAddress;
    return Va >= NXPP_BASE && Va < NXPP_END;
}

PVOID
NxPoolPagesAlloc(
    IN SIZE_T NumberOfBytes)
{
    ULONG Count = (ULONG)BYTES_TO_PAGES(NumberOfBytes);
    ULONG Start, i;
    KIRQL OldIrql, PfnIrql;
    ULONG_PTR Va;

    ASSERT(Count != 0);
    if (NxppRunPages == NULL) return NULL;   /* before MM init */

    if (!NxppLockInit)
    {
        /* First call is single-threaded early boot. */
        KeInitializeSpinLock(&NxppLock);
        NxppLockInit = TRUE;
    }

    KeAcquireSpinLock(&NxppLock, &OldIrql);

    /* First fit from the rotating hint, then wrap once. */
    Start = (ULONG)-1;
    for (ULONG Probe = NxppScanHint; Probe + Count <= NXPP_PAGES; Probe++)
    {
        if (NxppTestRange(Probe, Count))
        {
            Start = Probe;
            break;
        }
    }
    if (Start == (ULONG)-1)
    {
        for (ULONG Probe = 0;
             Probe < NxppScanHint && Probe + Count <= NXPP_PAGES; Probe++)
        {
            if (NxppTestRange(Probe, Count))
            {
                Start = Probe;
                break;
            }
        }
    }
    if (Start == (ULONG)-1)
    {
        KeReleaseSpinLock(&NxppLock, OldIrql);
        DPRINT1("pool pages: window full for %lu pages\n", Count);
        return NULL;
    }

    /* Back the run; page tables come from the supply too. */
    Va = NXPP_BASE + ((ULONG_PTR)Start << PAGE_SHIFT);
    PfnIrql = MiAcquirePfnLock();
    for (i = 0; i < Count; i++)
    {
        ULONG_PTR PageVa = Va + ((ULONG_PTR)i << PAGE_SHIFT);
        PFN_NUMBER Pfn;

        if (!(NXPP_PDE(PageVa) & 1))
        {
            PFN_NUMBER PtPfn = NxkPageSupplyTakeZero(0);
            if (PtPfn == 0) goto unwind;
            NXPP_PDE(PageVa) = ((ULONG)PtPfn << PAGE_SHIFT) | NXPP_PTE_BITS;
            NxkPageSupplySetOwner(PtPfn, (ULONG_PTR)&NXPP_PDE(PageVa));
        }

        ASSERT(NXPP_PTE(PageVa) == 0);
        Pfn = NxkPageSupplyTakeAny(0);
        if (Pfn == 0)
        {
unwind:
            /* Roll this allocation back. */
            while (i--)
            {
                PageVa = Va + ((ULONG_PTR)i << PAGE_SHIFT);
                NxkPageSupplySetOwner(NXPP_PTE(PageVa) >> PAGE_SHIFT, 0);
                NxkPageSupplyReturn(NXPP_PTE(PageVa) >> PAGE_SHIFT);
                NXPP_PTE(PageVa) = 0;
                KeInvalidateTlbEntry((PVOID)PageVa);
            }
            MiReleasePfnLock(PfnIrql);
            KeReleaseSpinLock(&NxppLock, OldIrql);
            return NULL;
        }
        NXPP_PTE(PageVa) = ((ULONG)Pfn << PAGE_SHIFT) | NXPP_PTE_BITS;
        /* Pool pages are CPU-accessed through the window VA only, so
         * the contiguous allocator may relocate the frame -- the
         * retail-probed behavior for its system-VA-mapped pool.
         * Exceptions opt out via NxPoolPagesSetMovable. */
        NxkPageSupplySetOwner(Pfn, (ULONG_PTR)&NXPP_PTE(PageVa));
    }
    MiReleasePfnLock(PfnIrql);

    NxppSetRange(Start, Count, TRUE);
    NxppRunPages[Start] = (USHORT)Count;
    NxppCommitted += Count;
    NxppScanHint = Start + Count;
    if (NxppScanHint >= NXPP_PAGES) NxppScanHint = 0;

    KeReleaseSpinLock(&NxppLock, OldIrql);
    return (PVOID)Va;
}

ULONG
NxPoolPagesFree(
    IN PVOID BaseAddress)
{
    ULONG_PTR Va = (ULONG_PTR)BaseAddress;
    ULONG Start, Count;
    KIRQL OldIrql;

    ASSERT(Va >= NXPP_BASE && Va < NXPP_END);
    ASSERT((Va & (PAGE_SIZE - 1)) == 0);

    Start = (ULONG)((Va - NXPP_BASE) >> PAGE_SHIFT);

    KeAcquireSpinLock(&NxppLock, &OldIrql);
    Count = NxppRunPages[Start];
    ASSERT(Count != 0);
    NxppRunPages[Start] = 0;
    {
        KIRQL PfnIrql = MiAcquirePfnLock();
        for (ULONG i = 0; i < Count; i++)
        {
            ULONG_PTR PageVa = Va + ((ULONG_PTR)i << PAGE_SHIFT);
#if DBG
            NxppLastFreeRa[Start + i] = NxppPendingFreeRa;
#endif
            NxkPageSupplySetOwner(NXPP_PTE(PageVa) >> PAGE_SHIFT, 0);
            NxkPageSupplyReturn(NXPP_PTE(PageVa) >> PAGE_SHIFT);
            NXPP_PTE(PageVa) = 0;
            KeInvalidateTlbEntry((PVOID)PageVa);
        }
        MiReleasePfnLock(PfnIrql);
    }
    NxppSetRange(Start, Count, FALSE);
    NxppCommitted -= Count;
    KeReleaseSpinLock(&NxppLock, OldIrql);
    return Count;
}

/* Pages currently backing the window -- the raw input to the
 * PoolPagesCommitted statistic. */
ULONG
NxPoolPagesCommitted(VOID)
{
    return NxppCommitted;
}

/*
 * Make a window-mapped range immovable (owner cleared) or restore its
 * relocatability (owner recomputed from the window PTE).  Two users:
 * kernel-stack pages (a thread cannot survive its own stack frames
 * being copied out from under the relocation call chain) and buffers
 * pinned for DMA, where the device holds the physical address.
 */
VOID
NxPoolPagesSetMovable(
    IN PVOID BaseAddress,
    IN SIZE_T NumberOfBytes,
    IN BOOLEAN Movable)
{
    ULONG_PTR Va = (ULONG_PTR)BaseAddress & ~((ULONG_PTR)PAGE_SIZE - 1);
    ULONG_PTR End = (ULONG_PTR)BaseAddress + NumberOfBytes;
    KIRQL OldIrql;

    if (Va < NXPP_BASE || Va >= NXPP_END) return;

    OldIrql = MiAcquirePfnLock();
    for (; Va < End; Va += PAGE_SIZE)
    {
        ULONG Pte = NXPP_PTE(Va);
        if (Pte & 0x1)
        {
            NxkPageSupplySetOwner(Pte >> PAGE_SHIFT,
                                  Movable ? (ULONG_PTR)&NXPP_PTE(Va) : 0);
        }
    }
    MiReleasePfnLock(OldIrql);
}
