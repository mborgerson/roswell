/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     nxvm -- the title virtual-memory manager.
 *
 * Owns the title address range (64 KiB .. 2 GiB - 128 KiB) behind the
 * Xbox-shaped NtAllocateVirtualMemory / NtFreeVirtualMemory /
 * NtQueryVirtualMemory ordinals plus the address-protect pair.  A
 * sorted region list records reservations; committed state and
 * per-page protection live directly in the page tables (one address
 * space, no VADs, no working sets).
 *
 * Commit is EAGER, matching retail: pages come off the page supply at
 * the commit call, so commit charge is real, exhaustion surfaces as a
 * clean STATUS_NO_MEMORY from the failing call (with that call's
 * partial work rolled back), and a committed page can never fault.
 * Re-commit over committed pages only updates protection -- contents
 * survive (retail-pinned).  PAGE_NOACCESS commits keep the frame in a
 * not-present software PTE.
 *
 * Page-table pages come from the same supply and are kept once
 * allocated (a PT covers 4 MB; the title range needs at most ~512).
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#include <mm/ARM3/miarm.h>

#include "mm.h"

/* CONSTANTS ****************************************************************/

/* Xbox-only allocation flag (not in the NT headers): the committed
 * pages' contents are unspecified, and retail skips the zero fill. */
#define MEM_NOZERO       0x00800000UL

#define NXV_LOWEST       0x00010000UL
#define NXV_HIGHEST      0x7FFE0000UL   /* top 128 KB stays unmanaged */
#define NXV_GRANULARITY  0x00010000UL   /* 64 KiB reserve alignment */

/* i386 PTE bits. */
#define NXV_PTE_P        0x001UL
#define NXV_PTE_RW       0x002UL
#define NXV_PTE_PWT      0x008UL        /* PAT1 = write-combining */
#define NXV_PTE_PCD      0x010UL        /* PAT3 = uncached */
/* Not-present software marker for committed PAGE_NOACCESS frames. */
#define NXV_PTE_SW_COMMIT 0x800UL
/* Transient marker on pages committed by the in-progress call, so a
 * mid-range exhaustion rolls back exactly this call's pages (bit 10 is
 * hardware-ignored). */
#define NXV_PTE_FRESH     0x400UL

#define NXV_PDE(Va)  (((PULONG)0xC0300000)[(ULONG_PTR)(Va) >> 22])
#define NXV_PTE(Va)  (((PULONG)0xC0000000)[(ULONG_PTR)(Va) >> 12])

#define NXV_TAG 'mVxN'

typedef struct _NXV_REGION
{
    LIST_ENTRY Link;                    /* sorted by Base */
    ULONG_PTR Base;
    SIZE_T Size;
    ULONG AllocProtect;
} NXV_REGION;

static LIST_ENTRY NxvRegionList;
static FAST_MUTEX NxvMutex;
static volatile LONG NxvInitDone;
static SIZE_T NxvCommitted;             /* bytes, data pages only */
static SIZE_T NxvReserved;              /* bytes */

/* INIT *********************************************************************/

static VOID
NxvEnsureInit(VOID)
{
    if (NxvInitDone) return;
    /* First use is the single-threaded XBE load; no real race. */
    InitializeListHead(&NxvRegionList);
    ExInitializeFastMutex(&NxvMutex);
    InterlockedExchange(&NxvInitDone, 1);
}

/* PROTECTION MAPPING *******************************************************/

static BOOLEAN
NxvProtectToPte(
    IN ULONG Protect,
    OUT PULONG PteBits)
{
    ULONG Caching = 0;

    if (Protect & PAGE_NOCACHE) Caching |= NXV_PTE_PCD;
    if (Protect & PAGE_WRITECOMBINE) Caching |= NXV_PTE_PWT;
    if ((Caching & NXV_PTE_PCD) && (Caching & NXV_PTE_PWT)) return FALSE;

    switch (Protect & ~(PAGE_NOCACHE | PAGE_WRITECOMBINE | PAGE_GUARD))
    {
        case PAGE_NOACCESS:
            if (Caching) return FALSE;
            *PteBits = NXV_PTE_SW_COMMIT;
            return TRUE;
        case PAGE_READONLY:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
            *PteBits = NXV_PTE_P | Caching;
            return TRUE;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            *PteBits = NXV_PTE_P | NXV_PTE_RW | Caching;
            return TRUE;
        default:
            return FALSE;
    }
}

