/*
 * Page-table setup for the loader.
 *
 * The page directory is pinned at the retail Xbox CR3 (PD_PA, 0x0F000).  The
 * low PCR backing / LoaderBlock / Aux pages sit just above it, while the page
 * tables themselves cluster near the top of 128 MB RAM so the low arena stays
 * free for the title heap.  Concrete PAs come from boot-layout.h (NXBL_*):
 *
 *   PA 0x0000F000        PD (1 page) -- pinned at the retail Xbox CR3.
 *   PA 0x00030000        PCR region (16 pages) backing VA 0xFFDF0000..
 *   PA 0x00040000        LoaderBlock
 *   PA 0x00041000        LoaderAux (extension + ldr entry + descriptors)
 *   PA 0x07F00000        PT_LOW   -- 4 PTs, identity-maps PA 0..16MB (transient
 *                                    -- killed by MmArmInitSystem user-PDE zero)
 *   PA 0x07F04000        PT_KSEG0 -- 17 PTs, maps VA 0x80000000..0x84400000
 *                                    to PA 0x0000000..0x4400000.
 *   PA 0x07F15000        PT_PCR   -- 1 PT for VA 0xFFDF0000..0xFFE00000
 *
 * After PagingEnable(), the identity map keeps our loader's text/data/stack
 * working; the kernel imagebase is reachable; KIP0PCRADDRESS = 0xFFDFF000
 * resolves to NXBL_PCR_REGION_PA; and CR3 = PD_PA matches the value the retail
 * Xbox kernel uses.  The persistent page tables are placed at the top of RAM.
 *
 * Accessing these pages doesn't need KSEG0: the CPU page-walker uses raw PA
 * from PD entries, and the kernel reads/writes PTs through the recursive
 * self-map at VA 0xC0000000+ (PD[0x300] -> PD).  PCR_REGION is reached via
 * VA 0xFFDF0000 (PT_PCR maps it).
 */

#include "paging.h"
#include "boot-layout.h"
#include "log.h"
#include "nxldr.h"

void
PagingInit(void)
{
    ULONG *Pd = (ULONG *)PD_PA;
    ULONG *Pt;
    ULONG i, j;

    dprintf("pi0\n");

    /* Clear PD. */
    memset(Pd, 0, PAGE_SIZE);
    dprintf("pi1\n");

    /* Identity map low 16 MB across 4 PTs.  This is transient -- it
     * gives the loader direct PA pointers between PagingEnable() and
     * the handoff, and it dies when MmArmInitSystem zeros user-mode
     * PDEs.  All long-lived structures (LoaderBlock, kernel image) must
     * also be reachable via KSEG0 below. */
    for (i = 0; i < 4; i++)
    {
        ULONG *PtId = (ULONG *)(NXBL_PT_LOW_PA + i * PAGE_SIZE);
        for (j = 0; j < 1024; j++)
        {
            ULONG Pa = (i * 0x400000) + (j * PAGE_SIZE);
            PtId[j] = Pa | PTE_FLAGS;
        }
        Pd[i] = ((ULONG)PtId) | PTE_FLAGS;
    }
    dprintf("pi2\n");

    /* KSEG0: NT's expected high-half identity for loader pages.
     * VA (0x80000000 | PA) -> PA, covering PA 0..0x4400000 (68 MB).
     * 17 PTs at PD[0x200..0x210].  This also maps the kernel image
     * (PA 0x4000000..0x4400000 = VA 0x84000000..0x84400000) the way
     * NT expects -- no separate kernel-only PT.  Loader-spanned range
     * matches LoaderBlock->Extension->LoaderPagesSpanned exactly, so
     * MmSystemPteSpaceStart (= KSEG0 + MmBootImageSize) lands at
     * PDE 0x211 right above our last KSEG0 PT.
     *
     * KSEG0 PDEs (0x200..0x210) survive the user-PDE zero in
     * MmArmInitSystem -- the kernel reuses our maps as its KSEG0. */
    for (i = 0; i < NXBL_PT_KSEG0_COUNT; i++)
    {
        ULONG *PtKseg0 = (ULONG *)(NXBL_PT_KSEG0_PA + i * PAGE_SIZE);
        for (j = 0; j < 1024; j++)
        {
            ULONG Pa = (i * 0x400000) + (j * PAGE_SIZE);
            PtKseg0[j] = Pa | PTE_FLAGS;
        }
        Pd[PD_INDEX(KSEG0_BASE) + i] = ((ULONG)PtKseg0) | PTE_FLAGS;
    }

    /* Recursive self-map: PD[0x300] -> PD itself.  Makes VA range
     * 0xC0000000..0xC0400000 act as the page-table aperture, so
     * MiAddressToPte(va) = PTE_BASE + (va >> 12)*4 just works.  The
     * kernel's KiMarkPageAsReadOnly (and most of Mm) walks page
     * tables through this aperture; without it the very first
     * post-KdInit step page-faults silently. */
    Pd[0x300] = PD_PA | PTE_FLAGS;
    dprintf("pi3\n");

    /* High-reserve region: VA 0xFFDF0000 .. 0xFFE00000 (16 pages) covers
     * KI_USER_SHARED_DATA (0xFFDF0000) + the KPCR/KPRCB pages up to and
     * including KIP0PCRADDRESS (0xFFDFF000).  All zero-filled; the kernel
     * populates KUSER_SHARED_DATA + KPCR in its own init. */
    Pt = (ULONG *)NXBL_PT_PCR_PA;
    memset(Pt, 0, PAGE_SIZE);
    for (i = 0; i < PCR_REGION_PAGES; i++)
    {
        Pt[PT_INDEX(PCR_REGION_VA) + i] = (NXBL_PCR_REGION_PA + i * PAGE_SIZE) | PTE_FLAGS;
    }
    Pd[PD_INDEX(PCR_REGION_VA)] = NXBL_PT_PCR_PA | PTE_FLAGS;
    memset((void *)NXBL_PCR_REGION_PA, 0, PCR_REGION_PAGES * PAGE_SIZE);
    dprintf("pi4\n");
}

void
PagingEnable(void)
{
    ULONG cr0, cr4;
    dprintf("pe0\n");
    __asm__ __volatile__("mov %0, %%cr3" ::"r"(PD_PA));
    /* CR4.PSE (bit 4): the kernel's NxkMmEnsureXboxWindows / EnsureMmioWindow
     * paint 4 MB PSE PDEs from very early in Phase1Initialization, but ReactOS
     * only enables CR4.PSE much later inside Ki386EnableCurrentLargePage
     * (called from KeInitSystem).  Set it here so PSE PDEs are interpreted as
     * large pages from the moment the kernel takes over -- otherwise the CPU
     * walks the "PT" at the PSE PFN (which for UC MMIO is device space) and
     * faults.  Idempotent with Ki386EnableCurrentLargePage's later enable. */
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x10;
    __asm__ __volatile__("mov %0, %%cr4" ::"r"(cr4));
    dprintf("pe1\n");
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ __volatile__("mov %0, %%cr0" ::"r"(cr0));
    dprintf("pe2\n");
    __asm__ __volatile__("jmp 1f; 1:");
    dprintf("pe3\n");
}
