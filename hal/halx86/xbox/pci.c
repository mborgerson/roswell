/*
 * PROJECT:     Xbox HAL (nxkrnl)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Program the Xbox PCI configuration to the retail-kernel layout
 *
 * nxkrnl does not trust the loader (cromwell, or anything else) to have set
 * the machine up correctly: the kernel programs its own machine configuration
 * to match the official Xbox kernel.  This file owns the PCI side of that --
 * every device's base-address registers and the 0:30.0 PCI-to-PCI bridge
 * windows, exactly as observed on the running retail kernel with xemu's
 * "info pci" monitor command.
 *
 * The Xbox south/north bridge is plain PCI: Type-1 configuration access
 * through I/O ports 0xCF8 (address) / 0xCFC (data).
 */

/* INCLUDES *****************************************************************/

#include "halxbox.h"

#define NDEBUG
#include <debug.h>

/* Cromwell PCI bring-up entry points -- defined in
 * xbox/pci-bringup/cromwell-pci.c.  These do the chipset / DMA / ACPI / SMC
 * delta the BAR + cmd tables below don't cover. */
extern void BootDetectMemorySize(void);
extern void BootAGPBUSInitialization(void);
extern void BootPciPeripheralInitialization(void);

/* PCI configuration-space accessors live in ntoskrnl/xb/pci.c.  HAL TUs
 * don't see ntoskrnl/include/internal/, so we forward-declare here. */
extern VOID PciCfgWrite32(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset, ULONG value);
extern VOID PciCfgWrite16(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset, USHORT value);

/* PCI command-register bits.  All values match retail exactly, captured by
 * reading the cmd register back via PCI config reads.
 *
 *   0x07 = I/O + memory + bus-master  -- the devices that DMA into RAM
 *                                        (USB OHCI x2, NIC, APU, NV2A) and
 *                                        the 0:30.0 PCI-to-PCI bridge (so it
 *                                        forwards bus-master cycles from
 *                                        secondary-bus NV2A).
 *   0x05 = I/O + bus-master           -- IDE (no MMIO; programmed I/O ATA).
 *   0x03 = I/O + memory               -- LPC bridge (no DMA).
 *   0x01 = I/O only                   -- SMBus (no DMA, no MMIO; just ports).
 *   0x07 = I/O + memory + bus-master  -- AC97 (uses MMIO for buffer engine).
 */
#define PCI_CMD_IO          0x0001
#define PCI_CMD_IO_MEM      0x0003
#define PCI_CMD_IO_BM       0x0005
#define PCI_CMD_IO_MEM_BM   0x0007

/* RETAIL PCI LAYOUT ********************************************************/

/*
 * Base-address registers.  Memory BARs take the bare base address (the
 * device's read-only type/prefetch bits in [3:0] are unaffected by a write);
 * I/O BARs likewise (bit 0 is hardwired to 1).
 */
typedef struct _NX_PCI_BAR
{
    UCHAR Bus, Slot, Func, Offset;
    ULONG Value;
} NX_PCI_BAR;

