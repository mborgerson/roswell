/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     nxcache -- the file cache behind the Cc* API.
 *
 * Replaces the section/VACB cache (ntoskrnl/cc/, unlinked when
 * NXK_MM_CACHE is on) with a fixed pool of 64 KiB buffers keyed by
 * (SectionObjectPointer, block-aligned file offset).  Misses read
 * through with IoPageRead; dirty blocks write back with
 * IoSynchronousPageWrite -- the FSDs serve both as ordinary paging IO,
 * so no data sections, views, or VM pages are involved and the cache
 * footprint is exactly the block pool.
 *
 * Write-back happens on explicit flush, file close, purge-with-flush
 * semantics, and shutdown; there is no lazy writer (matching the
 * pre-existing trim: CcCanIWrite is unconditional TRUE and writes
 * never throttle).  Eviction is clean-first LRU.
 *
 * Locking: one mutex guards all cache state and is NEVER held across
 * IO -- a block doing IO is marked Busy and referenced, and contenders
 * retry.  This keeps the FSD recursion legal: a paging read issued for
 * a data miss re-enters the cache for FAT metadata.
 *
 * Pin semantics: pins reference the underlying block; no per-BCB lock
 * is taken (the FSDs serialize metadata through their own resources,
 * and there is no lazy writer to race with).
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <xb-debug.h>
#define NDEBUG
#include <debug.h>

#include "mm.h"

/* GLOBALS (sysinfo counter surface of the unlinked cc/) *******************/

ULONG CcMapDataWait;
ULONG CcMapDataNoWait;
ULONG CcPinReadWait;
ULONG CcPinReadNoWait;
ULONG CcPinMappedDataCount;
ULONG CcLazyWriteIos;
ULONG CcLazyWritePages;
ULONG CcDataFlushes;
ULONG CcDataPages;

/* STRUCTURES ***************************************************************/

/* 16 KiB blocks: vfat's spans are dir pages (4 KiB, page-aligned) and
 * FAT chunks (CACHEPAGESIZE, chunk-aligned and clamped to the block
 * size), so no reachable request straddles a block.  A cross-block
 * request raises instead of truncating -- truncation walks the caller
 * off its buffer (seen as a vfatFindDirSpace AV historically). */
#define NXC_BLOCK_SHIFT   14
#define NXC_BLOCK_SIZE    (1UL << NXC_BLOCK_SHIFT)      /* 16 KiB */
#define NXC_PAGES_PER_BLOCK (NXC_BLOCK_SIZE / PAGE_SIZE)
/* Retail boots with a 16-page (64 KB) file cache and titles resize it
 * via FscSetCacheSize; every cache page is title-visible capacity, so
 * the boot default matches retail exactly.  Floor of 4 blocks: a pin
 * chain holds data + FAT metadata blocks, and two chains must stay
 * live concurrently. */
#define NXC_DEFAULT_BLOCKS  NXC_MIN_BLOCKS
#define NXC_MIN_BLOCKS      4
/* 512 KiB ceiling.  The descriptor array is static .bss (every entry is
 * title-visible RAM), so the ceiling is sized to observed need, not to a
 * round maximum: many titles never call FscSetCacheSize at all (they run
 * the 64 KiB / 4-block boot default), and the largest request anywhere is
 * the api-regression suite's 64 pages (16 blocks).  32 blocks leaves 2x
 * headroom; a title asking for more is clamped and simply caches less
 * (LRU eviction -- a perf bound, never a correctness one). */
#define NXC_MAX_BLOCKS      32
/* Write-back reserve.  A cached write large enough to dirty every
 * title-visible block deadlocks without it: flushing one dirty block
 * re-enters the FSD to read the FAT (to map the block's cluster), which
 * needs a cache block -- but every block holds this file's dirty data, and
 * freeing one means flushing another dirty block that needs the same FAT,
 * a cycle with no free slot (roswell#26).  These extra blocks are handed
 * out only to a re-entrant metadata read issued from inside a write-back
 * (NxcWbDepth > 0); the data-fill path never touches them, so one is always
 * available to break the cycle.  Two covers a FAT block plus a directory
 * block.  Not title-visible capacity (FscGetCacheSize excludes them). */
#define NXC_WB_RESERVE      2
#define NXC_TOTAL_BLOCKS    (NXC_MAX_BLOCKS + NXC_WB_RESERVE)

#define NXC_TAG           'cCxN'

typedef struct _NXC_MAP
{
    CSHORT NodeTypeCode;            /* NODE_TYPE_SHARED_MAP */
    CSHORT NodeByteSize;
    ULONG OpenCount;                /* live private cache maps */
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER FileSize;
    LARGE_INTEGER ValidDataLength;
    PFILE_OBJECT FileObject;        /* referenced; carries the paging IO */
    PCACHE_MANAGER_CALLBACKS Callbacks;
    PVOID LazyWriteContext;
    BOOLEAN PinAccess;
    LIST_ENTRY MapLinks;            /* in NxcMapList */
    LIST_ENTRY PrivateList;         /* PRIVATE_CACHE_MAPs */
    PRIVATE_CACHE_MAP EmbeddedPrivateMap;
} NXC_MAP;

typedef struct _NXC_BLOCK
{
    NXC_MAP *Map;                   /* NULL = free slot */
    LONGLONG Offset;                /* block-aligned file offset */
    PUCHAR Data;
    LIST_ENTRY LruLink;
    ULONG RefCount;                 /* pins + transient users */
    BOOLEAN Valid;                  /* contents populated */
    BOOLEAN Dirty;
    BOOLEAN Busy;                   /* read-in or write-out in flight */
    BOOLEAN Reserved;               /* write-back-only slot (never a data fill) */
} NXC_BLOCK;

typedef struct _NXC_BCB
{
    PUBLIC_BCB PFCB;                /* must be first: callers hand &PFCB back */
    NXC_BLOCK *Block;
    NXC_MAP *Map;
    LONG RefCount;
    LONG PinCount;
} NXC_BCB;

static NXC_BLOCK NxcBlocks[NXC_TOTAL_BLOCKS];   /* Data==NULL = inactive */
static ULONG NxcActiveBlocks;                   /* title-visible; excludes reserve */
static ULONG NxcWbDepth;                        /* write-back re-entrancy depth */
static LIST_ENTRY NxcLruList;       /* head = oldest */
static LIST_ENTRY NxcMapList;
static FAST_MUTEX NxcMutex;
static PFN_NUMBER NxcZeroPagePfn;   /* backs no-map CcZeroData writes */

#define NXC_NODE_SHARED_MAP  0x02FF
#define NXC_NODE_PRIVATE_MAP 0x02FE
#define NXC_NODE_BCB         0x02FD

/* IO HELPERS (no cache lock held) ******************************************/

