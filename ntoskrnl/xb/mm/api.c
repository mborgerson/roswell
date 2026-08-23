/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Title-facing Mm* ABI ordinals that have no NT analog (or
 *              whose signature differs enough that we wrap rather than
 *              forwarding straight to the kernel).
 *
 * Most are thin -- the heavy lifting sits in the Nx* helpers in xb/mm/
 * contig.c / sysva.c / iospace.c.  This file holds:
 *   - MmAllocateSystemMemory / MmFreeSystemMemory     (system-VA aperture)
 *   - MmLockUnlockBufferPages / MmLockUnlockPhysicalPage  (no-op: no pager)
 *   - MmQueryAddressProtect / MmQueryStatistics       (queries)
 *   - NxMmCreateKernelStack / NxMmDeleteKernelStack   (per-thread stack)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>
#include <xb-debug.h>

#include "mm.h"

#define NXMM_TAG  'mMxN'

/* SYSTEM MEMORY ************************************************************/

/* Xbox MmAllocateSystemMemory hands out VAs in the documented system-VA
 * window 0xD0000000-0xEFFFFFFF (memory-map.md SS1).  Routed through the
 * NxkSysVa* helpers in xb/mm/sysva.c: NT's NP pool supplies the backing
 * PAs, we alias them into the aperture so callers see a retail-shaped VA. */
PVOID NTAPI
MmAllocateSystemMemory(_In_ SIZE_T Size, _In_ ULONG Protect)
{
    PVOID p = NxkSysVaAllocate(Size, Protect);
    if (p == NULL)
    {
        DPRINT1("MmAllocateSystemMemory(%lu, %lx) -> aperture exhausted, "
                "falling back to NP pool\n", (ULONG)Size, Protect);
        return ExAllocatePoolWithTag(NonPagedPool, Size, NXMM_TAG);
    }
    return p;
}

/*
 * Free a MmAllocateSystemMemory pointer.  Returns the number of pages
 * freed -- *not* a boolean.  Page-rounded up: a 1-byte free still
 * accounts for the single page, and a 4 KB + 1 byte free returns 2.
 * Caller passes the original allocation size so we don't need to query.
 */
ULONG NTAPI
MmFreeSystemMemory(_In_ PVOID Base, _In_ SIZE_T Size)
{
    if (Base == NULL)
        return 0;

    if ((ULONG_PTR)Base >= NXK_SYSMEM_BASE &&
        (ULONG_PTR)Base <  NXK_SYSMEM_LIMIT)
    {
        return NxkSysVaFree(Base, Size);
    }

    /* Aperture-exhausted fallback: NP pool. */
    ExFreePoolWithTag(Base, NXMM_TAG);
    return (ULONG)((Size + PAGE_SIZE - 1) >> PAGE_SHIFT);
}

/* LOCK / UNLOCK ************************************************************/

/* No-ops: the kernel never pages out, so locking against the pager has
 * nothing to do. */
ULONG NTAPI
MmLockUnlockBufferPages(_In_ PVOID Base, _In_ SIZE_T Size, _In_ BOOLEAN Unlock)
{
    UNREFERENCED_PARAMETER(Base);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Unlock);
    return 0;
}
ULONG NTAPI
MmLockUnlockPhysicalPage(_In_ ULONG_PTR Pfn, _In_ BOOLEAN Unlock)
{
    UNREFERENCED_PARAMETER(Pfn);
    UNREFERENCED_PARAMETER(Unlock);
    return 0;
}

/* QUERIES ******************************************************************/

/* Retail walks the PTE to recover protection bits.  nxvm answers for
 * the title VA range; the static windows stay constant PAGE_READWRITE. */
ULONG NTAPI
MmQueryAddressProtect(_In_ PVOID Base)
{
#ifdef NXK_MM_VM
    if (NxVmOwnsAddress(Base))
        return NxVmQueryAddressProtect(Base);
#endif
    UNREFERENCED_PARAMETER(Base);
    return 0x04;  /* PAGE_READWRITE */
}

/* Xbox MM_STATISTICS is 9 ULONGs (0x24 bytes); the caller fills Length and
 * the kernel validates it before writing.  Only touch the caller's struct. */
typedef struct _MM_STATISTICS
{
    ULONG Length;
    ULONG TotalPhysicalPages;
    ULONG AvailablePages;
    ULONG VirtualMemoryBytesCommitted;
    ULONG VirtualMemoryBytesReserved;
    ULONG CachePagesCommitted;
    ULONG PoolPagesCommitted;
    ULONG StackPagesCommitted;
    ULONG ImagePagesCommitted;
} MM_STATISTICS, *PMM_STATISTICS;

extern PFN_COUNT MmNumberOfPhysicalPages;
extern PFN_NUMBER MmAvailablePages;
extern ULONG_PTR XeImageRangeStart;
extern ULONG_PTR XeImageRangeEnd;

