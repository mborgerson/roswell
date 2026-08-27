/*
 * PROJECT:     nxkrnl (ntoskrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared internals for the Xbox memory manager.
 *
 * Layout constants, heap-region type, and cross-file prototypes used by
 * xb/mm/{init,contig,sysva,iospace,api}.c.  Not exposed to non-MM kernel
 * code -- those go through the title-facing ABI in xb/mm/api.c and the
 * NxMm* wrappers in xb/mm/{contig,iospace}.c.
 */

#pragma once

/* XBOX MEMORY LAYOUT *******************************************************/

#define NXK_KSEG0_BASE      0x80000000UL    /* VA = PA | KSEG0_BASE */
#define NXK_PHYS_MASK       0x03FFFFFFUL    /* 64 MB physical address space */

/*
 * Full KSEG0 RAM identity window per the retail Xbox kernel: VA
 * 0x80000000-0x84000000 aliases the entire 64 MB of physical RAM.
 * cxbx-reloaded's AddressRanges.h calls this the "Contiguous Memory
 * Alias"; pbkit's (addr & 0x03FFFFFF) mask presumes it.  Painted as
 * 16 contiguous 4 MB PSE PDEs.
 */
#define NXK_KSEG0_RAM_SIZE  0x04000000UL    /* 64 MB */

/* NV2A hardware-reserved region at the top of the 64 MB address space.
 * See the GPU INSTANCE MEMORY block near MmClaimGpuInstanceMemory for the
 * details; declared here so NxkMmReserveXboxWindows can shape the heap
 * around them. */
#define NXK_NV2A_INSTANCE_BASE     0x03FE0000UL
#define NXK_NV2A_INSTANCE_BYTES    0x00010000UL    /* 64 KB claimable      */
#define NXK_NV2A_PADDING_BYTES     0x00010000UL    /* 64 KB topology gap   */
#define NXK_NV2A_HEAP_CEILING      (NXK_KSEG0_RAM_SIZE - NXK_NV2A_PADDING_BYTES)

/*
 * Retail Xbox 0xF0000000-base region split into a write-combined RAM
 * alias (cxbx AddressRanges 'AGP Tiled' / 'Extended AGP') and the
 * uncached device-MMIO range:
 *
 *   0xF0000000 - 0xF7FFFFFF   128 MB   WC alias of low 64 MB RAM (2x mirror)
 *   0xF8000000 - 0xFFBFFFFF   ~124 MB  UC device MMIO + flash mirrors
 */
#define NXK_WC_BASE         0xF0000000UL
#define NXK_WC_LIMIT        0xF8000000UL
#define NXK_MMIO_BASE       0xF8000000UL
/* UC MMIO stops at PDI 0x3FE (VA 0xFFC00000).  PDI 0x3FF is ReactOS-owned
 * (MI_HIGHEST_SYSTEM_ADDRESS etc.) -- overlaying it orphans the live PT. */
#define NXK_MMIO_LIMIT      0xFFC00000UL

#define NXK_4MB             0x00400000UL

/* i386 page-directory / page-table entry bits.
 *
 * NXK_PDE_WC selects IA32_PAT entry 1 (PWT=1, PCD=0, PAT=0), which
 * Ki386SetXboxPat programs as write-combining; see ke/i386/patpge.c.
 * NXK_PDE_MMIO selects PAT entry 3 (PWT=0, PCD=1, PAT=0) = strong UC. */
#define NXK_PDE_BASE        0xC0300000UL    /* self-mapped page directory */
#define NXK_PTE_BASE        0xC0000000UL    /* self-mapped page tables */
#define NXK_PDE_LARGE       0x00000083UL    /* present | write | large page */
#define NXK_PDE_WC          0x0000008BUL    /* + PWT -> PAT1 = WC */
#define NXK_PDE_MMIO        0x00000093UL    /* + PCD -> PAT3 = UC */
#define NXK_PDE_PT          0x00000003UL    /* present | write (PT, not PSE) */
#define NXK_PTE_VALID       0x00000003UL    /* present | write */

