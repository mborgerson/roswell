/*
 * Direct PCI configuration access via legacy ports 0xCF8/0xCFC.
 *
 * The HAL's BusHandler-based PCI interface (HalGetBusDataByOffset etc.)
 * relies on \Registry\Machine\Hardware\Description\System\MultiFunctionAdapter
 * being populated, which happens during PnP init.  Once PnP enumeration is
 * a no-op, that registry key is never written and HAL's PCI accessors all
 * return 0.  These helpers go straight to Type-1 config-space ports so the
 * static device tree (and HAL bring-up) can read/write VID/DID, BAR
 * contents, and the command register without depending on PnP being alive.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#ifdef SARCH_XBOX

#include "internal/nx-devtree.h"

#define PCI_CFG_ADDR    0xCF8
#define PCI_CFG_DATA    0xCFC

static ULONG
PciCfgAddress(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset)
{
    return 0x80000000UL |
           ((ULONG)bus << 16) |
           ((ULONG)dev << 11) |
           ((ULONG)fn  << 8)  |
           (offset & 0xFC);
}

ULONG
PciCfgRead32(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset)
{
    WRITE_PORT_ULONG((PULONG)PCI_CFG_ADDR,
                     PciCfgAddress(bus, dev, fn, offset));
    return READ_PORT_ULONG((PULONG)PCI_CFG_DATA);
}

VOID
PciCfgWrite32(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset, ULONG value)
{
    WRITE_PORT_ULONG((PULONG)PCI_CFG_ADDR,
                     PciCfgAddress(bus, dev, fn, offset));
    WRITE_PORT_ULONG((PULONG)PCI_CFG_DATA, value);
}

USHORT
PciCfgRead16(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset)
{
    ULONG word = PciCfgRead32(bus, dev, fn, offset & ~3);
    return (USHORT)(word >> ((offset & 2) * 8));
}

VOID
PciCfgWrite16(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset, USHORT value)
{
    WRITE_PORT_ULONG((PULONG)PCI_CFG_ADDR,
                     PciCfgAddress(bus, dev, fn, offset));
    WRITE_PORT_USHORT((PUSHORT)(PCI_CFG_DATA + (offset & 2)), value);
}

UCHAR
PciCfgRead8(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset)
{
    ULONG word = PciCfgRead32(bus, dev, fn, offset & ~3);
    return (UCHAR)(word >> ((offset & 3) * 8));
}

#endif /* SARCH_XBOX */