static NTSTATUS
NxcPagingIo(
    IN PFILE_OBJECT FileObject,
    IN PVOID Buffer,
    IN LONGLONG Offset,
    IN ULONG Length,
    IN BOOLEAN Write,
    OUT PSIZE_T Transferred)
{
    PMDL Mdl;
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Off;
    NTSTATUS Status;

    *Transferred = 0;

    Mdl = IoAllocateMdl(Buffer, Length, FALSE, FALSE, NULL);
    if (Mdl == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(Mdl);

    Off.QuadPart = Offset;
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Iosb.Status = STATUS_SUCCESS;
    Iosb.Information = 0;

    /* The device DMAs to these frames: hold them immovable across the
     * IO so a concurrent contiguous-allocation relocation can't pull
     * them out from under the transfer. */
    NxPoolPagesSetMovable(Buffer, Length, FALSE);

    if (Write)
        Status = IoSynchronousPageWrite(FileObject, Mdl, &Off, &Event, &Iosb);
    else
        Status = IoPageRead(FileObject, Mdl, &Off, &Event, &Iosb);

    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    NxPoolPagesSetMovable(Buffer, Length, TRUE);

    if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA)
        MmUnmapLockedPages(Mdl->MappedSystemVa, Mdl);
    IoFreeMdl(Mdl);

    *Transferred = Iosb.Information;
    return Status;
}

/* Populate a block from disk.  Called with the mutex held; drops and
 * reacquires it around the IO (Block is Busy + referenced). */
static NTSTATUS
NxcReadBlock(
    IN NXC_BLOCK *Block)
{
    NXC_MAP *Map = Block->Map;
    LONGLONG FileEnd = Map->FileSize.QuadPart;
    ULONG ReadLen = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T Got = 0;

    ASSERT(Block->Busy && Block->RefCount > 0);

    if (Block->Offset < FileEnd)
    {
        LONGLONG Span = FileEnd - Block->Offset;
        ReadLen = Span >= NXC_BLOCK_SIZE
                  ? NXC_BLOCK_SIZE
                  : (ULONG)ROUND_UP((ULONG)Span, PAGE_SIZE);
    }

    if (ReadLen != 0)
    {
        ExReleaseFastMutex(&NxcMutex);
        Status = NxcPagingIo(Map->FileObject, Block->Data, Block->Offset,
                             ReadLen, FALSE, &Got);
        /* A short read at the tail is EOF, not failure. */
        if (Status == STATUS_END_OF_FILE) Status = STATUS_SUCCESS;
        ExAcquireFastMutex(&NxcMutex);
    }

    if (NT_SUCCESS(Status))
    {
        if (Got < NXC_BLOCK_SIZE)
            RtlZeroMemory(Block->Data + Got, NXC_BLOCK_SIZE - Got);
        Block->Valid = TRUE;
        CcDataPages += NXC_BLOCK_SIZE / PAGE_SIZE;
    }

    Block->Busy = FALSE;
    return Status;
}

/* Write a dirty block back.  Same locking protocol as NxcReadBlock. */
static NTSTATUS
NxcFlushBlock(
    IN NXC_BLOCK *Block,
    OUT PSIZE_T Written)
{
    NXC_MAP *Map = Block->Map;
    LONGLONG FileEnd = Map->FileSize.QuadPart;
    ULONG WriteLen = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T Got = 0;

    ASSERT(Block->Busy && Block->RefCount > 0);
    ASSERT(Block->Dirty && Block->Valid);

    if (Block->Offset < FileEnd)
    {
        LONGLONG Span = FileEnd - Block->Offset;
        WriteLen = Span >= NXC_BLOCK_SIZE
                   ? NXC_BLOCK_SIZE
                   : (ULONG)ROUND_UP((ULONG)Span, PAGE_SIZE);
    }

    if (WriteLen != 0)
    {
        /* The paging write re-enters the FSD, which reads the FAT through
         * the cache to map this block's cluster.  Flag the re-entrancy so
         * that nested read can dip into the write-back reserve if every
         * title-visible block is otherwise spoken for (roswell#26). */
        NxcWbDepth++;
        ExReleaseFastMutex(&NxcMutex);
        Status = NxcPagingIo(Map->FileObject, Block->Data, Block->Offset,
                             WriteLen, TRUE, &Got);
        ExAcquireFastMutex(&NxcMutex);
        NxcWbDepth--;
    }

    if (NT_SUCCESS(Status))
    {
        Block->Dirty = FALSE;
        CcDataFlushes++;
        CcLazyWriteIos++;
        CcLazyWritePages += WriteLen / PAGE_SIZE;
        if (Map->ValidDataLength.QuadPart < Block->Offset + WriteLen)
        {
            Map->ValidDataLength.QuadPart =
                min(Block->Offset + WriteLen, FileEnd);
        }
    }

    Block->Busy = FALSE;
    *Written = Got;
    return Status;
}

/* WAIT/RETRY ***************************************************************/

static VOID
NxcShortWait(VOID)
{
    LARGE_INTEGER Interval = { .QuadPart = -10000 };    /* 1 ms */
    ExReleaseFastMutex(&NxcMutex);
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    ExAcquireFastMutex(&NxcMutex);
}

/* BLOCK LOOKUP / EVICTION **************************************************/

static NXC_BLOCK *
NxcLookupBlock(
    IN NXC_MAP *Map,
    IN LONGLONG Offset)
{
    for (ULONG i = 0; i < NXC_TOTAL_BLOCKS; i++)
    {
        if (NxcBlocks[i].Map == Map && NxcBlocks[i].Offset == Offset &&
            NxcBlocks[i].Data != NULL)
            return &NxcBlocks[i];
    }
    return NULL;
}

/* Claim a write-back reserve slot (a free one, else a clean unreferenced
 * reserve block).  Returns NULL if none is available.  Reserve blocks are
 * handed out only here, and only to the re-entrant metadata read of a
 * write-back, so the data-fill path can never exhaust them (roswell#26). */
static NXC_BLOCK *
NxcClaimReserve(VOID)
{
    for (ULONG i = NXC_MAX_BLOCKS; i < NXC_TOTAL_BLOCKS; i++)
    {
        if (NxcBlocks[i].Data != NULL && NxcBlocks[i].Map == NULL)
        {
            NxcBlocks[i].RefCount = 1;
            return &NxcBlocks[i];
        }
    }

    for (ULONG i = NXC_MAX_BLOCKS; i < NXC_TOTAL_BLOCKS; i++)
    {
        NXC_BLOCK *B = &NxcBlocks[i];
        if (B->Data == NULL || B->RefCount != 0 || B->Busy || B->Dirty)
            continue;
        RemoveEntryList(&B->LruLink);
        B->Map = NULL;
        B->Valid = FALSE;
        B->RefCount = 1;
        return B;
    }

    return NULL;
}

/* Find a reusable slot: a free one, else the least-recently-used clean
 * unreferenced block, else flush the LRU dirty unreferenced block.
 * Returns NULL only when !Wait.  Mutex held throughout (the dirty case
 * drops it inside NxcFlushBlock). */
