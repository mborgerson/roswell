/*
 * PROJECT:     Xbox HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Initialize the x86 HAL
 * COPYRIGHT:   Copyright 1998 David Welch (welch@cwcom.net)
 */

/* INCLUDES *****************************************************************/

#include "halxbox.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

VOID
NTAPI
HalpInitProcessor(
    IN ULONG ProcessorNumber,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Set default IDR */
    KeGetPcr()->IDR = 0xFFFFFFFF & ~(1 << PIC_CASCADE_IRQ);
}

/*
 * 8259 EISA Edge/Level Control Register.  One byte per PIC; bit i selects
 * level (1) or edge (0) triggering for that IRQ.  System-reserved IRQs are
 * required to be edge: master 0 (timer), 1 (KBD/USB0), 2 (cascade); slave 8
 * (RTC), 13 (FPU error).
 *
 * We match the retail Xbox kernel's final ELCR programming exactly:
 *   master = 0x68 -> only IRQ3, IRQ5, IRQ6 are level
 *   slave  = 0x18 -> only IRQ11, IRQ12 are level
 * The retail kernel programs each IRQ as its driver is enabled, ending at
 * 0x68/0x18.
 *
 * Notably IRQ1 (OHCI USB0) is edge under both retail and ours.  The
 * level-retry no-op fix in pic.c covers the kernel side of
 * LevelSensitive-on-edge-PIC delivery; matching retail here removes a
 * divergence point that could affect other drivers if/when they're wired up.
 *
 * BIOSes program ELCR; cromwell and xemu's BIOS leave it 0.  The kernel owns
 * its machine configuration, so program it ourselves.
 */
#define NXK_ELCR_MASTER_PORT  0x4D0
#define NXK_ELCR_SLAVE_PORT   0x4D1
#define NXK_ELCR_MASTER_BITS  0x68  /* IRQ3,5,6 level; rest edge -- retail */
#define NXK_ELCR_SLAVE_BITS   0x18  /* IRQ11,12 level; rest edge -- retail  */

VOID NxkInitializeVideo(VOID);

VOID
HalpInitPhase0(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* NxConfigurePciDevices runs earlier in HalInitSystem, before
     * HalpCalibrateStallExecution -- the chipset has to be in retail
     * configuration before the kernel times anything against it. */

    /* See NXK_ELCR_* above for the why. */
    __outbyte(NXK_ELCR_MASTER_PORT, NXK_ELCR_MASTER_BITS);
    __outbyte(NXK_ELCR_SLAVE_PORT,  NXK_ELCR_SLAVE_BITS);

    /* part_xbox.c was the legacy HalIoReadPartitionTable hook that
     * synthesised an MBR layout from a fixed Xbox FATX table.  No
     * kept code calls IoReadPartitionTable any more -- partmgr,
     * disk/pnp, and IopCreateArcNames all use IoReadPartitionTableEx
     * (ntoskrnl/fstub/fstubex.c::FstubReadPartitionTableXbox), which
     * keeps the canonical FATX-offset table. */

    /* NV2A CRTC + TV encoder bring-up is deferred until after
     * NxkMmEnsureXboxWindows has identity-mapped the 0xFD000000 MMIO
     * window (called from ExpInitializeExecutive late in init).  At
     * HalpInitPhase0 the NV2A BAR is not yet reachable through the
     * kernel VA, so any MMIO write here faults. */
}

VOID
HalpInitPhase1(VOID)
{
    /* Enable timer interrupt handler */
    HalpEnableInterruptHandler(IDT_DEVICE,
                               0,
                               PRIMARY_VECTOR_BASE + PIC_TIMER_IRQ,
                               CLOCK2_LEVEL,
                               HalpClockInterrupt,
                               Latched);

    /* No RTC profile interrupt: profiling is never started here, so
     * IRQ8 stays masked with the unexpected-interrupt stub. */

    /* Initialize DMA. NT does this in Phase 0 */
    HalpInitDma();
}

/* EOF */
