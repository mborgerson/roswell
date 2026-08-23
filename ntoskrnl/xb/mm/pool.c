/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     nxpool -- the ExAllocatePool* backend.
 *
 * Replaces the ARM3 small-block pool (mm/ARM3/expool.c, unlinked when
 * NXK_MM_POOL is on) with segregated size-class free lists.  Whole
 * pages come from the nxmm window allocator (NXK_MM_POOLPAGES) or the
 * ARM3 pool page layer, selected below.
 *
 * Shape: requests up to 2040 bytes carry an 8-byte header and come
 * from power-of-two free lists carved out of whole nonpaged pages
 * (retail's buckets); anything larger goes straight to whole pages (page-aligned,
 * tracked by VA for ExQueryPoolBlockSize, which titles reach through
 * the pool-size ordinal).  PagedPool requests are served by the same
 * nonpaged backend -- there is no paging, only the API distinction.
 * Pages are never returned to the system: pool footprint is bounded
 * by peak usage, which on this machine is what retail does too.
 *
 * One spinlock covers the class lists and the big-block table, so
 * every path is DISPATCH_LEVEL-safe and never waits.  Cache-aligned
 * pool types are treated as their base type, exactly like the ARM3
 * pool this replaces.  Tags are kept in the block headers for
 * debugging; there is no tag accounting (ExGetPoolTagInfo stubs out).
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <xb-debug.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

#include "mm.h"

/* Whole-page source: the nxmm window allocator when enabled, else the
 * ARM3 pool page layer. */
#ifdef NXK_MM_POOLPAGES
#define NXP_PAGES_ALLOC(Bytes) NxPoolPagesAlloc(Bytes)
#define NXP_PAGES_FREE(Va)     NxPoolPagesFree(Va)
#else
#define NXP_PAGES_ALLOC(Bytes) MiAllocatePoolPages(NonPagedPool, (Bytes))
#define NXP_PAGES_FREE(Va)     MiFreePoolPages(Va)
#endif

/* SIZE CLASSES *************************************************************/

/* Block sizes include the 8-byte header.  Retail-pinned: the pool-size
 * ordinal shows power-of-two buckets from 32 to 2048, and requests
 * above 2040 user bytes go to whole pages (mm/bigpool pool_query_sizes,
 * --official validated). */
static const USHORT NxpClassSize[] = {
    32, 64, 128, 256, 512, 1024, 2048,
};
#define NXP_CLASSES       RTL_NUMBER_OF(NxpClassSize)
#define NXP_SMALL_MAX     2040    /* user bytes; above = whole pages */

typedef struct _NXP_HEADER
{
    ULONG  Tag;
    USHORT ClassIndex;
    USHORT Magic;
} NXP_HEADER;

#define NXP_MAGIC_ALLOC   0x4E58  /* live block */
#define NXP_MAGIC_FREE    0x5846  /* on a class free list */

C_ASSERT(sizeof(NXP_HEADER) == 8);

typedef struct _NXP_FREE
{
    struct _NXP_FREE *Next;
} NXP_FREE;

/* (block bytes + 15) >> 4 -> class index; 4096 >> 4 entries + 1. */
static UCHAR NxpClassMap[(PAGE_SIZE >> 4) + 1];
static NXP_FREE *NxpFreeLists[NXP_CLASSES];
static KSPIN_LOCK NxpLock;

/* Per-window-page state for sub-page bucket pages.  One byte per page:
 * NXP_NOT_BUCKET means the page isn't a bucket page; 0..128 is the live
 * sub-page block count (the smallest 32-byte class fits 128 blocks, so
 * 129..254 are spare and 0xFF is free as the sentinel).  A count of 0 is
 * thus an empty-but-still-mapped bucket page.  Empty bucket pages are NOT
 * freed on the spot -- hot pools (IRPs during streaming) alloc/free a
 * single object and a per-free release would thrash a fresh page per
 * IO.  Instead NxpReclaimEmptyBucketPages sweeps them back to the
 * supply when a whole-page allocation can't be satisfied: capacity is
 * elastic at the exhaustion edge, steady-state stays fast. */