static NXC_BLOCK *
NxcClaimSlot(
    IN BOOLEAN Wait)
{
    ULONG Spins = 0;

    for (;;)
    {
        PLIST_ENTRY Entry;
        NXC_BLOCK *Victim = NULL;

        for (ULONG i = 0; i < NXC_MAX_BLOCKS; i++)
        {
            if (NxcBlocks[i].Data != NULL && NxcBlocks[i].Map == NULL)
            {
                NxcBlocks[i].RefCount = 1;
                return &NxcBlocks[i];
            }
        }

        /* Clean-first LRU scan.  Reserve blocks are never taken here -- they
         * stay available for the write-back metadata read below. */
        for (Entry = NxcLruList.Flink; Entry != &NxcLruList; Entry = Entry->Flink)
        {
            NXC_BLOCK *B = CONTAINING_RECORD(Entry, NXC_BLOCK, LruLink);
            if (B->Reserved || B->RefCount != 0 || B->Busy) continue;
            if (!B->Dirty)
            {
                Victim = B;
                break;
            }
            if (Victim == NULL) Victim = B;     /* oldest dirty fallback */
        }

        if (Victim != NULL)
        {
            if (Victim->Dirty)
            {
                SIZE_T Written;
                Victim->RefCount++;
                Victim->Busy = TRUE;
                (void)NxcFlushBlock(Victim, &Written);  /* drops/retakes mutex */
                Victim->RefCount--;
                /* State changed while unlocked; rescan. */
                continue;
            }

            RemoveEntryList(&Victim->LruLink);
            Victim->Map = NULL;
            Victim->Valid = FALSE;
            Victim->RefCount = 1;
            return Victim;
        }

        /* Every title-visible block is referenced or busy.  If this is the
         * metadata read of an in-flight write-back, take a reserve block so
         * the flush can make progress instead of deadlocking on itself. */
        if (NxcWbDepth > 0)
        {
            NXC_BLOCK *Block = NxcClaimReserve();
            if (Block != NULL) return Block;
        }

        if (!Wait) return NULL;
        if ((++Spins % 1024) == 0)
            DPRINT1("all %lu blocks pinned, still waiting\n",
                    NxcActiveBlocks);
        NxcShortWait();
    }
}

/* Get a referenced, Valid block for (Map, Offset).  Mutex held on entry
 * and exit.  Returns NULL when !Wait would block, raises on IO error. */
static NXC_BLOCK *
NxcGetBlock(
    IN NXC_MAP *Map,
    IN LONGLONG Offset,
    IN BOOLEAN Wait)
{
    NXC_BLOCK *Block;
    NTSTATUS Status;

    ASSERT((Offset & (NXC_BLOCK_SIZE - 1)) == 0);

    for (;;)
    {
        Block = NxcLookupBlock(Map, Offset);
        if (Block != NULL)
        {
            if (Block->Busy)
            {
                if (!Wait) return NULL;
                NxcShortWait();
                continue;
            }
            Block->RefCount++;
            if (Block->Valid)
            {
                /* Touch in LRU. */
                RemoveEntryList(&Block->LruLink);
                InsertTailList(&NxcLruList, &Block->LruLink);
                return Block;
            }
            /* Invalid non-busy block (failed earlier read): retry it. */
        }
        else
        {
            if (!Wait) return NULL;
            Block = NxcClaimSlot(Wait);
            if (Block == NULL) return NULL;
            /* The claim rescan may have raced us in. */
            if (NxcLookupBlock(Map, Offset) != NULL)
            {
                Block->Map = NULL;
                Block->RefCount = 0;
                continue;
            }
            Block->Map = Map;
            Block->Offset = Offset;
            Block->Valid = FALSE;
            Block->Dirty = FALSE;
            InsertTailList(&NxcLruList, &Block->LruLink);
        }

        if (!Wait)
        {
            Block->RefCount--;
            return NULL;
        }

        Block->Busy = TRUE;
        Status = NxcReadBlock(Block);           /* drops/retakes mutex */
        if (!NT_SUCCESS(Status))
        {
            Block->RefCount--;
            ExReleaseFastMutex(&NxcMutex);
            ExRaiseStatus(Status);
        }
        return Block;
    }
}

static VOID
NxcPutBlock(
    IN NXC_BLOCK *Block)
{
    ASSERT(Block->RefCount > 0);
    Block->RefCount--;
}

/* MAP LIFETIME *************************************************************/

VOID
NTAPI
CcInitializeCacheMap(
    IN PFILE_OBJECT FileObject,
    IN PCC_FILE_SIZES FileSizes,
    IN BOOLEAN PinAccess,
    IN PCACHE_MANAGER_CALLBACKS Callbacks,
    IN PVOID LazyWriteContext)
{
    NXC_MAP *Map;
    PPRIVATE_CACHE_MAP PrivateMap;

    ASSERT(FileObject);
    ASSERT(FileSizes);

    ExAcquireFastMutex(&NxcMutex);

    Map = FileObject->SectionObjectPointer->SharedCacheMap;
    if (Map == NULL)
    {
        Map = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Map), NXC_TAG);
        if (Map == NULL)
        {
            ExReleaseFastMutex(&NxcMutex);
            ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
        }
        RtlZeroMemory(Map, sizeof(*Map));
        Map->NodeTypeCode = NXC_NODE_SHARED_MAP;
        Map->NodeByteSize = sizeof(*Map);
        Map->FileObject = FileObject;
        Map->Callbacks = Callbacks;
        Map->LazyWriteContext = LazyWriteContext;
        Map->AllocationSize = FileSizes->AllocationSize;
        Map->FileSize = FileSizes->FileSize;
        Map->ValidDataLength = FileSizes->ValidDataLength;
        Map->PinAccess = PinAccess;
        InitializeListHead(&Map->PrivateList);

        ObReferenceObjectByPointer(FileObject, FILE_ALL_ACCESS, NULL,
                                   KernelMode);
        FileObject->SectionObjectPointer->SharedCacheMap = Map;
        InsertTailList(&NxcMapList, &Map->MapLinks);
    }

    if (FileObject->PrivateCacheMap == NULL)
    {
        if (Map->EmbeddedPrivateMap.NodeTypeCode == 0)
        {
            PrivateMap = &Map->EmbeddedPrivateMap;
        }
        else
        {
            PrivateMap = ExAllocatePoolWithTag(NonPagedPool,
                                               sizeof(*PrivateMap), NXC_TAG);
            if (PrivateMap == NULL)
            {
                ExReleaseFastMutex(&NxcMutex);
                ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
            }
        }
        RtlZeroMemory(PrivateMap, sizeof(*PrivateMap));
        PrivateMap->NodeTypeCode = NXC_NODE_PRIVATE_MAP;
        PrivateMap->FileObject = FileObject;
        PrivateMap->ReadAheadMask = PAGE_SIZE - 1;
        KeInitializeSpinLock(&PrivateMap->ReadAheadSpinLock);
        InsertTailList(&Map->PrivateList, &PrivateMap->PrivateLinks);
        FileObject->PrivateCacheMap = PrivateMap;
        Map->OpenCount++;
    }

    ExReleaseFastMutex(&NxcMutex);
}

/* Flush + drop every block of a map.  Mutex held; IO drops it. */
static VOID
NxcFlushMap(
    IN NXC_MAP *Map,
    IN LONGLONG Start,
    IN LONGLONG End,
    IN BOOLEAN Discard,
    OUT PIO_STATUS_BLOCK IoStatus OPTIONAL)
{
    NTSTATUS Status = STATUS_SUCCESS;
    SIZE_T Total = 0;

restart:
    for (ULONG i = 0; i < NXC_TOTAL_BLOCKS; i++)
    {
        NXC_BLOCK *Block = &NxcBlocks[i];
        SIZE_T Written;

        if (Block->Map != Map || Block->Data == NULL) continue;
        if (Block->Offset + NXC_BLOCK_SIZE <= Start || Block->Offset >= End)
            continue;

        if (Block->Busy)
        {
            NxcShortWait();
            goto restart;
        }

        if (Block->Dirty && Block->Valid && !Discard)
        {
            NTSTATUS BlockStatus;
            Block->RefCount++;
            Block->Busy = TRUE;
            BlockStatus = NxcFlushBlock(Block, &Written);   /* unlocks */
            Block->RefCount--;
            if (!NT_SUCCESS(BlockStatus)) Status = BlockStatus;
            Total += Written;
            goto restart;       /* table may have changed while unlocked */
        }

        if (Discard && Block->RefCount == 0)
        {
            RemoveEntryList(&Block->LruLink);
            Block->Map = NULL;
            Block->Valid = FALSE;
            Block->Dirty = FALSE;
        }
    }

    if (IoStatus)
    {
        IoStatus->Status = Status;
        IoStatus->Information = Total;
    }
}