DATA_SEG("INITDATA")
static const NX_PCI_BAR NxPciBars[] =
{
    { 0,  1, 0, 0x10, 0x00008000 },   /* LPC bridge   -- I/O  */
    /* LPC chipset register at 0x4C is set to 0x0000FDDE by cromwell; some
     * later kernel code overwrites it to 0x000F0000.  Restore it here so
     * we match retail. (We don't know what 0x4C does -- it's nForce
     * chipset-specific -- but retail's value is what cromwell leaves and
     * what retail kernel keeps, so it's what we want.) */
    { 0,  1, 0, 0x4C, 0x0000FDDE },

    /*
     * LPC INT_IRQ_ROUT (offset 0x6C) -- chipset interrupt-routing matrix.
     * Each nibble assigns a legacy IRQ to one of the nForce internal
     * devices: USB0=[3:0], USB1=[7:4], NIC=[11:8], APU=[15:12],
     * ACI=[19:16], reserved=[23:20], IDE=[27:24], reserved=[31:28].
     *
     * Without this register set, none of those devices can deliver
     * interrupts to the PIC -- the chipset routes them "off", so a title's
     * vblank/audio init waits on an IRQ that never fires.
     *
     * 0x0E065491 = USB0->IRQ1, USB1->IRQ9, NIC->IRQ4, APU->IRQ5,
     *              ACI->IRQ6, IDE->IRQ14 -- matches cromwell and freeldr
     *              verbatim.
     */
    { 0,  1, 0, 0x6C, 0x0E065491 },
    { 0,  1, 1, 0x14, 0x0000C000 },   /* SMBus        -- I/O  */
    { 0,  2, 0, 0x10, 0xFED00000 },   /* USB0 (OHCI)  -- MMIO */
    { 0,  3, 0, 0x10, 0xFED08000 },   /* USB1 (OHCI)  -- MMIO */
    { 0,  4, 0, 0x10, 0xFEF00000 },   /* NIC          -- MMIO */
    { 0,  4, 0, 0x14, 0x0000E000 },   /* NIC          -- I/O  */
    { 0,  5, 0, 0x10, 0xFE800000 },   /* APU (audio)  -- MMIO */
    { 0,  6, 0, 0x18, 0xFEC00000 },   /* AC97         -- MMIO */
    { 0,  9, 0, 0x20, 0x0000FF60 },   /* IDE          -- I/O  */
    /* nForce chipset-specific channel-enable register at IDE 0x50.
     * Bit 1 = primary channel enabled, bit 0 = secondary.  Xbox uses only
     * the primary (HDD master + CDROM slave); secondary stays disabled.
     * Without this the kernel's pciidex driver reads 0x00 here, marks
     * BOTH channels disabled in PciIdeGetChannelState
     * (pciidex/chipset/pata_generic.c:961), returns 0 PDOs from
     * QueryBusRelations, atapi never attaches, no CDROM, no XBE load.
     * Cromwell sets this register; nxkrnl must too. */
    { 0,  9, 0, 0x50, 0x00000002 },   /* IDE channel enable -- primary on */

    /*
     * Per-device PCI Interrupt Line (config offset 0x3C, byte 0).
     *
     * The LPC INT_IRQ_ROUT (above) routes the chipset's internal device
     * IRQs to legacy lines.  But OS drivers read each device's own PCI
     * 0x3C to find out which IRQ to bind -- so this byte must also
     * advertise the same routing, or atapi/USBPORT/audio bind IRQ 0 and
     * never see an interrupt.
     *
     * Cromwell programs the same Line numbers the LPC matrix uses; we
     * mirror them.  Pin (byte 0x3D) is hardwired in the chipset and
     * unchanged by these writes.
     */
    { 0,  2, 0, 0x3C, 0x00000001 },   /* USB0 -> IRQ1 */
    { 0,  3, 0, 0x3C, 0x00000009 },   /* USB1 -> IRQ9 */
    { 0,  4, 0, 0x3C, 0x00000004 },   /* NIC  -> IRQ4 */
    { 0,  5, 0, 0x3C, 0x00000005 },   /* APU  -> IRQ5 */
    { 0,  6, 0, 0x3C, 0x00000006 },   /* ACI  -> IRQ6 */
    { 1,  0, 0, 0x10, 0xFD000000 },   /* NV2A GPU     -- MMIO */
    { 1,  0, 0, 0x14, 0xF0000000 },   /* NV2A GPU     -- prefetchable VRAM */
    /*
     * NV2A PCI 0x3C: Interrupt Line=3, Pin=1 (the upper bytes Min_Gnt /
     * Max_Lat are unused on this chipset).  The MCPX bootrom leaves the
     * PCI hardwired pin in place but the line register is zero -- without
     * us writing 3 here, KeQueryInterruptVector returns 0 for the GPU and
     * AvSendTVEncoderOption(GET_SETTINGS) blocks waiting for vblank that
     * never gets routed.  cromwell sets this to 0x0103 verbatim.
     */
    { 1,  0, 0, 0x3C, 0x00000103 },
    /* AGP bridge own interrupt line (0:30.0 PCI-to-PCI).  Cromwell sets
     * this to 7; otherwise the bridge reports line=0 to the OS.  The
     * bridge forwards bus-1 INTx# to a chipset PIRQ -- which legacy IRQ
     * is OS-visible. */
    { 0, 30, 0, 0x3C, 0x00000007 },
};