static ULONG
NxvPteToProtect(
    IN ULONG Pte)
{
    ULONG Protect;

    if (Pte == 0) return 0;
    if (!(Pte & NXV_PTE_P)) return PAGE_NOACCESS;

    Protect = (Pte & NXV_PTE_RW) ? PAGE_READWRITE : PAGE_READONLY;
    if (Pte & NXV_PTE_PCD) Protect |= PAGE_NOCACHE;
    if (Pte & NXV_PTE_PWT) Protect |= PAGE_WRITECOMBINE;
    return Protect;
}

/* PAGE-TABLE HELPERS *******************************************************/

static BOOLEAN
NxvEnsurePageTable(
    IN ULONG_PTR Va)
{
    PFN_NUMBER Pfn;

    if (NXV_PDE(Va) & NXV_PTE_P) return TRUE;

    Pfn = MmAllocPage(MC_SYSTEM);       /* zeroed */
    if (Pfn == 0) return FALSE;
    NXV_PDE(Va) = ((ULONG)Pfn << PAGE_SHIFT) | NXV_PTE_P | NXV_PTE_RW;
    /* Page tables relocate like any PTE-mapped frame; the owner slot is
     * the PDE, which the relocation pass recognizes by address. */
    {
        KIRQL OldIrql = MiAcquirePfnLock();
        NxkPageSupplySetOwner(Pfn, (ULONG_PTR)&NXV_PDE(Va));
        MiReleasePfnLock(OldIrql);
    }
    return TRUE;
}

static VOID
NxvFreePageAtPte(
    IN PULONG Pte)
{
    PFN_NUMBER Pfn = (*Pte) >> PAGE_SHIFT;
    KIRQL OldIrql;

    *Pte = 0;
    OldIrql = MiAcquirePfnLock();
    NxkPageSupplySetOwner(Pfn, 0);
    MmDereferencePage(Pfn);
    MiReleasePfnLock(OldIrql);
}

/* REGION LIST **************************************************************/

static NXV_REGION *
NxvFindRegion(
    IN ULONG_PTR Va)
{
    PLIST_ENTRY Entry;

    for (Entry = NxvRegionList.Flink; Entry != &NxvRegionList;
         Entry = Entry->Flink)
    {
        NXV_REGION *R = CONTAINING_RECORD(Entry, NXV_REGION, Link);
        if (Va < R->Base) break;        /* sorted */
        if (Va < R->Base + R->Size) return R;
    }
    return NULL;
}

static BOOLEAN
NxvRangeOverlapsRegion(
    IN ULONG_PTR Start,
    IN ULONG_PTR End)
{
    PLIST_ENTRY Entry;

    for (Entry = NxvRegionList.Flink; Entry != &NxvRegionList;
         Entry = Entry->Flink)
    {
        NXV_REGION *R = CONTAINING_RECORD(Entry, NXV_REGION, Link);
        if (R->Base >= End) break;
        if (R->Base + R->Size > Start) return TRUE;
    }
    return FALSE;
}

static VOID
NxvInsertRegion(
    IN NXV_REGION *Region)
{
    PLIST_ENTRY Entry;

    for (Entry = NxvRegionList.Flink; Entry != &NxvRegionList;
         Entry = Entry->Flink)
    {
        NXV_REGION *R = CONTAINING_RECORD(Entry, NXV_REGION, Link);
        if (R->Base > Region->Base) break;
    }
    InsertTailList(Entry, &Region->Link);   /* before Entry */
    NxvReserved += Region->Size;            /* removal sites subtract */
}