#define NXP_NOT_BUCKET 0xFF
static UCHAR NxpPageState[NXK_POOL_WINDOW_PAGES];
#define NXP_PAGE_INDEX(P) \
    ((ULONG)(((ULONG_PTR)(P) - NXK_POOL_WINDOW_BASE) >> PAGE_SHIFT))

/* BIG-BLOCK TABLE **********************************************************/

/* VA -> page count for whole-page allocations, only so the pool-size
 * query keeps answering for them.  Fixed size: overflow just degrades
 * that query to 0 for the untracked block (alloc/free are unaffected;
 * MiFreePoolPages knows its own sizes). */
typedef struct _NXP_BIG_ENTRY
{
    PVOID Va;
    SIZE_T Pages;
} NXP_BIG_ENTRY;

#define NXP_BIG_ENTRIES 128
static NXP_BIG_ENTRY NxpBigTable[NXP_BIG_ENTRIES];

/* LINK-LEVEL SURFACE OF THE UNLINKED expool.c ******************************/

/* kd64/kddata.c (DBG tree) and mm/ARM3/special.c reference these. */
ULONG ExpPoolFlags;
ULONG ExpNumberOfPagedPools;
POOL_DESCRIPTOR NonPagedPoolDescriptor;
PPOOL_DESCRIPTOR ExpPagedPoolDescriptor[16 + 1];
PPOOL_TRACKER_TABLE PoolTrackTable;

/* INIT *********************************************************************/

VOID
NTAPI
InitializePool(IN POOL_TYPE PoolType,
               IN ULONG Threshold)
{
    ULONG u, c;

    UNREFERENCED_PARAMETER(Threshold);

    /* The PagedPool call from ARM3 init has nothing to set up. */
    if ((PoolType & BASE_POOL_TYPE_MASK) != NonPagedPool) return;

    KeInitializeSpinLock(&NxpLock);

    /* No page is a bucket page yet (0 would read as an empty bucket). */
    RtlFillMemory(NxpPageState, sizeof(NxpPageState), NXP_NOT_BUCKET);

    /* Map every 16-byte block-size step to the smallest fitting class. */
    c = 0;
    for (u = 0; u <= (PAGE_SIZE >> 4); u++)
    {
        while (c < NXP_CLASSES - 1 && (ULONG)NxpClassSize[c] < (u << 4)) c++;
        NxpClassMap[u] = (UCHAR)c;
    }
}

#ifdef NXK_MM_POOLPAGES
/* Residual link surface of the unlinked ARM3 pool page layer
 * (mm/ARM3/pool.c): init-written globals and API stubs that keep
 * their prior Xbox behavior. */
MM_PAGED_POOL_INFO MmPagedPoolInfo;
KGUARDED_MUTEX MmPagedPoolMutex;
SIZE_T MmAllocatedNonPagedPool;
ULONG MmSpecialPoolTag;
PFN_NUMBER MiExpansionPoolPagesInitialCharge;

/* ps/quota.c links this; quota is never raised on a single-process
 * kernel. */
BOOLEAN
NTAPI
MmRaisePoolQuota(
    IN POOL_TYPE PoolType,
    IN SIZE_T CurrentMaxQuota,
    OUT PSIZE_T NewMaxQuota)
{
    UNREFERENCED_PARAMETER(PoolType);
    UNREFERENCED_PARAMETER(CurrentMaxQuota);
    if (NewMaxQuota) *NewMaxQuota = 0;
    return FALSE;
}

/* Pre-existing Xbox stubs (not in the title ABI; the PIO reserved-VA
 * consumer tolerates NULL). */
PVOID
NTAPI
MmAllocateMappingAddress(
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG PoolTag)
{
    UNREFERENCED_PARAMETER(NumberOfBytes);
    UNREFERENCED_PARAMETER(PoolTag);
    return NULL;
}

VOID
NTAPI
MmFreeMappingAddress(
    _In_ PVOID BaseAddress,
    _In_ ULONG PoolTag)
{
    UNREFERENCED_PARAMETER(BaseAddress);
    UNREFERENCED_PARAMETER(PoolTag);
}
#endif