/*
 * System-memory aperture.  Retail Xbox MmAllocateSystemMemory hands out
 * 0xD0000000+ VAs from the documented system-VA window.
 */
#define NXK_SYSMEM_BASE     0xD0000000UL
#define NXK_SYSMEM_PT_COUNT 16                           /* 16 PT pages = 64 MB */
#define NXK_SYSMEM_BYTES    (NXK_SYSMEM_PT_COUNT * NXK_4MB)
#define NXK_SYSMEM_LIMIT    (NXK_SYSMEM_BASE + NXK_SYSMEM_BYTES)

/* SPLIT-IMAGE (RETAIL BASE) LAYOUT *****************************************/

/*
 * When the kernel links at the retail base, the image is split: header +
 * .text in the retail slot below the title's contiguous-pool floor, all
 * other sections + NT's bring-up carve + the loader cluster in an 8 MB
 * "kernel high zone".  Titles hardcode their pool floor right past the
 * retail resident sections, so nothing kernel-owned may live between the
 * floor and the high zone.
 */
#define NXK_RETAIL_IMAGE_BASE   0x80010000UL
#define NXK_RETAIL_POOL_FLOOR   0x00061000UL
#define NXK_HIGH_ZONE_BASE      0x03400000UL
#define NXK_HIGH_ZONE_LIMIT     0x03C00000UL

/* Which layout is this kernel running?  Decided at link time. */
extern UCHAR __ImageBase[];
#define NxkIsRetailBase() \
    ((ULONG_PTR)__ImageBase == NXK_RETAIL_IMAGE_BASE)

/* Is the MEMORY MAP retail-shaped?  True at the retail base, and on the
 * DBG diagnostic hybrid (devkit image + retail-shaped sub-64 MB map,
 * flagged by the loader via /NXRETAILMAP in LoadOptions).  Use this --
 * not NxkIsRetailBase -- wherever allocation policy depends on the map
 * shape rather than on where the kernel image lives. */
BOOLEAN NxkMmRetailShapedMap(VOID);

/* HEAP-REGION TRACKING *****************************************************/

/* Floor / ceiling / reserve sizing for NxkMmReserveXboxWindows. */
#define NXK_NT_RESERVE_PFNS      ( 2 * 1024 * 1024 / PAGE_SIZE)
#define NXK_RECLAIM_KERNEL_FLOOR (16 * 1024 * 1024)
#define NXK_LOW_RECLAIM_FLOOR    (0x11000UL)

#define NXK_MAX_HEAP_REGIONS  4

/* Low-PA reserve: split off the start of the title arena so constrained-low
 * requests (< 16 MB) still find room after the main pin-heavy block fills. */
#define NXK_LOW_RESERVE_BYTES  (64 * 1024)
#define NXK_LOW_RESERVE_PAGES  (NXK_LOW_RESERVE_BYTES / PAGE_SIZE)
#define NXK_LOW_PA_THRESHOLD   (0x01000000UL)

typedef struct _NXK_HEAP_REGION
{
    ULONG_PTR Pa;           /* PA base */
    ULONG     Pages;        /* page count (0 = unused slot) */
    BOOLEAN   LowReserve;   /* TRUE = constrained-low requests only */
} NXK_HEAP_REGION;

extern NXK_HEAP_REGION NxHeapRegions[NXK_MAX_HEAP_REGIONS];
extern ULONG           NxHeapRegionCount;

/* Set by Phase1Initialization (ex/init.c) right after it runs the
 * pre-paint MiFindInitializationCode/MiFreeInitializationCode pair.
 * Consumed by MmZeroPageThread to skip its own call. */
extern volatile LONG NxInitFreesDone;

/* CROSS-FILE PROTOTYPES ****************************************************/

/* xb/mm/contig.c -- heap-region helper called during boot init. */
VOID NxpAddHeapPagesCoalesced(ULONG_PTR Pa, ULONG Pages);