static NXV_REGION *
NxvAllocRegion(
    IN ULONG_PTR Base,
    IN SIZE_T Size,
    IN ULONG Protect)
{
    NXV_REGION *Region = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Region),
                                               NXV_TAG);
    if (Region == NULL) return NULL;
    Region->Base = Base;
    Region->Size = Size;
    Region->AllocProtect = Protect;
    return Region;
}

/* First-fit gap search at reserve granularity. */
static ULONG_PTR
NxvFindGap(
    IN SIZE_T Size,
    IN BOOLEAN TopDown)
{
    ULONG_PTR Cursor;
    PLIST_ENTRY Entry;

    if (!TopDown)
    {
        Cursor = NXV_LOWEST;
        for (Entry = NxvRegionList.Flink; Entry != &NxvRegionList;
             Entry = Entry->Flink)
        {
            NXV_REGION *R = CONTAINING_RECORD(Entry, NXV_REGION, Link);
            if (R->Base >= Cursor && R->Base - Cursor >= Size) return Cursor;
            if (R->Base + R->Size > Cursor)
                Cursor = ROUND_UP(R->Base + R->Size, NXV_GRANULARITY);
        }
        if (Cursor + Size <= NXV_HIGHEST) return Cursor;
        return 0;
    }

    Cursor = ROUND_DOWN(NXV_HIGHEST - Size, NXV_GRANULARITY);
    for (Entry = NxvRegionList.Blink; Entry != &NxvRegionList;
         Entry = Entry->Blink)
    {
        NXV_REGION *R = CONTAINING_RECORD(Entry, NXV_REGION, Link);
        if (R->Base + R->Size <= Cursor) break;
        if (R->Base < Size + NXV_LOWEST) return 0;
        Cursor = ROUND_DOWN(R->Base - Size, NXV_GRANULARITY);
    }
    return Cursor >= NXV_LOWEST ? Cursor : 0;
}

/* COMMIT / DECOMMIT ********************************************************/

/* Commit [Start, End): fresh pages from the supply, existing committed
 * pages re-protected in place.  All-or-nothing: this call's fresh
 * pages are rolled back on exhaustion.  Zero is the default; retail
 * honors MEM_NOZERO and hands out pages with unspecified contents. */
static NTSTATUS
NxvCommitRange(
    IN ULONG_PTR Start,
    IN ULONG_PTR End,
    IN ULONG PteBits,
    IN BOOLEAN Zero)
{
    ULONG_PTR Va;

    for (Va = Start; Va < End; Va += PAGE_SIZE)
    {
        PFN_NUMBER Pfn;

        if (!NxvEnsurePageTable(Va)) goto rollback;

        if (NXV_PTE(Va) != 0)
        {
            /* Already committed: protection update only, frame and
             * contents survive (retail-pinned). */
            NXV_PTE(Va) = (NXV_PTE(Va) & ~(ULONG)(PAGE_SIZE - 1)) | PteBits;
            KeInvalidateTlbEntry((PVOID)Va);
            continue;
        }

        {
            KIRQL OldIrql = MiAcquirePfnLock();
            Pfn = Zero ? NxkPageSupplyTakeZero(0) : NxkPageSupplyTakeAny(0);
            MiReleasePfnLock(OldIrql);
        }
        if (Pfn == 0) goto rollback;
        NXV_PTE(Va) = ((ULONG)Pfn << PAGE_SHIFT) | PteBits | NXV_PTE_FRESH;
        /* Record the frame's owning PTE: nxvm pages are CPU-accessed
         * through this VA only, so the contiguous allocator may
         * relocate the frame (retail-probed behavior). */
        NxkPageSupplySetOwner(Pfn, (ULONG_PTR)&NXV_PTE(Va));
        NxvCommitted += PAGE_SIZE;
    }

    for (Va = Start; Va < End; Va += PAGE_SIZE)
    {
        if (NXV_PTE(Va) & NXV_PTE_FRESH)
            NXV_PTE(Va) &= ~NXV_PTE_FRESH;
    }
    return STATUS_SUCCESS;

rollback:
    /* Drop exactly this call's pages; pre-existing committed pages were
     * re-protected in place above and carry no FRESH marker. */
    while (Va > Start)
    {
        Va -= PAGE_SIZE;
        if ((NXV_PDE(Va) & NXV_PTE_P) && (NXV_PTE(Va) & NXV_PTE_FRESH))
        {
            NxvFreePageAtPte(&NXV_PTE(Va));
            KeInvalidateTlbEntry((PVOID)Va);
            NxvCommitted -= PAGE_SIZE;
        }
    }
    return STATUS_NO_MEMORY;
}