/* Session pool descriptor init (mm/ARM3/pool.c) -- no session pool. */
VOID
NTAPI
ExInitializePoolDescriptor(IN PPOOL_DESCRIPTOR PoolDescriptor,
                           IN POOL_TYPE PoolType,
                           IN ULONG PoolIndex,
                           IN ULONG Threshold,
                           IN PVOID PoolLock)
{
    UNREFERENCED_PARAMETER(PoolDescriptor);
    UNREFERENCED_PARAMETER(PoolType);
    UNREFERENCED_PARAMETER(PoolIndex);
    UNREFERENCED_PARAMETER(Threshold);
    UNREFERENCED_PARAMETER(PoolLock);
}

/* ALLOCATION ***************************************************************/

ULONG NxpReclaimEmptyBucketPages(VOID);

static VOID
NxpAllocFailed(IN POOL_TYPE PoolType,
               IN SIZE_T NumberOfBytes)
{
    if (PoolType & MUST_SUCCEED_POOL_MASK)
    {
        KeBugCheckEx(MUST_SUCCEED_POOL_EMPTY, NumberOfBytes, 0, 0, 0);
    }
    if (PoolType & POOL_RAISE_IF_ALLOCATION_FAILURE)
    {
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    }
}

PVOID
NTAPI
ExAllocatePoolWithTag(IN POOL_TYPE PoolType,
                      IN SIZE_T NumberOfBytes,
                      IN ULONG Tag)
{
    SIZE_T BlockBytes;
    ULONG Class;
    KIRQL OldIrql;
    NXP_HEADER *Hdr;
    NXP_FREE *Blk;

    /* Whole-page path. */
    if (NumberOfBytes > NXP_SMALL_MAX)
    {
        PVOID Va = NXP_PAGES_ALLOC(NumberOfBytes);
        if (Va == NULL && NxpReclaimEmptyBucketPages() != 0)
            Va = NXP_PAGES_ALLOC(NumberOfBytes);
        if (Va == NULL)
        {
            NxpAllocFailed(PoolType, NumberOfBytes);
            return NULL;
        }
        ASSERT(PAGE_ALIGN(Va) == Va);

        KeAcquireSpinLock(&NxpLock, &OldIrql);
        for (ULONG i = 0; i < NXP_BIG_ENTRIES; i++)
        {
            if (NxpBigTable[i].Va == NULL)
            {
                NxpBigTable[i].Va = Va;
                NxpBigTable[i].Pages = BYTES_TO_PAGES(NumberOfBytes);
                break;
            }
        }
        KeReleaseSpinLock(&NxpLock, OldIrql);
        return Va;
    }

    BlockBytes = (NumberOfBytes ? NumberOfBytes : 1) + sizeof(NXP_HEADER);
    Class = NxpClassMap[(BlockBytes + 15) >> 4];

    KeAcquireSpinLock(&NxpLock, &OldIrql);
    Blk = NxpFreeLists[Class];
    if (Blk != NULL)
    {
        NxpFreeLists[Class] = Blk->Next;
        NxpPageState[NXP_PAGE_INDEX(Blk)]++;
        KeReleaseSpinLock(&NxpLock, OldIrql);
        ASSERT((((NXP_HEADER *)Blk) - 1)->Magic == NXP_MAGIC_FREE);
    }
    else
    {
        /* Refill: carve a fresh page into this class.  The page layer
         * has its own lock; don't hold ours across it. */
        ULONG Size = NxpClassSize[Class];
        ULONG Count = PAGE_SIZE / Size;
        PUCHAR Page;

        KeReleaseSpinLock(&NxpLock, OldIrql);
        Page = NXP_PAGES_ALLOC(PAGE_SIZE);
        if (Page == NULL && NxpReclaimEmptyBucketPages() != 0)
            Page = NXP_PAGES_ALLOC(PAGE_SIZE);
        if (Page == NULL)
        {
            NxpAllocFailed(PoolType, NumberOfBytes);
            return NULL;
        }

        /* Block 0 satisfies this request; chain the rest. */
        KeAcquireSpinLock(&NxpLock, &OldIrql);
        /* Mark as a bucket page with one live block in a single write. */
        NxpPageState[NXP_PAGE_INDEX(Page)] = 1;
        for (ULONG i = 1; i < Count; i++)
        {
            NXP_HEADER *H = (NXP_HEADER *)(Page + i * Size);
            NXP_FREE *F = (NXP_FREE *)(H + 1);
            H->Tag = 0;
            H->ClassIndex = (USHORT)Class;
            H->Magic = NXP_MAGIC_FREE;
            F->Next = NxpFreeLists[Class];
            NxpFreeLists[Class] = F;
        }
        KeReleaseSpinLock(&NxpLock, OldIrql);
        Blk = (NXP_FREE *)(Page + sizeof(NXP_HEADER));
    }

    /* Fresh-page block 0 carries whatever the page held before; the
     * header is fully (re)written here either way. */
    Hdr = ((NXP_HEADER *)Blk) - 1;
    Hdr->Tag = Tag;
    Hdr->ClassIndex = (USHORT)Class;
    Hdr->Magic = NXP_MAGIC_ALLOC;
    return Blk;
}