BOOLEAN
NTAPI
CcUninitializeCacheMap(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER TruncateSize OPTIONAL,
    IN PCACHE_UNINITIALIZE_EVENT UninitializeCompleteEvent OPTIONAL)
{
    NXC_MAP *Map;
    PPRIVATE_CACHE_MAP PrivateMap;

    ExAcquireFastMutex(&NxcMutex);

    Map = FileObject->SectionObjectPointer->SharedCacheMap;
    if (Map != NULL)
    {
        if (TruncateSize != NULL)
        {
            if (Map->FileSize.QuadPart > TruncateSize->QuadPart)
                Map->FileSize = *TruncateSize;
            /* Discard blocks fully beyond the truncation point. */
            NxcFlushMap(Map,
                        ROUND_UP(TruncateSize->QuadPart, NXC_BLOCK_SIZE),
                        MAXLONGLONG, TRUE, NULL);
        }

        PrivateMap = FileObject->PrivateCacheMap;
        if (PrivateMap != NULL)
        {
            RemoveEntryList(&PrivateMap->PrivateLinks);
            FileObject->PrivateCacheMap = NULL;
            if (PrivateMap == &Map->EmbeddedPrivateMap)
                PrivateMap->NodeTypeCode = 0;
            else
                ExFreePoolWithTag(PrivateMap, NXC_TAG);

            ASSERT(Map->OpenCount > 0);
            Map->OpenCount--;
            if (Map->OpenCount == 0)
            {
                PFILE_OBJECT MapFileObject = Map->FileObject;

                /* Last handle: write everything back, then drop the map. */
                NxcFlushMap(Map, 0, MAXLONGLONG, FALSE, NULL);
                NxcFlushMap(Map, 0, MAXLONGLONG, TRUE, NULL);
                RemoveEntryList(&Map->MapLinks);
                MapFileObject->SectionObjectPointer->SharedCacheMap = NULL;
                ExFreePoolWithTag(Map, NXC_TAG);

                ExReleaseFastMutex(&NxcMutex);
                ObDereferenceObject(MapFileObject);
                ExAcquireFastMutex(&NxcMutex);
            }
        }
    }

    ExReleaseFastMutex(&NxcMutex);

    if (UninitializeCompleteEvent)
        KeSetEvent(&UninitializeCompleteEvent->Event, IO_NO_INCREMENT, FALSE);
    return TRUE;
}

VOID
NTAPI
CcSetFileSizes(
    IN PFILE_OBJECT FileObject,
    IN PCC_FILE_SIZES FileSizes)
{
    NXC_MAP *Map;
    LARGE_INTEGER OldAllocation;

    ExAcquireFastMutex(&NxcMutex);
    Map = FileObject->SectionObjectPointer->SharedCacheMap;
    if (Map == NULL)
    {
        ExReleaseFastMutex(&NxcMutex);
        return;
    }

    OldAllocation = Map->AllocationSize;
    Map->AllocationSize = FileSizes->AllocationSize;
    Map->FileSize = FileSizes->FileSize;
    Map->ValidDataLength = FileSizes->ValidDataLength;

    if (FileSizes->AllocationSize.QuadPart < OldAllocation.QuadPart)
    {
        /* Shrink: drop blocks past the new allocation. */
        NxcFlushMap(Map,
                    ROUND_UP(FileSizes->AllocationSize.QuadPart, NXC_BLOCK_SIZE),
                    MAXLONGLONG, TRUE, NULL);
    }
    ExReleaseFastMutex(&NxcMutex);
}

BOOLEAN
NTAPI
CcGetFileSizes(
    IN PFILE_OBJECT FileObject,
    IN PCC_FILE_SIZES FileSizes)
{
    NXC_MAP *Map;
    BOOLEAN Result = FALSE;

    ExAcquireFastMutex(&NxcMutex);
    Map = FileObject->SectionObjectPointer->SharedCacheMap;
    if (Map != NULL)
    {
        FileSizes->AllocationSize = Map->AllocationSize;
        FileSizes->FileSize = Map->FileSize;
        FileSizes->ValidDataLength = Map->FileSize;
        Result = TRUE;
    }
    ExReleaseFastMutex(&NxcMutex);
    return Result;
}

BOOLEAN
NTAPI
CcPurgeCacheSection(
    IN PSECTION_OBJECT_POINTERS SectionObjectPointer,
    IN PLARGE_INTEGER FileOffset OPTIONAL,
    IN ULONG Length,
    IN BOOLEAN UninitializeCacheMaps)
{
    NXC_MAP *Map;
    LONGLONG Start, End;
    BOOLEAN Success = TRUE;

    ExAcquireFastMutex(&NxcMutex);
    Map = SectionObjectPointer->SharedCacheMap;
    if (Map == NULL)
    {
        ExReleaseFastMutex(&NxcMutex);
        return TRUE;
    }

    if (UninitializeCacheMaps)
    {
        while (!IsListEmpty(&Map->PrivateList))
        {
            PPRIVATE_CACHE_MAP PrivateMap =
                CONTAINING_RECORD(Map->PrivateList.Flink, PRIVATE_CACHE_MAP,
                                  PrivateLinks);
            ExReleaseFastMutex(&NxcMutex);
            CcUninitializeCacheMap(PrivateMap->FileObject, NULL, NULL);
            ExAcquireFastMutex(&NxcMutex);
            /* The map may be gone now. */
            if (SectionObjectPointer->SharedCacheMap != Map)
            {
                ExReleaseFastMutex(&NxcMutex);
                return TRUE;
            }
        }
    }

    Start = FileOffset != NULL ? FileOffset->QuadPart : 0;
    End = (Length == 0 || FileOffset == NULL) ? MAXLONGLONG
                                              : Start + Length;

    /* Purge fails if anything in range is referenced. */
    for (ULONG i = 0; i < NXC_TOTAL_BLOCKS; i++)
    {
        NXC_BLOCK *Block = &NxcBlocks[i];
        if (Block->Map != Map || Block->Data == NULL) continue;
        if (Block->Offset + NXC_BLOCK_SIZE <= Start || Block->Offset >= End)
            continue;
        if (Block->RefCount != 0 || Block->Busy)
        {
            Success = FALSE;
            break;
        }
    }

    if (Success)
        NxcFlushMap(Map, Start, End, TRUE, NULL);

    ExReleaseFastMutex(&NxcMutex);
    return Success;
}

/* COPY INTERFACE ***********************************************************/