static VOID
NxvDecommitRange(
    IN ULONG_PTR Start,
    IN ULONG_PTR End)
{
    ULONG_PTR Va;

    for (Va = Start; Va < End; Va += PAGE_SIZE)
    {
        if (!(NXV_PDE(Va) & NXV_PTE_P))
        {
            Va = (ROUND_UP(Va + 1, 4UL * 1024 * 1024)) - PAGE_SIZE;
            continue;
        }
        if (NXV_PTE(Va) != 0)
        {
            NxvFreePageAtPte(&NXV_PTE(Va));
            KeInvalidateTlbEntry((PVOID)Va);
            NxvCommitted -= PAGE_SIZE;
        }
    }
}

/* PUBLIC: ALLOCATE *********************************************************/

NTSTATUS
NTAPI
NxVmAllocateVirtualMemory(
    IN OUT PVOID *BaseAddress,
    IN ULONG_PTR ZeroBits,
    IN OUT PSIZE_T RegionSize,
    IN ULONG AllocationType,
    IN ULONG Protect)
{
    ULONG_PTR Start, End;
    SIZE_T Size;
    ULONG PteBits;
    NXV_REGION *Region;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(ZeroBits);

    NxvEnsureInit();

    if (BaseAddress == NULL || RegionSize == NULL || *RegionSize == 0)
        return STATUS_INVALID_PARAMETER;
    if (!(AllocationType & (MEM_COMMIT | MEM_RESERVE | MEM_RESET)))
        return STATUS_INVALID_PARAMETER;
    if (!NxvProtectToPte(Protect, &PteBits))
        return STATUS_INVALID_PAGE_PROTECTION;

    /* MEM_RESET: contents may be discarded -- keeping them is a valid
     * implementation, so this is bookkeeping-free. */
    if (AllocationType & MEM_RESET) return STATUS_SUCCESS;

    ExAcquireFastMutex(&NxvMutex);

    if (*BaseAddress == NULL)
    {
        Size = ROUND_UP(*RegionSize, PAGE_SIZE);
        Start = NxvFindGap(Size, BooleanFlagOn(AllocationType, MEM_TOP_DOWN));
        if (Start == 0)
        {
            Status = STATUS_NO_MEMORY;
            goto out;
        }

        Region = NxvAllocRegion(Start, Size, Protect);
        if (Region == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }
        NxvInsertRegion(Region);

        if (AllocationType & MEM_COMMIT)
        {
            Status = NxvCommitRange(Start, Start + Size, PteBits,
                                    !BooleanFlagOn(AllocationType, MEM_NOZERO));
            if (!NT_SUCCESS(Status))
            {
                RemoveEntryList(&Region->Link);
                NxvReserved -= Region->Size;
                ExFreePoolWithTag(Region, NXV_TAG);
                goto out;
            }
        }

        *BaseAddress = (PVOID)Start;
        *RegionSize = Size;
        goto out;
    }

    if ((ULONG_PTR)*BaseAddress < NXV_LOWEST ||
        (ULONG_PTR)*BaseAddress >= NXV_HIGHEST)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto out;
    }

    if (AllocationType & MEM_RESERVE)
    {
        Start = ROUND_DOWN((ULONG_PTR)*BaseAddress, NXV_GRANULARITY);
        End = ROUND_UP((ULONG_PTR)*BaseAddress + *RegionSize, PAGE_SIZE);

        if (NxvRangeOverlapsRegion(Start, End))
        {
            Status = STATUS_CONFLICTING_ADDRESSES;
            goto out;
        }

        Region = NxvAllocRegion(Start, End - Start, Protect);
        if (Region == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }
        NxvInsertRegion(Region);

        if (AllocationType & MEM_COMMIT)
        {
            Status = NxvCommitRange(Start, End, PteBits,
                                    !BooleanFlagOn(AllocationType, MEM_NOZERO));
            if (!NT_SUCCESS(Status))
            {
                RemoveEntryList(&Region->Link);
                NxvReserved -= Region->Size;
                ExFreePoolWithTag(Region, NXV_TAG);
                goto out;
            }
        }

        *BaseAddress = (PVOID)Start;
        *RegionSize = End - Start;
        goto out;
    }

    /* MEM_COMMIT with an explicit base. */
    Start = ROUND_DOWN((ULONG_PTR)*BaseAddress, PAGE_SIZE);
    End = ROUND_UP((ULONG_PTR)*BaseAddress + *RegionSize, PAGE_SIZE);

    Region = NxvFindRegion(Start);
    if (Region == NULL)
    {
        /* Commit on unreserved space: reserve it too. */
        ULONG_PTR RStart = ROUND_DOWN(Start, NXV_GRANULARITY);
        if (NxvRangeOverlapsRegion(RStart, End))
        {
            Status = STATUS_CONFLICTING_ADDRESSES;
            goto out;
        }
        Region = NxvAllocRegion(RStart, End - RStart, Protect);
        if (Region == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }
        NxvInsertRegion(Region);
    }
    else if (End > Region->Base + Region->Size)
    {
        Status = STATUS_CONFLICTING_ADDRESSES;
        goto out;
    }

    Status = NxvCommitRange(Start, End, PteBits,
                                    !BooleanFlagOn(AllocationType, MEM_NOZERO));
    if (NT_SUCCESS(Status))
    {
        *BaseAddress = (PVOID)Start;
        *RegionSize = End - Start;
    }