/* xb/mm/contig.c -- post-boot sorted-insert donation (locked). */
VOID NxkMmDonateHeapPages(ULONG_PTR Pa, ULONG Pages);

/* xb/mm/init.c -- donate freed init-section pages to the title heap;
 * called by MiFreeInitializationCode (mm/ARM3/sysldr.c).  FALSE = range
 * not donatable, caller runs the normal NT free. */
BOOLEAN NxkMmTryDonateInitPages(PVOID InitStart, PVOID InitEnd);

/* xb/mm/sysva.c -- aperture init called from NxkMmEnsureXboxWindows. */
VOID NxkMmInitSystemVa(VOID);
PVOID NxkSysVaAllocate(SIZE_T Size, ULONG Protect);
ULONG NxkSysVaFree(PVOID Va, SIZE_T Size);

/* xb/mm/init.c -- boot window painting (called by ex/init.c + lazily by
 * contig allocator). */
VOID NTAPI NxkMmReserveXboxWindows(VOID);
VOID NxkMmEnsureMmioWindow(VOID);
VOID NxkMmEnsureXboxWindows(VOID);

/* xb/mm/iospace.c -- VA->PA for sysva.c's PTE painting. */
ULONG_PTR NTAPI NxMmGetPhysicalAddress(IN PVOID Va);

/* xb/mm/vm.c -- the title VM manager (NXK_MM_VM). */
NTSTATUS NTAPI NxVmAllocateVirtualMemory(IN OUT PVOID *BaseAddress,
                                         IN ULONG_PTR ZeroBits,
                                         IN OUT PSIZE_T RegionSize,
                                         IN ULONG AllocationType,
                                         IN ULONG Protect);
NTSTATUS NTAPI NxVmFreeVirtualMemory(IN OUT PVOID *BaseAddress,
                                     IN OUT PSIZE_T RegionSize,
                                     IN ULONG FreeType);
NTSTATUS NTAPI NxVmProtectVirtualMemory(IN OUT PVOID *BaseAddress,
                                        IN OUT PSIZE_T RegionSize,
                                        IN ULONG NewProtect,
                                        OUT PULONG OldProtect);
NTSTATUS NTAPI NxVmQueryVirtualMemory(IN PVOID Address, OUT PVOID MbiOut);
BOOLEAN NxVmOwnsAddress(IN PVOID Address);
ULONG NxVmQueryAddressProtect(IN PVOID Address);
VOID NxVmSetAddressProtect(IN PVOID BaseAddress, IN ULONG NumberOfBytes,
                           IN ULONG NewProtect);
SIZE_T NxVmCommittedBytes(VOID);
SIZE_T NxVmReservedBytes(VOID);
PFN_COUNT NxVmCommittedPagesInRange(IN ULONG_PTR Start, IN ULONG_PTR End);

/* xb/mm/poolpages.c -- VA-contiguous page runs for nxpool
 * (NXK_MM_POOLPAGES).  The window caps total nonpaged-pool VA (the bucket
 * allocator carves its pages from here too); 16 MB is ample on a 64 MB box
 * once the title image + contiguous pool + framebuffer are accounted for.
 * NxpPageState is one resident byte per window page, so this also sizes it. */
#define NXK_POOL_WINDOW_BASE   0xC1000000UL
#define NXK_POOL_WINDOW_PAGES  4096
PVOID NxPoolPagesAlloc(IN SIZE_T NumberOfBytes);
VOID NxPoolPagesSetMovable(IN PVOID BaseAddress, IN SIZE_T NumberOfBytes,
                           IN BOOLEAN Movable);
ULONG NxPoolPagesFree(IN PVOID BaseAddress);
ULONG NxPoolPagesCommitted(VOID);

/* Class counters behind MmQueryStatistics (defined in api.c).  Kernel
 * stacks are their own statistic class; the pool-window subset is
 * subtracted from PoolPagesCommitted so the classes stay disjoint. */
extern volatile LONG NxkMmStackPages;
extern volatile LONG NxkMmStackPagesInPool;