BOOLEAN
NTAPI
CcCopyRead(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN BOOLEAN Wait,
    OUT PVOID Buffer,
    OUT PIO_STATUS_BLOCK IoStatus)
{
    NXC_MAP *Map = FileObject->SectionObjectPointer->SharedCacheMap;
    LONGLONG Current = FileOffset->QuadPart;
    PUCHAR Out = Buffer;
    ULONG Done = 0;

    if (Map == NULL) return FALSE;

    ExAcquireFastMutex(&NxcMutex);
    while (Done < Length)
    {
        LONGLONG BlockOffset = Current & ~(LONGLONG)(NXC_BLOCK_SIZE - 1);
        ULONG At = (ULONG)(Current - BlockOffset);
        ULONG Chunk = min(Length - Done, NXC_BLOCK_SIZE - At);
        NXC_BLOCK *Block = NxcGetBlock(Map, BlockOffset, Wait);

        if (Block == NULL)
        {
            ExReleaseFastMutex(&NxcMutex);
            return FALSE;
        }

        _SEH2_TRY
        {
            RtlCopyMemory(Out, Block->Data + At, Chunk);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            NxcPutBlock(Block);
            ExReleaseFastMutex(&NxcMutex);
            ExRaiseStatus(STATUS_INVALID_USER_BUFFER);
        }
        _SEH2_END;
        NxcPutBlock(Block);

        Out += Chunk;
        Current += Chunk;
        Done += Chunk;
    }
    ExReleaseFastMutex(&NxcMutex);

    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = Done;
    return TRUE;
}

BOOLEAN
NTAPI
CcCopyWrite(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN BOOLEAN Wait,
    IN PVOID Buffer)
{
    NXC_MAP *Map = FileObject->SectionObjectPointer->SharedCacheMap;
    LONGLONG Current = FileOffset->QuadPart;
    PUCHAR In = Buffer;
    ULONG Done = 0;

    if (Map == NULL) return FALSE;

    ASSERT(FileOffset->QuadPart + Length <= Map->AllocationSize.QuadPart);

    ExAcquireFastMutex(&NxcMutex);
    while (Done < Length)
    {
        LONGLONG BlockOffset = Current & ~(LONGLONG)(NXC_BLOCK_SIZE - 1);
        ULONG At = (ULONG)(Current - BlockOffset);
        ULONG Chunk = min(Length - Done, NXC_BLOCK_SIZE - At);
        NXC_BLOCK *Block = NxcGetBlock(Map, BlockOffset, Wait);

        if (Block == NULL)
        {
            ExReleaseFastMutex(&NxcMutex);
            return FALSE;
        }

        _SEH2_TRY
        {
            RtlCopyMemory(Block->Data + At, In, Chunk);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            NxcPutBlock(Block);
            ExReleaseFastMutex(&NxcMutex);
            ExRaiseStatus(STATUS_INVALID_USER_BUFFER);
        }
        _SEH2_END;
        Block->Dirty = TRUE;
        NxcPutBlock(Block);

        In += Chunk;
        Current += Chunk;
        Done += Chunk;
    }
    ExReleaseFastMutex(&NxcMutex);

    if (FileObject->Flags & FO_WRITE_THROUGH)
        CcFlushCache(FileObject->SectionObjectPointer, FileOffset, Length, NULL);

    return TRUE;
}

VOID
NTAPI
CcFastCopyRead(
    IN PFILE_OBJECT FileObject,
    IN ULONG FileOffset,
    IN ULONG Length,
    IN ULONG PageCount,
    OUT PVOID Buffer,
    OUT PIO_STATUS_BLOCK IoStatus)
{
    LARGE_INTEGER Offset;

    UNREFERENCED_PARAMETER(PageCount);
    Offset.QuadPart = FileOffset;
    if (!CcCopyRead(FileObject, &Offset, Length, TRUE, Buffer, IoStatus))
        ExRaiseStatus(STATUS_UNSUCCESSFUL);
}

VOID
NTAPI
CcFastCopyWrite(
    IN PFILE_OBJECT FileObject,
    IN ULONG FileOffset,
    IN ULONG Length,
    IN PVOID Buffer)
{
    LARGE_INTEGER Offset;

    Offset.QuadPart = FileOffset;
    if (!CcCopyWrite(FileObject, &Offset, Length, TRUE, Buffer))
        ExRaiseStatus(STATUS_UNSUCCESSFUL);
}

BOOLEAN
NTAPI
CcCanIWrite(
    IN PFILE_OBJECT FileObject,
    IN ULONG BytesToWrite,
    IN BOOLEAN Wait,
    IN BOOLEAN Retrying)
{
    /* No paging, no lazy writer, no throttle. */
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(BytesToWrite);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(Retrying);
    return TRUE;
}

VOID
NTAPI
CcDeferWrite(
    IN PFILE_OBJECT FileObject,
    IN PCC_POST_DEFERRED_WRITE PostRoutine,
    IN PVOID Context1,
    IN PVOID Context2,
    IN ULONG BytesToWrite,
    IN BOOLEAN Retrying)
{
    /* CcCanIWrite never defers; dispatch immediately if reached. */
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(BytesToWrite);
    UNREFERENCED_PARAMETER(Retrying);
    PostRoutine(Context1, Context2);
}

BOOLEAN
NTAPI
CcZeroData(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER StartOffset,
    IN PLARGE_INTEGER EndOffset,
    IN BOOLEAN Wait)
{
    NXC_MAP *Map = FileObject->SectionObjectPointer->SharedCacheMap;
    LONGLONG Current = StartOffset->QuadPart;
    LONGLONG End = EndOffset->QuadPart;

    if (Map == NULL)
    {
        /* Non-cached: synchronous paging writes from the shared zero
         * page (every MDL PFN points at it). */
        IO_STATUS_BLOCK Iosb;
        KEVENT Event;
        PMDL Mdl;
        LARGE_INTEGER WriteOffset;
        LONGLONG Length = End - Current;
        PPFN_NUMBER PfnArray;
        NTSTATUS Status;

        if (Length <= 0) return TRUE;

        Mdl = IoAllocateMdl(NULL, (ULONG)min(Length, NXC_BLOCK_SIZE),
                            FALSE, FALSE, NULL);
        if (Mdl == NULL) ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);

        PfnArray = MmGetMdlPfnArray(Mdl);
        for (ULONG i = 0; i < BYTES_TO_PAGES(Mdl->ByteCount); i++)
            PfnArray[i] = NxcZeroPagePfn;
        Mdl->MdlFlags |= MDL_PAGES_LOCKED;

        WriteOffset.QuadPart = Current;
        while (Length > 0)
        {
            ULONG Chunk = (ULONG)min(Length, NXC_BLOCK_SIZE);
            Mdl->ByteCount = Chunk;
            KeInitializeEvent(&Event, NotificationEvent, FALSE);
            Status = IoSynchronousPageWrite(FileObject, Mdl, &WriteOffset,
                                            &Event, &Iosb);
            if (Status == STATUS_PENDING)
            {
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE,
                                      NULL);
                Status = Iosb.Status;
            }
            if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA)
                MmUnmapLockedPages(Mdl->MappedSystemVa, Mdl);
            if (!NT_SUCCESS(Status))
            {
                IoFreeMdl(Mdl);
                ExRaiseStatus(Status);
            }
            WriteOffset.QuadPart += Chunk;
            Length -= Chunk;
        }
        IoFreeMdl(Mdl);
        return TRUE;
    }

    ExAcquireFastMutex(&NxcMutex);
    while (Current < End)
    {
        LONGLONG BlockOffset = Current & ~(LONGLONG)(NXC_BLOCK_SIZE - 1);
        ULONG At = (ULONG)(Current - BlockOffset);
        ULONG Chunk = (ULONG)min(End - Current, NXC_BLOCK_SIZE - At);
        NXC_BLOCK *Block = NxcGetBlock(Map, BlockOffset, Wait);

        if (Block == NULL)
        {
            ExReleaseFastMutex(&NxcMutex);
            return FALSE;
        }

        RtlZeroMemory(Block->Data + At, Chunk);
        Block->Dirty = TRUE;
        NxcPutBlock(Block);
        Current += Chunk;
    }
    ExReleaseFastMutex(&NxcMutex);

    if (FileObject->Flags & FO_WRITE_THROUGH)
    {
        CcFlushCache(FileObject->SectionObjectPointer, StartOffset,
                     (ULONG)(End - StartOffset->QuadPart), NULL);
    }
    return TRUE;
}