out:
    ExReleaseFastMutex(&NxvMutex);
    return Status;
}

/* PUBLIC: FREE *************************************************************/

NTSTATUS
NTAPI
NxVmFreeVirtualMemory(
    IN OUT PVOID *BaseAddress,
    IN OUT PSIZE_T RegionSize,
    IN ULONG FreeType)
{
    ULONG_PTR Start, End;
    NXV_REGION *Region;
    NTSTATUS Status = STATUS_SUCCESS;

    NxvEnsureInit();

    if (BaseAddress == NULL || RegionSize == NULL)
        return STATUS_INVALID_PARAMETER;
    if ((FreeType & (MEM_RELEASE | MEM_DECOMMIT)) == 0 ||
        (FreeType & (MEM_RELEASE | MEM_DECOMMIT)) ==
            (MEM_RELEASE | MEM_DECOMMIT))
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&NxvMutex);

    Region = NxvFindRegion((ULONG_PTR)*BaseAddress);
    if (Region == NULL)
    {
        Status = STATUS_MEMORY_NOT_ALLOCATED;
        goto out;
    }

    if (FreeType & MEM_RELEASE)
    {
        if (*RegionSize == 0)
        {
            if ((ULONG_PTR)*BaseAddress != Region->Base)
            {
                Status = STATUS_FREE_VM_NOT_AT_BASE;
                goto out;
            }
            Start = Region->Base;
            End = Region->Base + Region->Size;
        }
        else
        {
            Start = ROUND_DOWN((ULONG_PTR)*BaseAddress, PAGE_SIZE);
            End = ROUND_UP((ULONG_PTR)*BaseAddress + *RegionSize, PAGE_SIZE);
            if (End > Region->Base + Region->Size)
            {
                Status = STATUS_UNABLE_TO_FREE_VM;
                goto out;
            }
        }

        NxvDecommitRange(Start, End);

        /* Carve the released range out of the region. */
        NxvReserved -= (End - Start);
        if (Start == Region->Base && End == Region->Base + Region->Size)
        {
            RemoveEntryList(&Region->Link);
            ExFreePoolWithTag(Region, NXV_TAG);
        }
        else if (Start == Region->Base)
        {
            Region->Base = End;
            Region->Size -= (End - Start);
        }
        else if (End == Region->Base + Region->Size)
        {
            Region->Size = Start - Region->Base;
        }
        else
        {
            /* Middle release: split. */
            NXV_REGION *Tail = NxvAllocRegion(End,
                                              Region->Base + Region->Size - End,
                                              Region->AllocProtect);
            if (Tail == NULL)
            {
                /* Undo the carve accounting; refuse the split. */
                NxvReserved += (End - Start);
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto out;
            }
            Region->Size = Start - Region->Base;
            NxvInsertRegion(Tail);
            NxvReserved -= Tail->Size;  /* InsertRegion re-added it */
        }

        *BaseAddress = (PVOID)Start;
        *RegionSize = End - Start;
        goto out;
    }

    /* MEM_DECOMMIT */
    Start = ROUND_DOWN((ULONG_PTR)*BaseAddress, PAGE_SIZE);
    if (*RegionSize == 0)
        End = Region->Base + Region->Size;
    else
        End = ROUND_UP((ULONG_PTR)*BaseAddress + *RegionSize, PAGE_SIZE);

    if (End > Region->Base + Region->Size)
    {
        Status = STATUS_UNABLE_TO_FREE_VM;
        goto out;
    }

    NxvDecommitRange(Start, End);
    *BaseAddress = (PVOID)Start;
    *RegionSize = End - Start;

