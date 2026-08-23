/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Boot-time MM init: carve title heap, paint KSEG0 + MMIO windows.
 *
 * Runs in two phases:
 *  1. NxkMmReserveXboxWindows (CODE_SEG INIT) -- called from
 *     MiInitMachineDependent very early in MmArmInitSystem.  Caps
 *     MmNonPagedPoolEnd below the Xbox MMIO windows, splits the NV2A
 *     framebuffer reservation, and reclaims most of low RAM into the
 *     title-heap arena tracked by xb/mm/contig.c.
 *  2. NxkMmEnsureXboxWindows -- called from Phase1Initialization after
 *     ntoskrnl frees its own discardable sections.  Paints the KSEG0
 *     identity window plus the WC and UC MMIO windows as PSE PDEs, then
 *     brings up the system-VA aperture via NxkMmInitSystemVa.
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>
#include <xb-debug.h>

#include "mm.h"

/* CR3 / PDE / PTE PRIMITIVES ***********************************************/

static
VOID
NxpMapLargePage(ULONG VirtualAddress, ULONG PhysicalAddress, ULONG Flags)
{
    /* One 4 MB PSE page directly in the page directory. */
    ((volatile ULONG *)NXK_PDE_BASE)[VirtualAddress >> 22] =
        (PhysicalAddress & 0xFFC00000UL) | Flags;
}

/* WINDOW SETUP STATE *******************************************************/

static LONG NxWindowsReady = 0;
static volatile LONG NxMmioWindowReady;

/* Set by Phase1Initialization (ex/init.c) right after it runs the
 * pre-paint MiFindInitializationCode/MiFreeInitializationCode pair.
 * Consumed by MmZeroPageThread (mm/ARM3/zeropage.c) to skip its own
 * call -- by then MiFindInitializationCode itself has been unmapped
 * and freed.  Volatile + plain LONG because it's a one-shot write
 * before any consumer runs. */
volatile LONG NxInitFreesDone = 0;

/* MAP SHAPE ****************************************************************/

BOOLEAN
NxkMmRetailShapedMap(VOID)
{
    static LONG Cached = -1;

    if (Cached < 0)
    {
        BOOLEAN Shaped = NxkIsRetailBase();
        if (!Shaped && KeLoaderBlock != NULL &&
            KeLoaderBlock->LoadOptions != NULL &&
            strstr(KeLoaderBlock->LoadOptions, "NXRETAILMAP") != NULL)
        {
            Shaped = TRUE;
        }
        Cached = Shaped ? 1 : 0;
    }
    return (BOOLEAN)Cached;
}

/* RESERVATION + RECLAIM ****************************************************/

extern PMEMORY_ALLOCATION_DESCRIPTOR MxFreeDescriptor;
extern MEMORY_ALLOCATION_DESCRIPTOR MxOldFreeDescriptor;
extern PLOADER_PARAMETER_BLOCK KeLoaderBlock;

static
BOOLEAN
NxpIsLoaderTypeFree(TYPE_OF_MEMORY Type)
{
    /* The set MiIsMemoryTypeFree considers free.  These end up on NT's
     * PFN free list unless we remove them from the descriptor first. */
    return (Type == LoaderFree         ||
            Type == LoaderLoadedProgram ||
            Type == LoaderFirmwareTemporary ||
            Type == LoaderOsloaderStack);
}

#ifndef NXK_MM_PHYS
static
BOOLEAN
NxpIsLoaderTypeTransientReclaimable(TYPE_OF_MEMORY Type)
{
    /* Descriptors that NT marks ActiveAndValid (refcount 1, not on PFN
     * free list) but whose contents are dead after kernel init -- their
     * data is read by NT during MmArmInitSystem only.  Safe for us to
     * hand to the title because nothing else writes them post-init.
     *
     * LoaderMemoryData is NOT in this set even though it would qualify
     * by NT semantics: the loader labels live structures with this type --
     * the page directory at PA 0xF000, the kernel's GDT/IDT/TSS/PCR/PT
     * cluster (placed above the kernel image), and
     * the MEMORY_ALLOCATION_DESCRIPTOR list itself (~PA 0x11000).
     * Writing to any of those while the CPU is using them
     * triple-faults the guest.
     *
     * LoaderSpecialMemory: NT's PFN init explicitly excludes this type
     * (see IncludeType[LoaderSpecialMemory]=FALSE in mminit.c) -- no PFN
     * entries are created at all, so NT can neither read nor allocate
     * from these pages.  Used by NxpSplitNv2aReserve to mark the 3.875 MB
     * reclaimed from the 4 MB framebuffer-reservation block as ours. */
    return (Type == LoaderSystemBlock        ||
            Type == LoaderStartupPanicStack  ||
            Type == LoaderSpecialMemory);
}
#endif /* !NXK_MM_PHYS */