/* PIN INTERFACE ************************************************************/

static BOOLEAN
NxcPinCommon(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN BOOLEAN Wait,
    IN BOOLEAN Pin,
    OUT PVOID *pBcb,
    OUT PVOID *pBuffer)
{
    NXC_MAP *Map = FileObject->SectionObjectPointer->SharedCacheMap;
    LONGLONG BlockOffset;
    ULONG At;
    NXC_BLOCK *Block;
    NXC_BCB *Bcb;

    ASSERT(Map != NULL);

    BlockOffset = FileOffset->QuadPart & ~(LONGLONG)(NXC_BLOCK_SIZE - 1);
    At = (ULONG)(FileOffset->QuadPart - BlockOffset);

    if (At + Length > NXC_BLOCK_SIZE)
    {
        /* No reachable caller spans blocks (see the block-size note);
         * truncating would walk the caller off its buffer, so fail
         * loudly into the caller's SEH instead. */
        XbDbg("cross-block map/pin (off=%I64x len=%lu ra=%p ra2=%p)\n",
              FileOffset->QuadPart, Length,
              __builtin_return_address(0), __builtin_return_address(1));
        ExRaiseStatus(STATUS_INVALID_PARAMETER);
    }

    Bcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Bcb), NXC_TAG);
    if (Bcb == NULL)
    {
        *pBcb = NULL;
        ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    }

    ExAcquireFastMutex(&NxcMutex);
    Block = NxcGetBlock(Map, BlockOffset, Wait);
    ExReleaseFastMutex(&NxcMutex);

    if (Block == NULL)
    {
        ExFreePoolWithTag(Bcb, NXC_TAG);
        *pBcb = NULL;
        *pBuffer = NULL;
        return FALSE;
    }

    /* The block stays referenced until the BCB is fully released. */
    RtlZeroMemory(Bcb, sizeof(*Bcb));
    Bcb->PFCB.NodeTypeCode = NXC_NODE_BCB;
    Bcb->PFCB.NodeByteSize = 0;
    Bcb->PFCB.MappedLength = Length;
    Bcb->PFCB.MappedFileOffset = *FileOffset;
    Bcb->Block = Block;
    Bcb->Map = Map;
    Bcb->RefCount = 1;
    Bcb->PinCount = Pin ? 1 : 0;

    *pBcb = &Bcb->PFCB;
    *pBuffer = Block->Data + At;
    return TRUE;
}

BOOLEAN
NTAPI
CcMapData(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN ULONG Flags,
    OUT PVOID *pBcb,
    OUT PVOID *pBuffer)
{
    if (Flags & MAP_WAIT) ++CcMapDataWait; else ++CcMapDataNoWait;
    return NxcPinCommon(FileObject, FileOffset, Length,
                        BooleanFlagOn(Flags, MAP_WAIT), FALSE, pBcb, pBuffer);
}

BOOLEAN
NTAPI
CcPinRead(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN ULONG Flags,
    OUT PVOID *pBcb,
    OUT PVOID *pBuffer)
{
    NXC_MAP *Map = FileObject->SectionObjectPointer->SharedCacheMap;

    ASSERT(Map != NULL);
    if (!Map->PinAccess)
    {
        DPRINT1("pinning a file with no pin access\n");
        return FALSE;
    }

    if (Flags & PIN_WAIT) ++CcPinReadWait; else ++CcPinReadNoWait;
    return NxcPinCommon(FileObject, FileOffset, Length,
                        BooleanFlagOn(Flags, PIN_WAIT), TRUE, pBcb, pBuffer);
}

BOOLEAN
NTAPI
CcPinMappedData(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN ULONG Flags,
    IN OUT PVOID *Bcb)
{
    PVOID Buffer;
    PVOID OldBcb = *Bcb;
    BOOLEAN Result;

    ++CcPinMappedDataCount;

    Result = CcPinRead(FileObject, FileOffset, Length, Flags, Bcb, &Buffer);
    if (Result && OldBcb != NULL)
        CcUnpinData(OldBcb);
    return Result;
}

BOOLEAN
NTAPI
CcPreparePinWrite(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN BOOLEAN Zero,
    IN ULONG Flags,
    OUT PVOID *pBcb,
    OUT PVOID *pBuffer)
{
    BOOLEAN Result;

    Result = CcPinRead(FileObject, FileOffset, Length, Flags, pBcb, pBuffer);
    if (Result && Zero)
        RtlZeroMemory(*pBuffer, Length);
    return Result;
}

VOID
NTAPI
CcSetDirtyPinnedData(
    IN PVOID Bcb,
    IN PLARGE_INTEGER Lsn)
{
    NXC_BCB *iBcb = CONTAINING_RECORD(Bcb, NXC_BCB, PFCB);

    UNREFERENCED_PARAMETER(Lsn);
    iBcb->Block->Dirty = TRUE;
}

VOID
NTAPI
CcUnpinData(
    IN PVOID Bcb)
{
    NXC_BCB *iBcb = CONTAINING_RECORD(Bcb, NXC_BCB, PFCB);

    if (iBcb->PinCount > 0)
        iBcb->PinCount--;

    if (InterlockedDecrement(&iBcb->RefCount) == 0)
    {
        ExAcquireFastMutex(&NxcMutex);
        NxcPutBlock(iBcb->Block);
        ExReleaseFastMutex(&NxcMutex);
        ExFreePoolWithTag(iBcb, NXC_TAG);
    }
}

VOID
NTAPI
CcUnpinDataForThread(
    IN PVOID Bcb,
    IN ERESOURCE_THREAD ResourceThreadId)
{
    UNREFERENCED_PARAMETER(ResourceThreadId);
    CcUnpinData(Bcb);
}

VOID
NTAPI
CcRepinBcb(
    IN PVOID Bcb)
{
    NXC_BCB *iBcb = CONTAINING_RECORD(Bcb, NXC_BCB, PFCB);
    InterlockedIncrement(&iBcb->RefCount);
}

VOID
NTAPI
CcUnpinRepinnedBcb(
    IN PVOID Bcb,
    IN BOOLEAN WriteThrough,
    IN PIO_STATUS_BLOCK IoStatus)
{
    NXC_BCB *iBcb = CONTAINING_RECORD(Bcb, NXC_BCB, PFCB);

    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = 0;

    if (WriteThrough)
    {
        CcFlushCache(iBcb->Map->FileObject->SectionObjectPointer,
                     &iBcb->PFCB.MappedFileOffset,
                     iBcb->PFCB.MappedLength,
                     IoStatus);
    }

    CcUnpinData(Bcb);
}