PVOID
NTAPI
ExAllocatePool(POOL_TYPE PoolType,
               SIZE_T NumberOfBytes)
{
    return ExAllocatePoolWithTag(PoolType, NumberOfBytes, TAG_NONE);
}

/* FREE *********************************************************************/

VOID
NTAPI
ExFreePoolWithTag(IN PVOID P,
                  IN ULONG TagToFree)
{
    NXP_HEADER *Hdr;
    NXP_FREE *Blk;
    KIRQL OldIrql;

    if (P == NULL)
    {
        KeBugCheckEx(BAD_POOL_CALLER, 0x46, 0, 0, 0);
    }

    /* Whole-page path. */
    if (PAGE_ALIGN(P) == P)
    {
#if DBG && defined(NXK_MM_POOLPAGES)
        {
            VOID NxPoolPagesNoteFreer(PVOID);
            NxPoolPagesNoteFreer(_ReturnAddress());
        }
#endif
        KeAcquireSpinLock(&NxpLock, &OldIrql);
        for (ULONG i = 0; i < NXP_BIG_ENTRIES; i++)
        {
            if (NxpBigTable[i].Va == P)
            {
                NxpBigTable[i].Va = NULL;
                NxpBigTable[i].Pages = 0;
                break;
            }
        }
        KeReleaseSpinLock(&NxpLock, OldIrql);
        NXP_PAGES_FREE(P);
        return;
    }

    Hdr = ((NXP_HEADER *)P) - 1;
    if (Hdr->Magic != NXP_MAGIC_ALLOC)
    {
        /* Double free or never a pool block. */
        KeBugCheckEx(BAD_POOL_HEADER,
                     (Hdr->Magic == NXP_MAGIC_FREE) ? 7 : 8,
                     (ULONG_PTR)P, Hdr->Magic, Hdr->Tag);
    }
    if (TagToFree != 0 && Hdr->Tag != TagToFree)
    {
        KeBugCheckEx(BAD_POOL_CALLER, 0x0A,
                     (ULONG_PTR)P, Hdr->Tag, TagToFree);
    }
    ASSERT(Hdr->ClassIndex < NXP_CLASSES);

    Hdr->Magic = NXP_MAGIC_FREE;
    Blk = (NXP_FREE *)P;

    KeAcquireSpinLock(&NxpLock, &OldIrql);
    Blk->Next = NxpFreeLists[Hdr->ClassIndex];
    NxpFreeLists[Hdr->ClassIndex] = Blk;

    NxpPageState[NXP_PAGE_INDEX(P)]--;
    KeReleaseSpinLock(&NxpLock, OldIrql);
}

/* Sweep empty bucket pages back to the supply.  Called when a
 * whole-page allocation fails; returns the number of pages freed. */