out:
    ExReleaseFastMutex(&NxvMutex);
    return Status;
}

/* PUBLIC: QUERY ************************************************************/

NTSTATUS
NTAPI
NxVmQueryVirtualMemory(
    IN PVOID Address,
    OUT PVOID MbiOut)
{
    PMEMORY_BASIC_INFORMATION Mbi = MbiOut;
    ULONG_PTR Va, Run, Limit;
    NXV_REGION *Region;
    ULONG State, Protect;

    NxvEnsureInit();

    if (Mbi == NULL) return STATUS_INVALID_PARAMETER;
    Va = ROUND_DOWN((ULONG_PTR)Address, PAGE_SIZE);
    if (Va >= NXV_HIGHEST) return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&NxvMutex);

    RtlZeroMemory(Mbi, sizeof(*Mbi));
    Mbi->BaseAddress = (PVOID)Va;

    Region = NxvFindRegion(Va);
    if (Region == NULL)
    {
        /* Free: the run extends to the next region (or the top). */
        PLIST_ENTRY Entry;
        Limit = NXV_HIGHEST;
        for (Entry = NxvRegionList.Flink; Entry != &NxvRegionList;
             Entry = Entry->Flink)
        {
            NXV_REGION *R = CONTAINING_RECORD(Entry, NXV_REGION, Link);
            if (R->Base > Va)
            {
                Limit = R->Base;
                break;
            }
        }
        Mbi->AllocationBase = NULL;
        Mbi->AllocationProtect = 0;
        Mbi->RegionSize = Limit - Va;
        Mbi->State = MEM_FREE;
        Mbi->Protect = PAGE_NOACCESS;
        Mbi->Type = 0;
        ExReleaseFastMutex(&NxvMutex);
        return STATUS_SUCCESS;
    }

    /* The run is the span of pages sharing this page's state+protect. */
    if (NXV_PDE(Va) & NXV_PTE_P)
        Protect = NxvPteToProtect(NXV_PTE(Va));
    else
        Protect = 0;
    State = Protect != 0 ? MEM_COMMIT : MEM_RESERVE;

    Limit = Region->Base + Region->Size;
    for (Run = Va + PAGE_SIZE; Run < Limit; Run += PAGE_SIZE)
    {
        ULONG RunProtect = (NXV_PDE(Run) & NXV_PTE_P)
                           ? NxvPteToProtect(NXV_PTE(Run)) : 0;
        if (RunProtect != Protect) break;
    }

    Mbi->AllocationBase = (PVOID)Region->Base;
    Mbi->AllocationProtect = Region->AllocProtect;
    Mbi->RegionSize = Run - Va;
    Mbi->State = State;
    Mbi->Protect = Protect;
    Mbi->Type = MEM_PRIVATE;

    ExReleaseFastMutex(&NxvMutex);
    return STATUS_SUCCESS;
}