VOID
NTAPI
CcSetBcbOwnerPointer(
    IN PVOID Bcb,
    IN PVOID OwnerPointer)
{
    UNREFERENCED_PARAMETER(Bcb);
    UNREFERENCED_PARAMETER(OwnerPointer);
}

PFILE_OBJECT
NTAPI
CcGetFileObjectFromBcb(
    IN PVOID Bcb)
{
    NXC_BCB *iBcb = CONTAINING_RECORD(Bcb, NXC_BCB, PFCB);
    return iBcb->Map->FileObject;
}

/* FLUSH INTERFACE **********************************************************/

VOID
NTAPI
CcFlushCache(
    IN PSECTION_OBJECT_POINTERS SectionObjectPointers,
    IN PLARGE_INTEGER FileOffset OPTIONAL,
    IN ULONG Length,
    OUT PIO_STATUS_BLOCK IoStatus OPTIONAL)
{
    NXC_MAP *Map;
    LONGLONG Start, End;

    if (IoStatus)
    {
        IoStatus->Status = STATUS_SUCCESS;
        IoStatus->Information = 0;
    }
    if (SectionObjectPointers == NULL)
    {
        if (IoStatus) IoStatus->Status = STATUS_INVALID_PARAMETER;
        return;
    }

    ExAcquireFastMutex(&NxcMutex);
    Map = SectionObjectPointers->SharedCacheMap;
    if (Map != NULL)
    {
        Start = FileOffset != NULL ? FileOffset->QuadPart : 0;
        End = FileOffset != NULL ? Start + Length : MAXLONGLONG;
        NxcFlushMap(Map, Start, End, FALSE, IoStatus);
    }
    ExReleaseFastMutex(&NxcMutex);
}

BOOLEAN
NTAPI
CcIsThereDirtyData(
    IN PVPB Vpb)
{
    BOOLEAN Dirty = FALSE;

    ExAcquireFastMutex(&NxcMutex);
    for (ULONG i = 0; i < NXC_TOTAL_BLOCKS && !Dirty; i++)
    {
        NXC_BLOCK *Block = &NxcBlocks[i];
        if (Block->Map == NULL || Block->Data == NULL || !Block->Dirty)
            continue;
        if (Block->Map->FileObject->Vpb != Vpb) continue;
        if (BooleanFlagOn(Block->Map->FileObject->Flags, FO_TEMPORARY_FILE))
            continue;
        Dirty = TRUE;
    }
    ExReleaseFastMutex(&NxcMutex);
    return Dirty;
}

/* Flush absolutely everything (shutdown, power-down, lazy-writer waits). */
static VOID
NxcFlushAll(VOID)
{
    PLIST_ENTRY Entry;

    ExAcquireFastMutex(&NxcMutex);
restart:
    for (Entry = NxcMapList.Flink; Entry != &NxcMapList; Entry = Entry->Flink)
    {
        NXC_MAP *Map = CONTAINING_RECORD(Entry, NXC_MAP, MapLinks);
        for (ULONG i = 0; i < NXC_TOTAL_BLOCKS; i++)
        {
            if (NxcBlocks[i].Map == Map && NxcBlocks[i].Dirty)
            {
                NxcFlushMap(Map, 0, MAXLONGLONG, FALSE, NULL);
                goto restart;   /* list may have changed while unlocked */
            }
        }
    }
    ExReleaseFastMutex(&NxcMutex);
}

NTSTATUS
NTAPI
CcWaitForCurrentLazyWriterActivity(VOID)
{
    /* No lazy writer: the equivalent guarantee is everything flushed. */
    NxcFlushAll();
    return STATUS_SUCCESS;
}

VOID
NTAPI
CcShutdownSystem(VOID)
{
    NxcFlushAll();
}

/* po/power.c shutdown path (legacy cc/ entry point). */
NTSTATUS
CcRosFlushDirtyPages(
    ULONG Target,
    PULONG Count,
    BOOLEAN Wait,
    BOOLEAN CalledFromLazy)
{
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(CalledFromLazy);
    NxcFlushAll();
    if (Count) *Count = 0;
    return STATUS_SUCCESS;
}

LARGE_INTEGER
NTAPI
CcGetFlushedValidData(
    IN PSECTION_OBJECT_POINTERS SectionObjectPointer,
    IN BOOLEAN BcbListHeld)
{
    NXC_MAP *Map;
    LARGE_INTEGER Result = { .QuadPart = 0 };

    UNREFERENCED_PARAMETER(BcbListHeld);

    ExAcquireFastMutex(&NxcMutex);
    Map = SectionObjectPointer->SharedCacheMap;
    if (Map != NULL) Result = Map->ValidDataLength;
    ExReleaseFastMutex(&NxcMutex);
    return Result;
}

PFILE_OBJECT
NTAPI
CcGetFileObjectFromSectionPtrs(
    IN PSECTION_OBJECT_POINTERS SectionObjectPointer)
{
    NXC_MAP *Map = SectionObjectPointer->SharedCacheMap;
    return Map != NULL ? Map->FileObject : NULL;
}

/* MDL INTERFACE (no consumer issues MDL IO on this machine) ****************/

VOID
NTAPI
CcMdlRead(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    OUT PMDL *MdlChain,
    OUT PIO_STATUS_BLOCK IoStatus)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(MdlChain);
    UNREFERENCED_PARAMETER(IoStatus);
    UNIMPLEMENTED;
    ExRaiseStatus(STATUS_NOT_IMPLEMENTED);
}

VOID
NTAPI
CcMdlReadComplete(
    IN PFILE_OBJECT FileObject,
    IN PMDL MdlChain)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(MdlChain);
    UNIMPLEMENTED;
}

VOID
NTAPI
CcMdlReadComplete2(
    IN PFILE_OBJECT FileObject,
    IN PMDL MdlChain)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(MdlChain);
    UNIMPLEMENTED;
}

VOID
NTAPI
CcPrepareMdlWrite(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    OUT PMDL *MdlChain,
    OUT PIO_STATUS_BLOCK IoStatus)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(MdlChain);
    UNREFERENCED_PARAMETER(IoStatus);
    UNIMPLEMENTED;
    ExRaiseStatus(STATUS_NOT_IMPLEMENTED);
}

VOID
NTAPI
CcMdlWriteComplete(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PMDL MdlChain)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(MdlChain);
    UNIMPLEMENTED;
}

VOID
NTAPI
CcMdlWriteComplete2(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PMDL MdlChain)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(MdlChain);
    UNIMPLEMENTED;
}

VOID
NTAPI
CcMdlWriteAbort(
    IN PFILE_OBJECT FileObject,
    IN PMDL MdlChain)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(MdlChain);
    UNIMPLEMENTED;
}

/* MISC STUBS ***************************************************************/

VOID
NTAPI
CcScheduleReadAhead(
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length)
{
    /* No read-ahead worker. */
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
}

VOID
NTAPI
CcSetReadAheadGranularity(
    IN PFILE_OBJECT FileObject,
    IN ULONG Granularity)
{
    PPRIVATE_CACHE_MAP PrivateMap = FileObject->PrivateCacheMap;
    if (PrivateMap != NULL) PrivateMap->ReadAheadMask = Granularity - 1;
}

VOID
NTAPI
CcSetDirtyPageThreshold(
    IN PFILE_OBJECT FileObject,
    IN ULONG DirtyPageThreshold)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(DirtyPageThreshold);
}