ULONG
NxpReclaimEmptyBucketPages(VOID)
{
    ULONG Freed = 0;
    ULONG Idx;
    KIRQL OldIrql;

    for (Idx = 0; Idx < NXK_POOL_WINDOW_PAGES; Idx++)
    {
        ULONG_PTR PageVa;
        NXP_HEADER *Hdr0;
        NXP_FREE **Link;

        /* Only empty bucket pages (state == 0) are reclaimable; a
         * non-bucket page reads NXP_NOT_BUCKET and a live one reads > 0. */
        if (NxpPageState[Idx] != 0)
            continue;

        KeAcquireSpinLock(&NxpLock, &OldIrql);
        if (NxpPageState[Idx] != 0)
        {
            KeReleaseSpinLock(&NxpLock, OldIrql);
            continue;
        }

        PageVa = NXK_POOL_WINDOW_BASE + ((ULONG_PTR)Idx << PAGE_SHIFT);
        Hdr0 = (NXP_HEADER *)PageVa;
        Link = &NxpFreeLists[Hdr0->ClassIndex];
        while (*Link != NULL)
        {
            if (((ULONG_PTR)*Link & ~(ULONG_PTR)(PAGE_SIZE - 1)) == PageVa)
                *Link = (*Link)->Next;
            else
                Link = &(*Link)->Next;
        }
        NxpPageState[Idx] = NXP_NOT_BUCKET;
        KeReleaseSpinLock(&NxpLock, OldIrql);

        NXP_PAGES_FREE((PVOID)PageVa);
        Freed++;
    }
    return Freed;
}

VOID
NTAPI
ExFreePool(PVOID P)
{
    ExFreePoolWithTag(P, 0);
}

/* QUERY ********************************************************************/

SIZE_T
NTAPI
ExQueryPoolBlockSize(IN PVOID PoolBlock,
                     OUT PBOOLEAN QuotaCharged)
{
    SIZE_T Size = 0;
    KIRQL OldIrql;
    NXP_HEADER *Hdr;

    if (QuotaCharged != NULL) *QuotaCharged = FALSE;

    if (PAGE_ALIGN(PoolBlock) == PoolBlock)
    {
        KeAcquireSpinLock(&NxpLock, &OldIrql);
        for (ULONG i = 0; i < NXP_BIG_ENTRIES; i++)
        {
            if (NxpBigTable[i].Va == PoolBlock)
            {
                Size = NxpBigTable[i].Pages * PAGE_SIZE;
                break;
            }
        }
        KeReleaseSpinLock(&NxpLock, OldIrql);
        return Size;
    }

    Hdr = ((NXP_HEADER *)PoolBlock) - 1;
    ASSERT(Hdr->Magic == NXP_MAGIC_ALLOC);
    ASSERT(Hdr->ClassIndex < NXP_CLASSES);
    return (SIZE_T)NxpClassSize[Hdr->ClassIndex] - sizeof(NXP_HEADER);
}

/* QUOTA / PRIORITY WRAPPERS ************************************************/

/* Single-process kernel: no quota is tracked or charged. */

PVOID
NTAPI
ExAllocatePoolWithQuotaTag(IN POOL_TYPE PoolType,
                           IN SIZE_T NumberOfBytes,
                           IN ULONG Tag)
{
    BOOLEAN Raise = !(PoolType & POOL_QUOTA_FAIL_INSTEAD_OF_RAISE);
    PVOID Buffer;

    PoolType &= ~POOL_QUOTA_FAIL_INSTEAD_OF_RAISE;
    Buffer = ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);
    if (Buffer == NULL && Raise) RtlRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    return Buffer;
}

PVOID
NTAPI
ExAllocatePoolWithQuota(IN POOL_TYPE PoolType,
                        IN SIZE_T NumberOfBytes)
{
    return ExAllocatePoolWithQuotaTag(PoolType, NumberOfBytes, TAG_NONE);
}

PVOID
NTAPI
ExAllocatePoolWithTagPriority(IN POOL_TYPE PoolType,
                              IN SIZE_T NumberOfBytes,
                              IN ULONG Tag,
                              IN EX_POOL_PRIORITY Priority)
{
    UNREFERENCED_PARAMETER(Priority);
    return ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);
}

VOID
NTAPI
ExReturnPoolQuota(IN PVOID P)
{
    UNREFERENCED_PARAMETER(P);
}

/* No tag accounting; the query interface reports unimplemented. */
NTSTATUS
NTAPI
ExGetPoolTagInfo(IN PSYSTEM_POOLTAG_INFORMATION SystemInformation,
                 IN ULONG SystemInformationLength,
                 IN OUT PULONG ReturnLength OPTIONAL)
{
    UNREFERENCED_PARAMETER(SystemInformation);
    UNREFERENCED_PARAMETER(SystemInformationLength);
    if (ReturnLength != NULL) *ReturnLength = 0;
    return STATUS_NOT_IMPLEMENTED;
}