/* Devices whose PCI command register gets enabled.  See PCI_CMD_* above for
 * the I/O+memory-only vs +bus-master decision per device. */
typedef struct _NX_PCI_CMD
{
    UCHAR Bus, Slot, Func;
    USHORT Cmd;
} NX_PCI_CMD;

DATA_SEG("INITDATA")
static const NX_PCI_CMD NxPciDevices[] =
{
    { 0,  1, 0, PCI_CMD_IO_MEM    }, /* LPC bridge -- no DMA               */
    { 0,  1, 1, PCI_CMD_IO        }, /* SMBus      -- I/O only             */
    { 0,  2, 0, PCI_CMD_IO_MEM_BM }, /* USB0 (OHCI)                        */
    { 0,  3, 0, PCI_CMD_IO_MEM_BM }, /* USB1 (OHCI)                        */
    { 0,  4, 0, PCI_CMD_IO_MEM_BM }, /* NIC                                */
    { 0,  5, 0, PCI_CMD_IO_MEM_BM }, /* APU (audio)                        */
    { 0,  6, 0, PCI_CMD_IO_MEM_BM }, /* AC97                               */
    { 0,  9, 0, PCI_CMD_IO_BM     }, /* IDE        -- I/O + BM, no MMIO    */
    { 0, 30, 0, PCI_CMD_IO_MEM_BM }, /* PCI-to-PCI bridge (NV2A child)     */
    { 1,  0, 0, PCI_CMD_IO_MEM_BM }, /* NV2A GPU                           */
};

/*
 * 0:30.0 nForce PCI-to-PCI bridge.  The NV2A sits on the secondary bus, so
 * the bridge must forward bus 1 and the two memory windows that cover the
 * GPU's MMIO (0xFD000000) and VRAM aperture (0xF0000000) BARs.
 */
#define NX_BRIDGE_BUS       0
#define NX_BRIDGE_SLOT      30

/* offset 0x18: primary 0, secondary 1, subordinate 1, secondary latency 0 */
#define NX_BRIDGE_BUSNUM    0x00010100
/* offset 0x20: memory window 0xFD000000-0xFE7FFFFF (limit<<16 | base) */
#define NX_BRIDGE_MEM       0xFE70FD00
/* offset 0x24: prefetchable window 0xF0000000-0xF3FFFFFF */
#define NX_BRIDGE_PREFETCH  0xF3F0F000

/* PUBLIC ENTRY POINT *******************************************************/