/*
 * Storage for descriptors we splice into the loader-block list at boot
 * to carve out hardware-reserved sub-regions (e.g. the 128 KB NV2A
 * window inside the 4 MB framebuffer-reservation block).  Static so the
 * loader-block list keeps valid pointers for the kernel's lifetime.
 */
#define NXK_MAX_EXTRA_DESCRIPTORS  2
static MEMORY_ALLOCATION_DESCRIPTOR NxkExtraDescriptors[NXK_MAX_EXTRA_DESCRIPTORS];
static ULONG                       NxkExtraDescriptorCount;

/*
 * Split the LoaderFirmwarePermanent descriptor that freeldr leaves over the
 * top 4 MB of the 64 MB title-visible address space (PA 0x03C00000-
 * 0x04000000, originally a framebuffer reservation).  Only the top 128 KB
 * (PA 0x03FE0000-0x04000000) actually needs to stay permanent -- the NV2A
 * places RAMHT/RAMFC/graphics-contexts there and the GPU MMU finds them at
 * a fixed PA, so NT must not hand those pages out.  The lower ~3.875 MB has
 * no consumer in our build; retype it to LoaderFirmwareTemporary so the
 * heap-claim loop below puts it back in the title's contig arena.
 */
static
VOID
NxpSplitNv2aReserve(VOID)
{
    PLIST_ENTRY Entry;
    PMEMORY_ALLOCATION_DESCRIPTOR Desc, Tail;
#ifdef NXK_MM_PHYS
    /* Retail-probed: the claimable instance half is title-allocatable
     * RAM at boot (exact-window pins there succeed on retail; D3D takes
     * it back through MmClaimGpuInstanceMemory).  Only the topology
     * padding stays reserved -- in xemu it aliases PRAMIN and must
     * never be handed out as RAM. */
    ULONG_PTR SplitPfn = (NXK_NV2A_INSTANCE_BASE + NXK_NV2A_INSTANCE_BYTES)
                         >> PAGE_SHIFT;
#else
    ULONG_PTR SplitPfn = NXK_NV2A_INSTANCE_BASE >> PAGE_SHIFT;
#endif
    ULONG_PTR TopPfn   = NXK_KSEG0_RAM_SIZE >> PAGE_SHIFT;

    if (KeLoaderBlock == NULL) return;

    for (Entry = KeLoaderBlock->MemoryDescriptorListHead.Flink;
         Entry != &KeLoaderBlock->MemoryDescriptorListHead;
         Entry = Entry->Flink)
    {
        Desc = CONTAINING_RECORD(Entry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);

        if (Desc->MemoryType != LoaderFirmwarePermanent) continue;
        if (Desc->BasePage >= SplitPfn) continue;
        if (Desc->BasePage + Desc->PageCount < TopPfn) continue;

        if (NxkExtraDescriptorCount >= NXK_MAX_EXTRA_DESCRIPTORS)
        {
            DPRINT1("nv2a split: out of static descriptor storage\n");
            return;
        }

        Tail = &NxkExtraDescriptors[NxkExtraDescriptorCount++];
        Tail->MemoryType = LoaderFirmwarePermanent;
        Tail->BasePage   = SplitPfn;
        Tail->PageCount  = TopPfn - SplitPfn;
        InsertHeadList(&Desc->ListEntry, &Tail->ListEntry);

        Desc->PageCount  = SplitPfn - Desc->BasePage;
#ifdef NXK_MM_PHYS
        /* One pool: retype the lower part so the PFN-database walk frees
         * it straight into the page supply. */
        Desc->MemoryType = LoaderFirmwareTemporary;
#else
        /* Retype the lower part to LoaderSpecialMemory.  NT's PFN init
         * excludes that type just like LoaderFirmwarePermanent -- no PFN
         * entries are created and the pages don't enter any free list --
         * but it's distinguishable from the kept-reserved upper part,
         * so NxpIsLoaderTypeTransientReclaimable can pick it up below
         * and the heap-claim loop folds it into the title heap. */
        Desc->MemoryType = LoaderSpecialMemory;
#endif

        DPRINT1("nv2a split: reclaim PA %08lx-%08lx (%lu KB) for title heap; "
                "keep PA %08lx-%08lx (%lu KB) reserved for GPU\n",
                (ULONG)(Desc->BasePage << PAGE_SHIFT),
                (ULONG)((Desc->BasePage + Desc->PageCount) << PAGE_SHIFT),
                (ULONG)((Desc->PageCount << PAGE_SHIFT) >> 10),
                (ULONG)(Tail->BasePage << PAGE_SHIFT),
                (ULONG)((Tail->BasePage + Tail->PageCount) << PAGE_SHIFT),
                (ULONG)((Tail->PageCount << PAGE_SHIFT) >> 10));
        return;
    }

    DPRINT1("nv2a split: no FirmwarePermanent descriptor spans PA "
            "%08lx-%08lx; nv2a region unreserved\n",
            (ULONG)NXK_NV2A_INSTANCE_BASE,
            (ULONG)NXK_KSEG0_RAM_SIZE);
}

