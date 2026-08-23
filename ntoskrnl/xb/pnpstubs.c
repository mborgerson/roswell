/*
 * Cm* globals referenced by ex/init.c + po on SARCH=xbox.  All Cm, Iop,
 * Nt*Key, Rtl*Registry, and IoOpenDeviceRegistryKey functions are absent
 * -- their callers are gated under SARCH_XBOX and their imptab entries
 * are in xb/gen-import-table.py's SKIP_EXACT.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#ifdef SARCH_XBOX

ULONG CmNtGlobalFlag = 0;
CM_SYSTEM_CONTROL_VECTOR CmControlVector[] = { { NULL } };
POBJECT_TYPE CmpKeyObjectType = NULL;

/*
 * pciidex's PciIdeXPdoGetDmaAdapter calls IoGetDmaAdapter to wire up
 * the IDE channel's DMA path.  Upstream pnpmgr/pnpdma.c first probes
 * the PDO via IRP_MN_QUERY_INTERFACE(GUID_BUS_INTERFACE_STANDARD) and
 * falls back to HalGetDmaAdapter when no bus driver answers.  Our
 * synthetic PDOs don't answer with a DMA-adapter interface, so the
 * HAL adapter is always what callers would have landed on -- go there
 * directly.  Returning NULL strands pciidex in PIO mode, costing a large
 * chunk of DVD-streaming throughput.
 */
PDMA_ADAPTER
NTAPI
IoGetDmaAdapter(IN PDEVICE_OBJECT PhysicalDeviceObject,
                IN PDEVICE_DESCRIPTION DeviceDescription,
                IN OUT PULONG NumberOfMapRegisters)
{
    return HalGetDmaAdapter(PhysicalDeviceObject,
                            DeviceDescription,
                            NumberOfMapRegisters);
}

#endif /* SARCH_XBOX */