/* Kernel-stack pages, all sources (ARM3 thread stacks and the pool-
 * backed title stacks below).  Retail-probed semantics: stacks are
 * their own class, exactly size/PAGE_SIZE pages, no guard page. */
volatile LONG NxkMmStackPages;
volatile LONG NxkMmStackPagesInPool;

NTSTATUS NTAPI
MmQueryStatistics(_Inout_ PMM_STATISTICS Stats)
{
    if (Stats == NULL || Stats->Length != sizeof(MM_STATISTICS))
        return STATUS_INVALID_PARAMETER;

    Stats->TotalPhysicalPages          = MmNumberOfPhysicalPages;
    Stats->AvailablePages              = (ULONG)MmAvailablePages;
#ifdef NXK_MM_VM
    Stats->VirtualMemoryBytesCommitted = (ULONG)NxVmCommittedBytes();
    Stats->VirtualMemoryBytesReserved  = (ULONG)NxVmReservedBytes();
#else
    Stats->VirtualMemoryBytesCommitted = 0;
    Stats->VirtualMemoryBytesReserved  = 0;
#endif
#ifdef NXK_MM_CACHE
    {
        extern PFN_COUNT NTAPI FscGetCacheSize(VOID);
        Stats->CachePagesCommitted     = FscGetCacheSize();
    }
#else
    Stats->CachePagesCommitted         = 0;
#endif
    /* The classes are disjoint on retail: cache buffers and title
     * kernel stacks live in our pool window but count only under
     * their own statistic. */
#ifdef NXK_MM_POOLPAGES
    {
        LONG Pool = (LONG)NxPoolPagesCommitted()
                    - NxkMmStackPagesInPool
                    - (LONG)Stats->CachePagesCommitted;
        Stats->PoolPagesCommitted      = Pool > 0 ? (ULONG)Pool : 0;
    }
#else
    Stats->PoolPagesCommitted          = 0;
#endif
    Stats->StackPagesCommitted         = NxkMmStackPages > 0
                                         ? (ULONG)NxkMmStackPages : 0;
#ifdef NXK_MM_VM
    Stats->ImagePagesCommitted         = XeImageRangeEnd != 0
        ? NxVmCommittedPagesInRange(XeImageRangeStart, XeImageRangeEnd) : 0;
#else
    Stats->ImagePagesCommitted         = 0;
#endif
    return STATUS_SUCCESS;
}

/* KERNEL STACK *************************************************************/

/*
 * MmCreateKernelStack(NumberOfBytes, DebuggerThread).  Allocate a
 * kernel-mode stack for a worker thread the title is about to spawn.
 * The caller treats the return value as the initial ESP -- the stack
 * grows down from there -- so we hand back BASE + NumberOfBytes, with
 * NumberOfBytes page-aligned upward.  Stacks live in NonPagedPool: they
 * must stay wired and need a kernel-VA the title can hand to
 * PsCreateSystemThreadEx as its KernelStack argument.
 */
PVOID
NTAPI
NxMmCreateKernelStack(_In_ SIZE_T NumberOfBytes, _In_ BOOLEAN DebuggerThread)
{
    PVOID Base;
    UNREFERENCED_PARAMETER(DebuggerThread);

    if (NumberOfBytes == 0)
        return NULL;
    NumberOfBytes = (NumberOfBytes + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1);

    Base = ExAllocatePoolWithTag(NonPagedPool, NumberOfBytes, 'kStN');
    if (Base == NULL)
        return NULL;

    InterlockedExchangeAdd((volatile LONG *)&NxkMmStackPages,
                           (LONG)(NumberOfBytes >> PAGE_SHIFT));
    InterlockedExchangeAdd((volatile LONG *)&NxkMmStackPagesInPool,
                           (LONG)(NumberOfBytes >> PAGE_SHIFT));
    return (PVOID)((ULONG_PTR)Base + NumberOfBytes);
}

/*
 * MmDeleteKernelStack(StackBase, StackLimit).  The title hands back the
 * pair returned from MmCreateKernelStack: StackBase is the initial-ESP
 * value, StackLimit is one page below it (per the Xbox SDK convention
 * that StackLimit = "lowest valid address").  We allocated from
 * StackBase - SIZE, so subtract to get the pool block.
 */
VOID
NTAPI
NxMmDeleteKernelStack(_In_ PVOID StackBase, _In_ PVOID StackLimit)
{
    ULONG_PTR Base = (ULONG_PTR)StackBase;
    ULONG_PTR Limit = (ULONG_PTR)StackLimit;
    SIZE_T Size;

    if (StackBase == NULL || Limit >= Base)
        return;

    Size = Base - Limit;
    Size = (Size + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1);
    ExFreePoolWithTag((PVOID)(Base - Size), 'kStN');
    InterlockedExchangeAdd((volatile LONG *)&NxkMmStackPages,
                           -(LONG)(Size >> PAGE_SHIFT));
    InterlockedExchangeAdd((volatile LONG *)&NxkMmStackPagesInPool,
                           -(LONG)(Size >> PAGE_SHIFT));
}

/* EOF */
