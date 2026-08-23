/*
* PROJECT:         ReactOS Kernel
* LICENSE:         GPL - See COPYING in the top level directory
* FILE:            ntoskrnl/ke/i386/mtrr.c
* PURPOSE:         Support for MTRR and AMD K6 MTRR
* PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
*/

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* Variable-range MTRR MSRs: PHYSBASEn = 0x200 + 2n, PHYSMASKn = base + 1. */
#define MSR_MTRR_PHYSBASE0      0x200
#define MSR_MTRR_DEF_TYPE       0x2FF
#define MTRR_DEF_TYPE_ENABLE    0x800        /* "E" -- MTRRs enabled */

/*
 * The Xbox has one fixed CPU over one fixed memory map, so its MTRRs are a
 * constant.  We mirror the retail kernel's configuration because the
 * bootloader leaves a different one behind:
 *
 *   Var0  0x00000000-0x03FFFFFF   64 MB   WB  -- RAM
 *   Var1  0xFFF80000-0xFFFFFFFF  512 KB   WP  -- flash ROM
 *   Var2..7                              off
 *   default                              UC
 *
 * Write-combining (the GPU framebuffer/pushbuffer) is granted through
 * IA32_PAT, not a WC MTRR -- see KiInitializePAT().
 */
typedef struct _KI_MTRR_RANGE
{
    ULONG Msr;              /* PHYSBASE MSR; PHYSMASK is Msr + 1 */
    ULONGLONG PhysBase;
    ULONGLONG PhysMask;
} KI_MTRR_RANGE;

DATA_SEG("INITDATA")
static const KI_MTRR_RANGE KiXboxMtrr[] =
{
    { MSR_MTRR_PHYSBASE0 +  0, 0x0000000000000006ULL, 0x0000000FFC000800ULL },
    { MSR_MTRR_PHYSBASE0 +  2, 0x00000000FFF80005ULL, 0x0000000FFFF80800ULL },
    { MSR_MTRR_PHYSBASE0 +  4, 0x0000000000000000ULL, 0x0000000000000000ULL },
    { MSR_MTRR_PHYSBASE0 +  6, 0x0000000000000000ULL, 0x0000000000000000ULL },
    { MSR_MTRR_PHYSBASE0 +  8, 0x0000000000000000ULL, 0x0000000000000000ULL },
    { MSR_MTRR_PHYSBASE0 + 10, 0x0000000000000000ULL, 0x0000000000000000ULL },
    { MSR_MTRR_PHYSBASE0 + 12, 0x0000000000000000ULL, 0x0000000000000000ULL },
    { MSR_MTRR_PHYSBASE0 + 14, 0x0000000000000000ULL, 0x0000000000000000ULL },
};

/* FUNCTIONS *****************************************************************/

CODE_SEG("INIT")
VOID
NTAPI
KiInitializeMTRR(IN BOOLEAN FinalCpu)
{
    BOOLEAN Enable;
    ULONG Cr0, Cr3, Cr4, i;
    ULONGLONG DefType;

    /* The Xbox is uniprocessor, so FinalCpu is always TRUE; MTRRs are still
     * per-CPU state, so this programs whichever CPU is running it. */
    UNREFERENCED_PARAMETER(FinalCpu);

    /*
     * Rewriting MTRRs that already govern cached lines must run with interrupts
     * off, caches in no-fill mode and the MTRRs disabled, so no stale cache
     * line or TLB entry outlives the change.
     */
    Enable = KeDisableInterrupts();

    /* Drop global pages so the CR3 reloads below flush the whole TLB, then
     * enter no-fill cache mode (CR0.CD=1, CR0.NW=0). */
    Cr4 = __readcr4();
    __writecr4(Cr4 & ~CR4_PGE);
    Cr3 = __readcr3();
    Cr0 = __readcr0();
    __writecr0((Cr0 & ~CR0_NW) | CR0_CD);
    __wbinvd();
    __writecr3(Cr3);

    /* Disable the MTRRs while they are rewritten. */
    DefType = __readmsr(MSR_MTRR_DEF_TYPE);
    __writemsr(MSR_MTRR_DEF_TYPE, DefType & ~(ULONGLONG)MTRR_DEF_TYPE_ENABLE);

    /* Mirror the retail kernel's variable MTRRs -- all eight, so whatever
     * the bootloader left in the unused slots is cleared too. */
    for (i = 0; i < RTL_NUMBER_OF(KiXboxMtrr); i++)
    {
        __writemsr(KiXboxMtrr[i].Msr,     KiXboxMtrr[i].PhysBase);
        __writemsr(KiXboxMtrr[i].Msr + 1, KiXboxMtrr[i].PhysMask);
    }

    /* Re-enable: default type UC, fixed MTRRs off (MTRRdefType = 0x800). */
    __writemsr(MSR_MTRR_DEF_TYPE, MTRR_DEF_TYPE_ENABLE);

    /* Flush again, leave no-fill mode and restore CR0/CR4. */
    __wbinvd();
    __writecr3(Cr3);
    __writecr0(Cr0);
    __writecr4(Cr4);

    KeRestoreInterrupts(Enable);

    DPRINT1("MTRRs set to the Xbox retail configuration\n");
}

CODE_SEG("INIT")
VOID
NTAPI
KiAmdK6InitializeMTRR(VOID)
{
    /* The Xbox CPU is Intel; this AMD K6 path is never taken. */
    DPRINT("AMD MTRR support detected but not yet taken advantage of\n");
}