/* PUBLIC: ADDRESS PROTECTION ***********************************************/

/* TRUE if the address is nxvm's to answer. */
BOOLEAN
NxVmOwnsAddress(
    IN PVOID Address)
{
    return (ULONG_PTR)Address >= NXV_LOWEST &&
           (ULONG_PTR)Address < NXV_HIGHEST;
}

ULONG
NxVmQueryAddressProtect(
    IN PVOID Address)
{
    ULONG_PTR Va = ROUND_DOWN((ULONG_PTR)Address, PAGE_SIZE);
    ULONG Protect = 0;

    NxvEnsureInit();
    ExAcquireFastMutex(&NxvMutex);
    if ((NXV_PDE(Va) & NXV_PTE_P))
        Protect = NxvPteToProtect(NXV_PTE(Va));
    ExReleaseFastMutex(&NxvMutex);
    return Protect;
}

/* Retail-pinned: PAGE_NOACCESS requests are ignored (protection keeps
 * its previous value); read-only and cacheability changes apply. */
VOID
NxVmSetAddressProtect(
    IN PVOID BaseAddress,
    IN ULONG NumberOfBytes,
    IN ULONG NewProtect)
{
    ULONG_PTR Start = ROUND_DOWN((ULONG_PTR)BaseAddress, PAGE_SIZE);
    ULONG_PTR End = ROUND_UP((ULONG_PTR)BaseAddress + NumberOfBytes,
                             PAGE_SIZE);
    ULONG PteBits;
    ULONG_PTR Va;

    NxvEnsureInit();

    if (!NxvProtectToPte(NewProtect, &PteBits)) return;
    if (PteBits == NXV_PTE_SW_COMMIT) return;   /* NOACCESS ignored */

    ExAcquireFastMutex(&NxvMutex);
    for (Va = Start; Va < End; Va += PAGE_SIZE)
    {
        if (!(NXV_PDE(Va) & NXV_PTE_P)) continue;
        if (NXV_PTE(Va) == 0) continue;
        if (!(NXV_PTE(Va) & NXV_PTE_P)) continue;   /* NOACCESS page */
        NXV_PTE(Va) = (NXV_PTE(Va) & ~(ULONG)(PAGE_SIZE - 1)) | PteBits;
        KeInvalidateTlbEntry((PVOID)Va);
    }
    ExReleaseFastMutex(&NxvMutex);
}

/* PUBLIC: STATISTICS *******************************************************/

SIZE_T
NxVmCommittedBytes(VOID)
{
    return NxvCommitted;
}

SIZE_T
NxVmReservedBytes(VOID)
{
    return NxvReserved;
}

/* Committed pages inside [Start, End) -- feeds the ImagePagesCommitted
 * statistic for the XBE image range.  Lock-free PTE walk: a stale read
 * only skews a statistic. */
PFN_COUNT
NxVmCommittedPagesInRange(ULONG_PTR Start, ULONG_PTR End)
{
    ULONG_PTR Va;
    PFN_COUNT Count = 0;

    for (Va = Start; Va < End; Va += PAGE_SIZE)
    {
        if (!(NXV_PDE(Va) & NXV_PTE_P))
        {
            Va = (ROUND_UP(Va + 1, 4UL * 1024 * 1024)) - PAGE_SIZE;
            continue;
        }
        if (NXV_PTE(Va) != 0)
            Count++;
    }
    return Count;
}