VOID
NTAPI
NxkMmReserveXboxWindows(VOID)
{
    extern PVOID MmNonPagedPoolEnd;
    PLIST_ENTRY ListEntry;
    PMEMORY_ALLOCATION_DESCRIPTOR Desc;
#ifndef NXK_MM_PHYS
    ULONG_PTR FloorPage = NXK_LOW_RECLAIM_FLOOR >> PAGE_SHIFT;
    ULONG_PTR KernelFloorPage = NXK_RECLAIM_KERNEL_FLOOR >> PAGE_SHIFT;
    ULONG_PTR CeilingPage;
    ULONG_PTR LimitPage;
#endif

    if ((ULONG_PTR)MmNonPagedPoolEnd > NXK_WC_BASE)
        MmNonPagedPoolEnd = (PVOID)NXK_WC_BASE;

    if (KeLoaderBlock == NULL) goto done;

    /* Boot dump of the physical memory descriptor map so we can verify the
     * carve-out sizing on different host configurations. */
    for (ListEntry = KeLoaderBlock->MemoryDescriptorListHead.Flink;
         ListEntry != &KeLoaderBlock->MemoryDescriptorListHead;
         ListEntry = ListEntry->Flink)
    {
        PMEMORY_ALLOCATION_DESCRIPTOR D =
            CONTAINING_RECORD(ListEntry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        DPRINT1("MAPDUMP type=%2lu base=%05lx pages=%6lu  PA %08lx-%08lx%s\n",
                (ULONG)D->MemoryType, (ULONG)D->BasePage, (ULONG)D->PageCount,
                (ULONG)(D->BasePage << PAGE_SHIFT),
                (ULONG)((D->BasePage + D->PageCount) << PAGE_SHIFT),
                (D == MxFreeDescriptor) ? " <Mx>" : "");
    }

    /* Carve the NV2A 128 KB hardware reservation out of the framebuffer-
     * reservation descriptor so the lower part rejoins the title heap. */
    NxpSplitNv2aReserve();

#ifdef NXK_MM_PHYS
    /*
     * One shared page database: every free-typed descriptor enters the
     * supply via the PFN-database walk, so there is no title-heap claim
     * to perform.  Two boot-order details remain:
     *
     *  - MiScanMemoryDescriptors picked MxFreeDescriptor = the largest
     *    free block, which on the split layout is the title's main
     *    window starting at the retail pool floor.  NT bring-up
     *    structures (MxGetNextPage) would squat the exact-window PAs
     *    titles pin, so re-point it at the kernel high zone's free
     *    descriptor -- the NT carve.
     *
     *  - Retail MmQueryStatistics reports all of RAM in
     *    TotalPhysicalPages; the descriptor scan excluded the
     *    hardware-reserved types, so stamp the retail value.
     */
    if (NxkMmRetailShapedMap())
    {
        extern PFN_COUNT MmNumberOfPhysicalPages;
        PMEMORY_ALLOCATION_DESCRIPTOR HighZone = NULL;

        for (ListEntry = KeLoaderBlock->MemoryDescriptorListHead.Flink;
             ListEntry != &KeLoaderBlock->MemoryDescriptorListHead;
             ListEntry = ListEntry->Flink)
        {
            Desc = CONTAINING_RECORD(ListEntry,
                                     MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
            if (!NxpIsLoaderTypeFree(Desc->MemoryType)) continue;
            if (Desc->BasePage < (NXK_HIGH_ZONE_BASE >> PAGE_SHIFT)) continue;
            if (Desc->BasePage + Desc->PageCount >
                (NXK_HIGH_ZONE_LIMIT >> PAGE_SHIFT)) continue;
            if (HighZone == NULL || Desc->PageCount > HighZone->PageCount)
                HighZone = Desc;
        }
        if (HighZone != NULL && MxFreeDescriptor != HighZone)
        {
            MxFreeDescriptor = HighZone;
            MxOldFreeDescriptor = *HighZone;
            DPRINT1("MxFreeDescriptor -> high zone PA %08lx pages=%lu\n",
                    (ULONG)(HighZone->BasePage << PAGE_SHIFT),
                    (ULONG)HighZone->PageCount);
        }

        MmNumberOfPhysicalPages = NXK_KSEG0_RAM_SIZE >> PAGE_SHIFT;

        /* The descriptor scan dropped the hardware-reserved tail (the
         * FB + NV2A block) from MmHighestPhysicalPage; the NV2A split
         * above reclaims most of it for the title, so the PFN walk and
         * the supply must extend through the full 64 MB. */
        {
            extern PFN_NUMBER MmHighestPhysicalPage;
            if (MmHighestPhysicalPage < (NXK_KSEG0_RAM_SIZE >> PAGE_SHIFT) - 1)
                MmHighestPhysicalPage = (NXK_KSEG0_RAM_SIZE >> PAGE_SHIFT) - 1;
        }
    }
    else
    {
        /* Non-retail (devkit) map: MiScanMemoryDescriptors excludes the
         * hardware-reserved LoaderFirmwarePermanent framebuffer block from
         * MmNumberOfPhysicalPages, but NxpSplitNv2aReserve reclaimed most of
         * it into the page supply -- so the running count undercounts the RAM
         * the supply manages, and MmQueryStatistics would report
         * AvailablePages greater than TotalPhysicalPages.  Report all of
         * physical RAM, as retail MmQueryStatistics does, by taking the top
         * of the loader descriptor list (every descriptor, reserved blocks
         * included) so the result is right regardless of where the
         * reservation lands. */
        extern PFN_COUNT MmNumberOfPhysicalPages;
        ULONG_PTR TopPage = 0;

        for (ListEntry = KeLoaderBlock->MemoryDescriptorListHead.Flink;
             ListEntry != &KeLoaderBlock->MemoryDescriptorListHead;
             ListEntry = ListEntry->Flink)
        {
            Desc = CONTAINING_RECORD(ListEntry,
                                     MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
            if (Desc->BasePage + Desc->PageCount > TopPage)
                TopPage = Desc->BasePage + Desc->PageCount;
        }
        if (TopPage > MmNumberOfPhysicalPages)
            MmNumberOfPhysicalPages = (PFN_COUNT)TopPage;
    }
    goto done;
#else
    /* Locate the kernel-structure ceiling: the lowest BasePage of a
     * non-reclaimable descriptor at high PA (>= NXK_RECLAIM_KERNEL_FLOOR).
     * freeldr packs the kernel image + its GDT/IDT/TSS/PCR/page tables at the
     * top of RAM, so everything reclaimable below this is one contiguous span
     * the title can own.  Default to all of RAM if nothing is found. */
    CeilingPage = NXK_KSEG0_RAM_SIZE >> PAGE_SHIFT;
    for (ListEntry = KeLoaderBlock->MemoryDescriptorListHead.Flink;
         ListEntry != &KeLoaderBlock->MemoryDescriptorListHead;
         ListEntry = ListEntry->Flink)
    {
        Desc = CONTAINING_RECORD(ListEntry,
                                 MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        if (Desc->PageCount == 0) continue;
        if (Desc->BasePage < KernelFloorPage) continue;
        if (NxpIsLoaderTypeFree(Desc->MemoryType) ||
            NxpIsLoaderTypeTransientReclaimable(Desc->MemoryType))
            continue;
        if (Desc->BasePage < CeilingPage)
            CeilingPage = Desc->BasePage;
    }

    /* Push the ceiling out to the NV2A reservation: small kernel-startup
     * descriptors (LoaderStartupPcrPage etc.) between the heap top and the
     * NV2A region would otherwise pin the ceiling at their low PA and waste
     * the megabytes above them.  The heap-claim loop below skips non-free /
     * non-transient descriptors so any kernel-live structures inside the
     * expanded range stay untouched -- they just become holes in the heap. */
    if (CeilingPage < (NXK_NV2A_INSTANCE_BASE >> PAGE_SHIFT))
        CeilingPage = NXK_NV2A_INSTANCE_BASE >> PAGE_SHIFT;

    /* Claim every reclaimable page in [Floor, Ceiling) into one coalesced
     * title arena.  The historical NXK_NT_RESERVE_PFNS "safety slice"
     * below the kernel is no longer carved off: on the 128 MB devkit
     * config NT's MxFreeDescriptor is the high-PA block above the kernel
     * (always > 50 MB), so the safety slice was always unused, and the
     * pages below were stranded.  If MxFreeDescriptor turns out to be too
     * small (e.g. a low-PA pick), the re-point logic further down still
     * promotes it to the largest free descriptor we left. */
    LimitPage = CeilingPage;
    DPRINT1("reclaim ceiling=%05lx limit=%05lx\n",
            (ULONG)CeilingPage, (ULONG)LimitPage);

    for (ListEntry = KeLoaderBlock->MemoryDescriptorListHead.Flink;
         ListEntry != &KeLoaderBlock->MemoryDescriptorListHead;
         ListEntry = ListEntry->Flink)
    {
        ULONG_PTR DescEnd;
        ULONG     ClaimPages;

        Desc = CONTAINING_RECORD(ListEntry,
                                 MEMORY_ALLOCATION_DESCRIPTOR,
                                 ListEntry);

        if (Desc->BasePage >= LimitPage) break;     /* descriptors are sorted */
        if (Desc->PageCount == 0) continue;
        if (Desc->BasePage < FloorPage) continue;   /* protect IVT + PD@0xF000 */

        DescEnd = Desc->BasePage + Desc->PageCount;

        if (NxpIsLoaderTypeFree(Desc->MemoryType))
        {
            /* Free pages inside the kernel high zone are NT's bring-up
             * carve (MxFreeDescriptor) on the split layout -- the only
             * free memory NT gets on 64 MB.  Leave them alone. */
            if (NxkIsRetailBase() &&
                Desc->BasePage >= (NXK_HIGH_ZONE_BASE >> PAGE_SHIFT) &&
                DescEnd <= (NXK_HIGH_ZONE_LIMIT >> PAGE_SHIFT))
            {
                continue;
            }

            /* For MxFreeDescriptor and other free descriptors: claim the
             * portion below LimitPage; leave the rest for NT. */
            ULONG_PTR ClaimEnd = (DescEnd < LimitPage) ? DescEnd : LimitPage;
            ClaimPages = (ULONG)(ClaimEnd - Desc->BasePage);
            NxpAddHeapPagesCoalesced(Desc->BasePage << PAGE_SHIFT, ClaimPages);
            Desc->BasePage += ClaimPages;
            Desc->PageCount -= ClaimPages;
        }
        else if (NxpIsLoaderTypeTransientReclaimable(Desc->MemoryType))
        {
            /* Claim the full descriptor without changing its type.  PFN
             * init marks the pages ActiveAndValid (refcount 1); NT won't
             * allocate them and our heap takes them. */
            ULONG_PTR ClaimEnd = (DescEnd < LimitPage) ? DescEnd : LimitPage;
            ClaimPages = (ULONG)(ClaimEnd - Desc->BasePage);
            NxpAddHeapPagesCoalesced(Desc->BasePage << PAGE_SHIFT, ClaimPages);
        }
        /* else: skip -- LoaderBootDriver, LoaderNlsData, LoaderRegistryData
         * etc. carry data NT actively uses past init. */
    }

    /* Split a low-PA reserve off the start of the title arena.  The first
     * heap region after reclaim covers the low portion of RAM (PA 0x10000
     * upward); peel NXK_LOW_RESERVE_PAGES off its base into a separate
     * region marked LowReserve so unconstrained allocs skip it, leaving
     * room for later <16 MB constrained requests.
     *
     * Skipped on the split layout: there the arena starts exactly at the
     * retail pool floor, where titles pin exact contiguous windows --
     * a reserve at that PA would break the pins, and constrained-low
     * requests are satisfied top-down inside their window anyway. */
    if (NxHeapRegionCount > 0 &&
        NxHeapRegions[0].Pa < NXK_RETAIL_POOL_FLOOR &&
        NxHeapRegions[0].Pages > NXK_LOW_RESERVE_PAGES * 2)
    {
        if (NxHeapRegionCount >= NXK_MAX_HEAP_REGIONS)
        {
            DPRINT1("heap region table full, can't split low reserve\n");
        }
        else
        {
            ULONG i;
            for (i = NxHeapRegionCount; i > 0; i--)
                NxHeapRegions[i] = NxHeapRegions[i - 1];
            NxHeapRegionCount++;
            NxHeapRegions[0].Pages = NXK_LOW_RESERVE_PAGES;
            NxHeapRegions[0].LowReserve = TRUE;
            NxHeapRegions[1].Pa += NXK_LOW_RESERVE_BYTES;
            NxHeapRegions[1].Pages -= NXK_LOW_RESERVE_PAGES;
            NxHeapRegions[1].LowReserve = FALSE;
        }
    }

    /* MiScanMemoryDescriptors picked MxFreeDescriptor = the largest free block
     * (and saved MxOldFreeDescriptor) BEFORE we ran.  If that pick was the low
     * Mx region we just claimed for the title, it's now empty and ARM3 bring-up
     * (MiBuildPfnDatabaseFromLoaderBlock / MxGetNextPage) would find 0 free
     * pages and bugcheck 0x7D (INSTALL_MORE_MEMORY).  ONLY in that case re-point
     * MxFreeDescriptor (+ its saved copy) to the largest free descriptor we
     * LEFT (the NT reserve / any high region above the title's blocks).
     *
     * IMPORTANT: skip this when MxFreeDescriptor still has ample pages -- e.g.
     * the 128 MB devkit config, where the >64 MB region above the kernel is the
     * largest and NT already picked it (we never claim it).  Re-stamping
     * MxOldFreeDescriptor there would clobber the original base/count that NT
     * may have already advanced via early MxGetNextPage calls, corrupting the
     * PFN free-list accounting (contig allocs then fail -> 0x50 PF). */
    if (MxFreeDescriptor == NULL ||
        MxFreeDescriptor->PageCount < NXK_NT_RESERVE_PFNS)
    {
        PMEMORY_ALLOCATION_DESCRIPTOR Best = NULL;
        ULONG_PTR BestPages = 0;
        for (ListEntry = KeLoaderBlock->MemoryDescriptorListHead.Flink;
             ListEntry != &KeLoaderBlock->MemoryDescriptorListHead;
             ListEntry = ListEntry->Flink)
        {
            Desc = CONTAINING_RECORD(ListEntry,
                                     MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
            if (NxpIsLoaderTypeFree(Desc->MemoryType) &&
                Desc->PageCount > BestPages)
            {
                Best = Desc;
                BestPages = Desc->PageCount;
            }
        }
        if (Best != NULL)
        {
            MxFreeDescriptor = Best;
            MxOldFreeDescriptor = *Best;
            DPRINT1("re-pointed MxFreeDescriptor -> PA %08lx pages=%lu (%lu KB) for NT bring-up\n",
                    (ULONG)(Best->BasePage << PAGE_SHIFT), (ULONG)Best->PageCount,
                    (ULONG)((Best->PageCount << PAGE_SHIFT) >> 10));
        }
    }
    else
    {
        DPRINT1("MxFreeDescriptor kept (PA %08lx pages=%lu) -- not emptied by carve-out\n",
                (ULONG)(MxFreeDescriptor->BasePage << PAGE_SHIFT),
                (ULONG)MxFreeDescriptor->PageCount);
    }
#endif /* NXK_MM_PHYS */

done:
#ifndef NXK_MM_PHYS
    {
        ULONG i;
        for (i = 0; i < NxHeapRegionCount; i++)
            DPRINT1("title-heap region[%lu] PA=%08lx pages=%lu (%lu KB)\n",
                    i, (ULONG)NxHeapRegions[i].Pa,
                    NxHeapRegions[i].Pages,
                    (ULONG)((((ULONG_PTR)NxHeapRegions[i].Pages) << PAGE_SHIFT) >> 10));
    }
#else
    return;
#endif
}

/* INIT-SECTION PAGE DONATION ***********************************************/

/*
 * Called by MiFreeInitializationCode in place of the NT free.  When the
 * freed init pages sit in title-visible RAM (kernel linked at the retail
 * IMAGEBASE), hand them to the title heap instead of NT's PFN free list:
 * titles hardcode a contiguous pool starting right past the retail
 * kernel's resident sections, so the discarded image tail
 * (INIT / INITDATA / .rsrc) must rejoin the title arena, not NT pool.
 * The PFN entries stay ActiveAndValid (refcount 1), exactly like the
 * boot-reclaim's transient-reclaimable descriptors -- NT never allocates
 * them, our heap owns them.
 *
 * Returns FALSE -- caller proceeds with the NT free -- when the range
 * falls outside title-visible RAM (the devkit IMAGEBASE 0x84000000
 * layout, where the kernel sits above the 64 MB window and the freed
 * pages are only NT-reachable) or is not PT-mapped as expected.
 * All-or-nothing: nothing is unmapped or donated until the whole range
 * has validated, so the fallback path always sees untouched pages.
 *
 * Ordering: must run while the kernel image is still PT-mapped (before
 * the KSEG0 PSE paint) because the PAs are read out of the PTEs -- the
 * same constraint the NT free path has; Phase1Initialization sequences
 * both.  After the paint, the donated PAs are title-reachable through
 * the painted KSEG0 alias at VA (0x80000000 | PA), which at the retail
 * base is the same VA the section occupied.
 */
BOOLEAN
NxkMmTryDonateInitPages(PVOID InitStart, PVOID InitEnd)
{
    volatile ULONG *PdeBase = (volatile ULONG *)NXK_PDE_BASE;
    volatile ULONG *PteBase = (volatile ULONG *)NXK_PTE_BASE;
    ULONG_PTR VaStart = (ULONG_PTR)InitStart & ~((ULONG_PTR)PAGE_SIZE - 1);
    ULONG_PTR Pages   = ((ULONG_PTR)InitEnd >> PAGE_SHIFT)
                            - (VaStart >> PAGE_SHIFT);
    ULONG_PTR StartPa = 0;
    ULONG_PTR i;

    if (Pages == 0) return TRUE;    /* nothing to free either way */

    /* Pass 1: validate.  Every page must be PT-mapped (present PDE, no
     * PSE, present PTE), physically contiguous with its predecessor, and
     * inside the title-visible window the heap can alias. */
    for (i = 0; i < Pages; i++)
    {
        ULONG_PTR Va = VaStart + (i << PAGE_SHIFT);
        ULONG Pde = PdeBase[Va >> 22];
        ULONG Pte;
        ULONG_PTR Pa;

        if (!(Pde & 0x1) || (Pde & 0x80)) return FALSE;
        Pte = PteBase[Va >> PAGE_SHIFT];
        if (!(Pte & 0x1)) return FALSE;

        Pa = Pte & ~((ULONG_PTR)PAGE_SIZE - 1);
        if (Pa < NXK_LOW_RECLAIM_FLOOR) return FALSE;
        if (Pa + PAGE_SIZE > NXK_NV2A_INSTANCE_BASE) return FALSE;
        if (i == 0)
            StartPa = Pa;
        else if (Pa != StartPa + (i << PAGE_SHIFT))
            return FALSE;
    }

    /* Pass 2: unmap and donate. */
    for (i = 0; i < Pages; i++)
    {
        ULONG_PTR Va = VaStart + (i << PAGE_SHIFT);
        PteBase[Va >> PAGE_SHIFT] = 0;
        __invlpg((PVOID)Va);    /* kernel PTEs are global; CR3 reload
                                   wouldn't flush them */
    }
    NxkMmDonateHeapPages(StartPa, (ULONG)Pages);


    DPRINT1("init-page donation: VA %08lx-%08lx -> title heap PA %08lx-%08lx (%lu KB)\n",
            (ULONG)VaStart, (ULONG)(VaStart + (Pages << PAGE_SHIFT)),
            (ULONG)StartPa, (ULONG)(StartPa + (Pages << PAGE_SHIFT)),
            (ULONG)((Pages << PAGE_SHIFT) >> 10));
    return TRUE;
}

/* KSEG0 + MMIO WINDOW PAINTING *********************************************/

/*
 * The WC + UC MMIO paints don't depend on the kernel-image PSE state, so
 * they can run before init-section free.  Split out so bootvid can read
 * NV2A CRTC registers (0xFD600140 etc.) during Phase1InitializationDiscard
 * before the framebuffer pointer is latched.
 */
VOID
NxkMmEnsureMmioWindow(VOID)
{
    ULONG Va;

    if (InterlockedCompareExchange(&NxMmioWindowReady, 1, 0) != 0)
        return;

    /* Write-combined RAM alias (AGP-tiled window): VA 0xF0000000-0xF8000000
     * maps to low RAM with PAT1 = WC.  Safe to overlay because
     * NxkMmReserveXboxWindows capped MmNonPagedPoolEnd at NXK_WC_BASE before
     * any pool VA was handed out. */
    for (Va = NXK_WC_BASE; Va < NXK_WC_LIMIT; Va += NXK_4MB)
        NxpMapLargePage(Va, Va & NXK_PHYS_MASK, NXK_PDE_WC);

    /* Device MMIO + flash mirrors: VA == PA for 0xF8000000-0xFFC00000,
     * strong UC via PAT3.  Covers NV2A, APU, USB, NVNET, and the flash
     * mirrors.  Count loop (not Va < LIMIT) since LIMIT would be
     * 0x100000000 -- a 33-bit value the ULONG comparison can't express. */
    for (Va = NXK_MMIO_BASE; Va < NXK_MMIO_LIMIT; Va += NXK_4MB)
        NxpMapLargePage(Va, Va, NXK_PDE_MMIO);

    __writecr3(__readcr3());

    DPRINT1("Xbox MMIO windows mapped (WC 0xF0000000-0xF8000000, "
            "UC MMIO 0xF8000000-0xFFC00000)\n");
}

/*
 * Establish the Xbox windows in the current (System-process) page directory.
 *
 * Called eagerly from Phase1Initialization in ex/init.c, after the
 * .INIT-section Phase1InitializationDiscard work has returned and after
 * Phase1Initialization has freed ntoskrnl's own discardable sections
 * (so we don't have to skip the free for VAs that fall inside our
 * paint range -- they're already gone).  No title has spawned yet, so
 * the memory map is stable for the title's entire life -- no "kick the
 * painter" allocation is needed before the title can touch device MMIO
 * or the KSEG0 alias.
 *
 * The first PDE we overlay (VA 0x80000000-0x80400000 -> PA 0-0x400000)
 * contains the kernel image (PA 0x10000-0x510000, kernel linked at VA
 * 0x80010000); the PSE overlay maps the same PA range so the running
 * kernel's code/data stay reachable at their existing VAs.  The second
 * PDE (VA 0x80400000-0x80800000 -> PA 0x400000-0x800000) contains the
 * tail of the kernel image (PA 0x400000-0x510000); same argument.
 *
 * Idempotent via NxWindowsReady.  Called eagerly once from ex/init.c at
 * the end of Phase 1 init, just before XeRunInitialTitle spawns the
 * title.
 */
VOID
NxkMmEnsureXboxWindows(VOID)
{
    ULONG Va;

    /* WC + UC MMIO -- idempotent, may already have run early for bootvid. */
    NxkMmEnsureMmioWindow();

    if (InterlockedCompareExchange(&NxWindowsReady, 1, 0) != 0)
        return;

    /* Full KSEG0 RAM identity, per cxbx-reloaded's AddressRanges.h:
     * VA 0x80000000-0x84000000 -> PA 0x00000000-0x04000000.  16 PDEs of
     * 4 MB each, all WB cache.  pbkit's GPU mask `addr & 0x03FFFFFF`
     * matches this layout exactly.
     *
     * Cap the paint at MmSystemPteSpaceStart (currently 0x82C00000).
     * Past that, ReactOS reserves "System PTE Space" -- the VA range it
     * carves PTEs from for I/O / MDL mappings.  PSE-overlaying those
     * PDEs orphans the PT pages backing them; later kernel-VA mappings
     * from that range then silently map to the PSE PA instead of the
     * caller's PFN, producing progressive dispatcher corruption (a
     * KeDelay-after-the-paint cycle would hang the wait queue once
     * something in its path touched a System PTE VA).  Bisected
     * 2026-05-25 via tests/xbe/kedelay-test: painting PDI 0x20B alone
     * (= VA 0x82C00000, the very first PDE in the System PTE Space) was
     * sufficient to reproduce the dispatcher freeze. */
    {
        extern PVOID MmSystemPteSpaceStart;
        ULONG_PTR Cap = (ULONG_PTR)MmSystemPteSpaceStart;
        if (Cap == 0 || Cap > NXK_KSEG0_BASE + NXK_KSEG0_RAM_SIZE)
            Cap = NXK_KSEG0_BASE + NXK_KSEG0_RAM_SIZE;
        Cap &= ~(NXK_4MB - 1);  /* round down to PSE boundary */
        for (Va = NXK_KSEG0_BASE; Va < Cap; Va += NXK_4MB)
            NxpMapLargePage(Va, Va & NXK_PHYS_MASK, NXK_PDE_LARGE);
    }

    /* Flush the TLB so the new entries take effect. */
    __writecr3(__readcr3());

#ifdef NXK_MM_PHYS
    /* The loader's identity PTs died with the user-PDE zero and its
     * KSEG0 PTs were just replaced by the PSE paint; on the retail-
     * shaped map they sit in title-visible RAM at the head of the
     * loader cluster -- hand the 20 pages back.  (The PCR PT and the
     * descriptor tables that follow them stay live.)  Keyed to the
     * image base, NOT the map shape: the cluster PA belongs to the
     * split layout; the DBG hybrid keeps its devkit cluster above the
     * title world and these PAs are ordinary free RAM there. */
    if (NxkIsRetailBase())
    {
        extern VOID NTAPI NxkPageSupplyReturn(PFN_NUMBER Page);
        KIRQL OldIrql = KeAcquireQueuedSpinLock(LockQueuePfnLock);
        ULONG i;
        for (i = 0; i < 20; i++)
            NxkPageSupplyReturn((0x03BC0000UL >> PAGE_SHIFT) + i);
        KeReleaseQueuedSpinLock(LockQueuePfnLock, OldIrql);
    }

    /* The loader backs all 16 pages of the PCR region (VA 0xFFDF0000..)
     * but only two are architecturally addressed: KUSER_SHARED_DATA at
     * the base and the KPCR at the top.  Unmap the middle and donate
     * the frames -- a stray access faults loudly instead of reading
     * silently recycled memory.  PA comes from the live PTEs, so this
     * works on both boot layouts. */
    {
        extern VOID NTAPI NxkPageSupplyReturn(PFN_NUMBER Page);
        volatile ULONG *PteBase = (volatile ULONG *)0xC0000000;
        KIRQL OldIrql = KeAcquireQueuedSpinLock(LockQueuePfnLock);
        ULONG Va;
        for (Va = 0xFFDF1000UL; Va < 0xFFDFF000UL; Va += PAGE_SIZE)
        {
            ULONG Pte = PteBase[Va >> PAGE_SHIFT];
            if (!(Pte & 1)) continue;
            PteBase[Va >> PAGE_SHIFT] = 0;
            __invlpg((PVOID)Va);
            NxkPageSupplyReturn(Pte >> PAGE_SHIFT);
        }
        KeReleaseQueuedSpinLock(LockQueuePfnLock, OldIrql);
    }
#endif

    DPRINT1("Xbox KSEG0 painted (0x80000000-0x84000000 WB)\n");

    /* Bring up the system-memory aperture eagerly so the first call
     * to MmAllocateSystemMemory doesn't have to allocate PT pages
     * under whatever IRQL / lock the caller holds. */
    NxkMmInitSystemVa();
}

/* EOF */