VOID
NxConfigurePciDevices(VOID)
{
    ULONG i;

    /* Foundational chipset writes from cromwell's 2BL bootstrap
     * (boot_rom/2bBootStartup.S:355-370).  These run BEFORE the BARs in
     * the freeldr+cromwell baseline -- the 2BL does them in assembly
     * before handing off to BootStartBiosLoader -- so we have to do them
     * up front here too.  Without them the IDE/NIC chipset paths stay
     * disabled.
     *
     *   LPC 0x8C bit 30 -- "Enable IDE and NIC" per the 2BL comment.
     *   PCI host 0x80    -- "CPU Whoami" stamp = 0x100.
     */
    PciCfgWrite32(0, 1, 0, 0x8C, 0x40000000);
    PciCfgWrite32(0, 0, 0, 0x80, 0x00000100);

    /* Cromwell's 2BL also calls BootAGPBUSInitialization right after the
     * two 2BL-startup writes above (boot_rom/2bBootStartBios.c:60-72).
     * That toggles the AGP / HUB chipset link before the rest of the
     * machine config -- so we run it early too, *before* the BAR table.
     * cromwell's main BootResetAction runs it again later; the function
     * is idempotent. */
    BootAGPBUSInitialization();

    /* Program every device's base-address registers. */
    for (i = 0; i < RTL_NUMBER_OF(NxPciBars); i++)
    {
        PciCfgWrite32(NxPciBars[i].Bus, NxPciBars[i].Slot,
                         NxPciBars[i].Func, NxPciBars[i].Offset,
                         NxPciBars[i].Value);
    }

    /* Configure the PCI-to-PCI bridge: bus numbers and forwarding windows. */
    PciCfgWrite32(NX_BRIDGE_BUS, NX_BRIDGE_SLOT, 0, 0x18, NX_BRIDGE_BUSNUM);
    PciCfgWrite16(NX_BRIDGE_BUS, NX_BRIDGE_SLOT, 0, 0x1C, 0x0000);
    PciCfgWrite32(NX_BRIDGE_BUS, NX_BRIDGE_SLOT, 0, 0x20, NX_BRIDGE_MEM);
    PciCfgWrite32(NX_BRIDGE_BUS, NX_BRIDGE_SLOT, 0, 0x24, NX_BRIDGE_PREFETCH);
    PciCfgWrite32(NX_BRIDGE_BUS, NX_BRIDGE_SLOT, 0, 0x28, 0x00000000);
    PciCfgWrite32(NX_BRIDGE_BUS, NX_BRIDGE_SLOT, 0, 0x2C, 0x00000000);
    PciCfgWrite32(NX_BRIDGE_BUS, NX_BRIDGE_SLOT, 0, 0x30, 0x00000000);

    /* Enable I/O, memory, and (where retail does so) bus-master decoding. */
    for (i = 0; i < RTL_NUMBER_OF(NxPciDevices); i++)
    {
        PciCfgWrite16(NxPciDevices[i].Bus, NxPciDevices[i].Slot,
                         NxPciDevices[i].Func, 0x04, NxPciDevices[i].Cmd);
    }

    /* Ported cromwell bring-up: programs the AGP bridge, DMA controller,
     * ACPI block, SMC audio enable and the chipset/IDE/AC97 registers in
     * one shot.  Replaces our hand-written delta helpers.
     *
     * BootDetectMemorySize is deferred to NxConfigurePciDevicesLate --
     * it writes NV2A PFB MMIO at 0xFD100200/0xFD100204, which isn't
     * mapped into kernel VA until NxkMmEnsureXboxWindows runs much later. */
    BootAGPBUSInitialization();
    BootPciPeripheralInitialization();

    DPRINT1("PCI configuration programmed to the retail layout\n");
}

/*
 * Late-phase PCI bring-up -- the bring-up steps cromwell does in
 * BootDetectMemorySize that have to wait until the 0xFD000000 UC MMIO
 * window is mapped.  Called from ExpInitializeExecutive just after
 * NxkMmEnsureXboxWindows, before NxkInitializeVideo touches NV2A MMIO.
 *
 * We reproduce cromwell's BootDetectMemorySize step-for-step EXCEPT the
 * pattern probe in the middle.  Cromwell's probe writes 0xAA / 0x55 to
 * VA (void*)(64*1024*1024) = PA 0x04000000 -- which is exactly where the
 * kernel image lives.  Running the probe in this configuration would
 * overwrite the running kernel mid-boot.  We instead trust the
 * kernel-shrink prerequisite gate
 * (xbox_ram set statically to 128 in xbox-video-support.c) and keep the
 * surrounding register writes verbatim.
 */
extern unsigned int xbox_ram;

VOID
NxConfigurePciDevicesLate(VOID)
{
    /* NV2A PFB timing config -- matches cromwell BootDetectMemorySize. */
    WRITE_REGISTER_ULONG((PULONG)0xFD100200, 0x03070103);
    WRITE_REGISTER_ULONG((PULONG)0xFD100204, 0x11448000);

    /* Host bridge 0:0.0 reg 0x84: memory-top.  Cromwell opens with the
     * 128 MiB value, runs the probe, then writes the final value based on
     * what was detected.  We skip the probe but keep the final write. */
    PciCfgWrite32(0, 0, 0, 0x84, 0x07FFFFFF);    /* opening: 128 MiB */

    if (xbox_ram == 64)
        PciCfgWrite32(0, 0, 0, 0x84, 0x03FFFFFF);
    else if (xbox_ram == 128)
        PciCfgWrite32(0, 0, 0, 0x84, 0x07FFFFFF);

    /* Linker keep marker for the verbatim cromwell symbol; the call site
     * is the surrounding code, not a direct invocation. */
    (void)&BootDetectMemorySize;

    DPRINT1("PCI memory-size programmed (xbox_ram = %u MiB)\n", xbox_ram);
}

/* EOF */