VOID
NTAPI
CcSetAdditionalCacheAttributes(
    IN PFILE_OBJECT FileObject,
    IN BOOLEAN DisableReadAhead,
    IN BOOLEAN DisableWriteBehind)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(DisableReadAhead);
    UNREFERENCED_PARAMETER(DisableWriteBehind);
}

VOID
NTAPI
CcSetLogHandleForFile(
    IN PFILE_OBJECT FileObject,
    IN PVOID LogHandle,
    IN PFLUSH_TO_LSN FlushToLsnRoutine)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(LogHandle);
    UNREFERENCED_PARAMETER(FlushToLsnRoutine);
}

LARGE_INTEGER
NTAPI
CcGetDirtyPages(
    IN PVOID LogHandle,
    IN PDIRTY_PAGE_ROUTINE DirtyPageRoutine,
    IN PVOID Context1,
    IN PVOID Context2)
{
    LARGE_INTEGER Zero = { .QuadPart = 0 };
    UNREFERENCED_PARAMETER(LogHandle);
    UNREFERENCED_PARAMETER(DirtyPageRoutine);
    UNREFERENCED_PARAMETER(Context1);
    UNREFERENCED_PARAMETER(Context2);
    return Zero;
}

LARGE_INTEGER
NTAPI
CcGetLsnForFileObject(
    IN PFILE_OBJECT FileObject,
    OUT PLARGE_INTEGER OldestLsn OPTIONAL)
{
    LARGE_INTEGER Zero = { .QuadPart = 0 };
    UNREFERENCED_PARAMETER(FileObject);
    if (OldestLsn) *OldestLsn = Zero;
    return Zero;
}

PVOID
NTAPI
CcRemapBcb(
    IN PVOID Bcb)
{
    UNREFERENCED_PARAMETER(Bcb);
    UNIMPLEMENTED;
    return NULL;
}

/* FILE SYSTEM CACHE ORDINALS ***********************************************/

/* The title-facing cache-size interface (Fsc* ordinals).  Sizes are in
 * pages, like retail; the cache works in 16 KiB blocks (NXC_PAGES_PER_BLOCK
 * pages each), so a request rounds up to the block and is clamped to
 * [NXC_MIN_BLOCKS, NXC_MAX_BLOCKS].  The floor keeps concurrent pins
 * deadlock-free: a pin chain holds the data block plus its FAT-metadata
 * block, and two chains must stay live at once.  The boot default is
 * NXC_DEFAULT_BLOCKS -- retail's 16-page (64 KiB) cache. */

PFN_COUNT
NTAPI
FscGetCacheSize(VOID)
{
    return NxcActiveBlocks * NXC_PAGES_PER_BLOCK;
}

NTSTATUS
NTAPI
FscSetCacheSize(
    IN PFN_COUNT NumberOfCachePages)
{
    ULONG Target = (NumberOfCachePages + NXC_PAGES_PER_BLOCK - 1) /
                   NXC_PAGES_PER_BLOCK;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Target < NXC_MIN_BLOCKS) Target = NXC_MIN_BLOCKS;
    if (Target > NXC_MAX_BLOCKS) Target = NXC_MAX_BLOCKS;

    ExAcquireFastMutex(&NxcMutex);

    /* Grow: activate empty slots. */
    for (ULONG i = 0; i < NXC_MAX_BLOCKS && NxcActiveBlocks < Target; i++)
    {
        if (NxcBlocks[i].Data != NULL) continue;
        NxcBlocks[i].Data = ExAllocatePoolWithTag(NonPagedPool,
                                                  NXC_BLOCK_SIZE, NXC_TAG);
        if (NxcBlocks[i].Data == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        NxcActiveBlocks++;
    }

    /* Shrink: retire unreferenced blocks, flushing dirty ones.  Blocks
     * pinned right now stay; the cache lands as close to the target as
     * the pins allow. */
shrink_restart:
    for (ULONG i = 0; i < NXC_MAX_BLOCKS && NxcActiveBlocks > Target; i++)
    {
        NXC_BLOCK *Block = &NxcBlocks[i];

        if (Block->Data == NULL) continue;
        if (Block->RefCount != 0 || Block->Busy) continue;

        if (Block->Map != NULL)
        {
            if (Block->Dirty && Block->Valid)
            {
                SIZE_T Written;
                Block->RefCount++;
                Block->Busy = TRUE;
                (void)NxcFlushBlock(Block, &Written);   /* unlocks */
                Block->RefCount--;
                goto shrink_restart;    /* table changed while unlocked */
            }
            RemoveEntryList(&Block->LruLink);
            Block->Map = NULL;
        }
        ExFreePoolWithTag(Block->Data, NXC_TAG);
        Block->Data = NULL;
        Block->Valid = FALSE;
        Block->Dirty = FALSE;
        NxcActiveBlocks--;
    }

    ExReleaseFastMutex(&NxcMutex);
    return Status;
}

/* Drop clean, unreferenced cache contents; the buffers stay. */
VOID
NTAPI
FscInvalidateIdleBlocks(VOID)
{
    ExAcquireFastMutex(&NxcMutex);
    for (ULONG i = 0; i < NXC_TOTAL_BLOCKS; i++)
    {
        NXC_BLOCK *Block = &NxcBlocks[i];
        if (Block->Data == NULL || Block->Map == NULL) continue;
        if (Block->RefCount != 0 || Block->Busy || Block->Dirty) continue;
        RemoveEntryList(&Block->LruLink);
        Block->Map = NULL;
        Block->Valid = FALSE;
    }
    ExReleaseFastMutex(&NxcMutex);
}

/* INIT *********************************************************************/

BOOLEAN
CcInitializeCacheManager(VOID)
{
    PVOID ZeroPage;

    ExInitializeFastMutex(&NxcMutex);
    InitializeListHead(&NxcLruList);
    InitializeListHead(&NxcMapList);

    for (ULONG i = 0; i < NXC_DEFAULT_BLOCKS; i++)
    {
        NxcBlocks[i].Data = ExAllocatePoolWithTag(NonPagedPool,
                                                  NXC_BLOCK_SIZE, NXC_TAG);
        if (NxcBlocks[i].Data == NULL) return FALSE;
        NxcActiveBlocks++;
    }

    /* Write-back reserve blocks: always allocated, never part of the
     * title-visible count, handed out only to re-entrant write-back
     * metadata reads (see NXC_WB_RESERVE). */
    for (ULONG i = NXC_MAX_BLOCKS; i < NXC_TOTAL_BLOCKS; i++)
    {
        NxcBlocks[i].Data = ExAllocatePoolWithTag(NonPagedPool,
                                                  NXC_BLOCK_SIZE, NXC_TAG);
        if (NxcBlocks[i].Data == NULL) return FALSE;
        NxcBlocks[i].Reserved = TRUE;
    }

    ZeroPage = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, NXC_TAG);
    if (ZeroPage == NULL) return FALSE;
    RtlZeroMemory(ZeroPage, PAGE_SIZE);
    NxcZeroPagePfn = (PFN_NUMBER)(MmGetPhysicalAddress(ZeroPage).QuadPart
                                  >> PAGE_SHIFT);

    DPRINT1("%lu x %lu KB blocks\n",
            NxcActiveBlocks, NXC_BLOCK_SIZE / 1024);
    return TRUE;
}

VOID
NTAPI
CcPfInitializePrefetcher(VOID)
{
}
