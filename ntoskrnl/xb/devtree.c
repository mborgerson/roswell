/*
 * Static Xbox device tree -- the hand-rolled topology that replaces PnP
 * enumeration on SARCH=xbox.  Hardware is fixed so the device list is a
 * constant array.
 *
 * NxkDriveDeviceTree() walks the table, fabricates a synthetic parent
 * PDO per driver-bound entry, invokes the folded driver's AddDevice,
 * then sends IRP_MN_START_DEVICE down the new stack.  After START
 * succeeds, it queries the FDO for BusRelations and drives the next
 * layer (channel -> ata device -> disk/cdrom -> partmgr) the same way
 * PnP's recursive descent would.
 */

#include <ntoskrnl.h>
#include <wdmguid.h>
#define NDEBUG
#include <debug.h>

#ifdef SARCH_XBOX

#include "internal/nx-devtree.h"

/* Folded driver entry points (see io/iomgr/driver.c). */
extern DRIVER_INITIALIZE PciDriverEntry;
extern DRIVER_INITIALIZE PciideDriverEntry;
extern DRIVER_INITIALIZE AtapiDriverEntry;
extern DRIVER_INITIALIZE DiskDriverEntry;
extern DRIVER_INITIALIZE CdromDriverEntry;
extern DRIVER_INITIALIZE PartmgrDriverEntry;
extern DRIVER_INITIALIZE MountmgrDriverEntry;

#define NXK_NO_BDF 0xff

/* Per-device resource lists.  IRQs come from xemu's xbox_lpc_map_irq();
 * IDE IO ports are the fixed legacy primary/secondary channels.  PCI MMIO
 * BARs are dynamic -- the synthetic PDO reads them from PCI config at
 * QueryResources time.
 *
 * The tree and its resource arrays live in INITDATA: their only readers
 * are the INIT enumeration pass and the QUERY_RESOURCES handlers, whose
 * IRPs are only sent by that same pass (BuildResourceList is already
 * INIT on the same argument).  Runtime config access (BusGet/SetData)
 * goes through the BDF copied into the PDO extension, not the tree. */

DATA_SEG("INITDATA")
static const struct nxk_resource NxkResOhci0[] = {
    { NXK_RES_IRQ, 1, 1 },
};

DATA_SEG("INITDATA")
static const struct nxk_resource NxkResOhci1[] = {
    { NXK_RES_IRQ, 9, 1 },
};

DATA_SEG("INITDATA")
static const struct nxk_resource NxkResNvnet[] = {
    { NXK_RES_IRQ, 4, 1 },
};

DATA_SEG("INITDATA")
static const struct nxk_resource NxkResNvapu[] = {
    { NXK_RES_IRQ, 5, 1 },
};

DATA_SEG("INITDATA")
static const struct nxk_resource NxkResAci[] = {
    { NXK_RES_IRQ, 6, 1 },
};

DATA_SEG("INITDATA")
static const struct nxk_resource NxkResIde[] = {
    { NXK_RES_IO,  0x1F0,  8 },
    { NXK_RES_IO,  0x3F6,  1 },
    { NXK_RES_IRQ, 14,     1 },
    { NXK_RES_IO,  0x170,  8 },
    { NXK_RES_IO,  0x376,  1 },
    { NXK_RES_IRQ, 15,     1 },
    /* BAR4: bus master DMA register file (HAL bringup programs the BAR to
     * I/O 0xFF60).  pciidex maps BAR4 into AccessRange[4]; without it
     * PciIdeControllerInitDma stamps every channel down to PIO and DVD
     * reads become CPU-bound. */
    { NXK_RES_IO,  0xFF60, 16 },
};

DATA_SEG("INITDATA")
static const struct nxk_resource NxkResNv2a[] = {
    { NXK_RES_IRQ, 3, 1 },
};

#define NXK_RES(arr) (arr), (ULONG)RTL_NUMBER_OF(arr)

DATA_SEG("INITDATA")
const struct nxk_device NxkDeviceTree[] = {
    /* PCI host bridge */
    { "PCI Host Bridge",  PciDriverEntry, L"\\Driver\\Pci",
      0, 0x00, 0, 0x10DE, 0x02A5, NULL, 0 },

    /* LPC bridge -- HAL pokes config directly, no kernel driver. */
    { "LPC Bridge",       NULL, NULL,
      0, 0x01, 0, 0x10DE, 0x01B2, NULL, 0 },

    /* SMBus -- SMC / video encoder / temp sensor talk over it from XAPI. */
    { "SMBus",            NULL, NULL,
      0, 0x01, 1, 0x10DE, 0x01B4, NULL, 0 },

    /* OHCI USB controllers -- titles drive these via XAPI USBD. */
    { "OHCI USB 0",       NULL, NULL,
      0, 0x02, 0, 0x10DE, 0x01C2, NXK_RES(NxkResOhci0) },
    { "OHCI USB 1",       NULL, NULL,
      0, 0x03, 0, 0x10DE, 0x01C2, NXK_RES(NxkResOhci1) },

    /* NVNet -- title-side XNet stack. */
    { "NVNet",            NULL, NULL,
      0, 0x04, 0, 0x10DE, 0x01C3, NXK_RES(NxkResNvnet) },

    /* MCPX APU -- DSound talks to it directly. */
    { "NVAPU",            NULL, NULL,
      0, 0x05, 0, 0x10DE, 0x01B0, NXK_RES(NxkResNvapu) },

    /* MCPX ACI (AC'97). */
    { "ACI (AC'97)",      NULL, NULL,
      0, 0x06, 0, 0x10DE, 0x01B1, NXK_RES(NxkResAci) },

    /* AGP-to-PCI bridge.  Devfn 0x1e on real hardware and in xemu. */
    { "AGP Bridge",       PciDriverEntry, L"\\Driver\\Pci",
      0, 0x1E, 0, 0x10DE, 0x01B7, NULL, 0 },

    /* IDE controller -- pciide attaches; atapi enumerates the channels. */
    { "IDE Controller",   PciideDriverEntry, L"\\Driver\\pciide",
      0, 0x09, 0, 0x10DE, 0x01BC, NXK_RES(NxkResIde) },

    /* NV2A GPU on the secondary AGP bus. */
    { "NV2A GPU",         NULL, NULL,
      1, 0x00, 0, 0x10DE, 0x02A0, NXK_RES(NxkResNv2a) },
};

DATA_SEG("INITDATA")
const ULONG NxkDeviceTreeCount = RTL_NUMBER_OF(NxkDeviceTree);

static const char *
ResKindName(enum nxk_res_kind k)
{
    switch (k) {
    case NXK_RES_IO:   return "io  ";
    case NXK_RES_MMIO: return "mmio";
    case NXK_RES_IRQ:  return "irq ";
    }
    return "????";
}

/* Resolve \Driver\<name> in the object namespace; ObReferenceObjectByName
 * returns a referenced PDRIVER_OBJECT we deliberately leak (driver objects
 * live forever). */
static PDRIVER_OBJECT
ResolveDriver(const WCHAR *name)
{
    UNICODE_STRING uname;
    PDRIVER_OBJECT drv = NULL;
    NTSTATUS status;

    RtlInitUnicodeString(&uname, name);
    status = ObReferenceObjectByName(&uname,
                                     OBJ_CASE_INSENSITIVE,
                                     NULL,
                                     0,
                                     IoDriverObjectType,
                                     KernelMode,
                                     NULL,
                                     (PVOID *)&drv);
    if (!NT_SUCCESS(status))
        return NULL;
    return drv;
}

/* Diagnostic dump only -- safe to keep around once NxkDriveDeviceTree
 * runs the real work. */
VOID
NxkInitDeviceTree(VOID)
{
    ULONG i, j;
    ULONG total_res = 0;

    DPRINT("[devtree] static Xbox device tree (count=%u)\n",
             NxkDeviceTreeCount);

    for (i = 0; i < NxkDeviceTreeCount; i++) {
        const struct nxk_device *d = &NxkDeviceTree[i];
        PDRIVER_OBJECT drv = NULL;
        PDEVICE_OBJECT fdo = NULL;

        if (d->driver_name)
            drv = ResolveDriver(d->driver_name);

        if (drv)
            fdo = drv->DeviceObject;

        DPRINT("[devtree] entry %02u: BDF=%02x:%02x.%x VID=%04x DID=%04x"
                 " name=%s driver=%p fdo=%p res=%u\n",
                 i, d->bus, d->dev, d->fn, d->vendor, d->device,
                 d->name ? d->name : "(null)",
                 drv, fdo, d->res_count);

        if (d->bus != NXK_NO_BDF) {
            USHORT vid = PciCfgRead16(d->bus, d->dev, d->fn, 0x00);
            USHORT did = PciCfgRead16(d->bus, d->dev, d->fn, 0x02);
            const char *match = (vid == d->vendor && did == d->device)
                              ? "ok" : "MISMATCH";
            DPRINT("[devtree]   cfg vid=%04x did=%04x %s\n", vid, did, match);

            for (j = 0; j < 6; j++) {
                ULONG bar = PciCfgRead32(d->bus, d->dev, d->fn,
                                            (UCHAR)(0x10 + j * 4));
                if (bar != 0)
                    DPRINT("[devtree]   bar%u=%08x\n", j, bar);
            }
        }

        for (j = 0; j < d->res_count; j++) {
            const struct nxk_resource *r = &d->res[j];
            DPRINT("[devtree]   res[%u] %s start=%08x len=%08x\n",
                     j, ResKindName(r->kind), r->start, r->length);
        }
        total_res += d->res_count;
    }

    DPRINT("[devtree] done: %u devices, %u resources\n",
             NxkDeviceTreeCount, total_res);
}

/*
 * Translate a raw CM_RESOURCE_LIST in place so it matches what NT's PnP
 * arbiter would feed to IRP_MN_START_DEVICE.  Folded drivers (e.g.
 * pciidex's PciIdeConnectInterrupt) hand the list's Interrupt.Vector /
 * Interrupt.Level straight to IoConnectInterrupt, and raw values (IRQ 14,
 * etc.) get rejected by KeConnectInterrupt with STATUS_INVALID_PARAMETER
 * -- only the HAL-translated kernel vector / IRQL pair is accepted.
 * Mirrors ntoskrnl/io/pnpmgr/pnpres.c without the DEVICE_NODE dependency
 * so it can run with PnP no-op'd.
 */
static NTSTATUS
TranslateResourceList(
    _Inout_ PCM_RESOURCE_LIST ResourceList)
{
    PCM_FULL_RESOURCE_DESCRIPTOR Full;
    ULONG i, j;

    if (!ResourceList)
        return STATUS_INVALID_PARAMETER;

    Full = &ResourceList->List[0];
    for (i = 0; i < ResourceList->Count; i++) {
        PCM_PARTIAL_RESOURCE_LIST Partial = &Full->PartialResourceList;
        INTERFACE_TYPE iface = Full->InterfaceType;
        ULONG bus = Full->BusNumber;

        for (j = 0; j < Partial->Count; j++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc =
                &Partial->PartialDescriptors[j];

            switch (Desc->Type) {
            case CmResourceTypePort: {
                ULONG space = 1; /* I/O space */
                PHYSICAL_ADDRESS xlat;
                /* PCI IO is identity-mapped on Xbox; HAL's PCIBus
                 * translator rejects sub-window addresses (legacy ATA
                 * 0x1F0 etc.).  Accept its result when it works and fall
                 * back to identity otherwise so one rejected entry
                 * doesn't nullify the whole list. */
                if (HalTranslateBusAddress(iface, bus,
                                           Desc->u.Port.Start,
                                           &space, &xlat)) {
                    Desc->u.Port.Start = xlat;
                    if (space == 0)
                        Desc->Flags = CM_RESOURCE_PORT_MEMORY;
                }
                break;
            }
            case CmResourceTypeMemory: {
                ULONG space = 0; /* memory space */
                PHYSICAL_ADDRESS xlat;
                if (HalTranslateBusAddress(iface, bus,
                                           Desc->u.Memory.Start,
                                           &space, &xlat)) {
                    Desc->u.Memory.Start = xlat;
                }
                break;
            }
            case CmResourceTypeInterrupt: {
                KIRQL irql;
                KAFFINITY aff = 0;
                ULONG vec = HalGetInterruptVector(iface, bus,
                                                  Desc->u.Interrupt.Level,
                                                  Desc->u.Interrupt.Vector,
                                                  &irql, &aff);
                /* Folded drivers entering through the static devtree treat PCI
                 * device IRQs as ISA-style (no PIRQ routing on Xbox); the
                 * PCIBus translator returns 0.  Fall back to Isa, which
                 * is what HAL programs the PIC against. */
                if (vec == 0 && iface == PCIBus) {
                    vec = HalGetInterruptVector(Isa, 0,
                                                Desc->u.Interrupt.Level,
                                                Desc->u.Interrupt.Vector,
                                                &irql, &aff);
                }
                if (vec == 0)
                    return STATUS_UNSUCCESSFUL;
                Desc->u.Interrupt.Vector = vec;
                Desc->u.Interrupt.Level = irql;
                Desc->u.Interrupt.Affinity = aff;
                break;
            }
            default:
                break;
            }
        }

        Full = (PCM_FULL_RESOURCE_DESCRIPTOR)
               &Partial->PartialDescriptors[Partial->Count];
    }

    return STATUS_SUCCESS;
}

/*
 * ============================================================================
 * Synthetic parent PDO
 * ============================================================================
 *
 * Each driver-bound entry in NxkDeviceTree[] gets a PDO created by
 * IoCreateDevice.  Its DeviceExtension is a NXK_PDO_EXT; its DriverObject
 * is a "bus" placeholder we lazily create once (BusDriverObject).  The
 * single dispatch routine answers the small set of PnP minor codes folded
 * drivers need to AddDevice + START on top of this PDO.
 *
 * Pattern follows pnproot.c's PDO -- minimum surface to make the folded
 * drivers happy, returns the requested status verbatim for anything we
 * don't implement.
 */

typedef struct _NXK_PDO_EXT {
    const struct nxk_device *Entry;
    UCHAR Bus, Dev, Fn;
    BUS_INTERFACE_STANDARD BusIf;
} NXK_PDO_EXT, *PNXK_PDO_EXT;

static PDRIVER_OBJECT BusDriverObject = NULL;

/* GetBusData / SetBusData shims: BusIf.Context is the PDO; pull the BDF
 * from the extension and forward to PciCfg*. */
static ULONG NTAPI
BusGetData(_Inout_opt_ PVOID Context,
               _In_ ULONG DataType,
               _Inout_updates_bytes_(Length) PVOID Buffer,
               _In_ ULONG Offset,
               _In_ ULONG Length)
{
    PDEVICE_OBJECT pdo = (PDEVICE_OBJECT)Context;
    PNXK_PDO_EXT ext;
    PUCHAR dst = (PUCHAR)Buffer;
    ULONG i;

    if (!pdo || DataType != PCI_WHICHSPACE_CONFIG || !Buffer)
        return 0;

    ext = (PNXK_PDO_EXT)pdo->DeviceExtension;

    for (i = 0; i < Length; i++) {
        dst[i] = PciCfgRead8(ext->Bus, ext->Dev, ext->Fn,
                                (UCHAR)(Offset + i));
    }
    return Length;
}

static ULONG NTAPI
BusSetData(_Inout_opt_ PVOID Context,
               _In_ ULONG DataType,
               _Inout_updates_bytes_(Length) PVOID Buffer,
               _In_ ULONG Offset,
               _In_ ULONG Length)
{
    PDEVICE_OBJECT pdo = (PDEVICE_OBJECT)Context;
    PNXK_PDO_EXT ext;
    PUCHAR src = (PUCHAR)Buffer;
    ULONG i, base;

    if (!pdo || DataType != PCI_WHICHSPACE_CONFIG || !Buffer)
        return 0;

    ext = (PNXK_PDO_EXT)pdo->DeviceExtension;

    /* Only DWORD-aligned writes are well-defined through CF8/CFC; widen
     * out to dword RMW for unaligned tails. */
    for (i = 0; i + 4 <= Length && ((Offset + i) & 3) == 0; i += 4) {
        ULONG v = (ULONG)src[i] |
                  ((ULONG)src[i + 1] << 8) |
                  ((ULONG)src[i + 2] << 16) |
                  ((ULONG)src[i + 3] << 24);
        PciCfgWrite32(ext->Bus, ext->Dev, ext->Fn,
                         (UCHAR)(Offset + i), v);
    }
    for (; i < Length; i++) {
        base = (Offset + i) & ~3u;
        ULONG word = PciCfgRead32(ext->Bus, ext->Dev, ext->Fn,
                                     (UCHAR)base);
        ULONG shift = ((Offset + i) & 3) * 8;
        word = (word & ~(0xFFu << shift)) |
               ((ULONG)src[i] << shift);
        PciCfgWrite32(ext->Bus, ext->Dev, ext->Fn,
                         (UCHAR)base, word);
    }
    return Length;
}

static VOID NTAPI
BusIfRef(_Inout_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static VOID NTAPI
BusIfDeref(_Inout_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static BOOLEAN NTAPI
BusTranslate(_Inout_opt_ PVOID Context,
                 _In_ PHYSICAL_ADDRESS BusAddress,
                 _In_ ULONG Length,
                 _Inout_ PULONG AddressSpace,
                 _Out_ PPHYSICAL_ADDRESS TranslatedAddress)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Length);
    return HalTranslateBusAddress(PCIBus, 0, BusAddress, AddressSpace,
                                  TranslatedAddress);
}

static PDMA_ADAPTER NTAPI
BusGetDmaAdapter(_Inout_opt_ PVOID Context,
                     _In_ PDEVICE_DESCRIPTION DeviceDescription,
                     _Out_ PULONG NumberOfMapRegisters)
{
    UNREFERENCED_PARAMETER(Context);
    return HalGetDmaAdapter(NULL, DeviceDescription, NumberOfMapRegisters);
}

/* Build a minimal "no resources" CM_RESOURCE_LIST -- one full descriptor
 * with zero partial descriptors.  Drivers expecting AllocatedResources to
 * be non-NULL (pci.sys's START) need this even when nothing is actually
 * being assigned. */
static PCM_RESOURCE_LIST
BuildEmptyResourceList(INTERFACE_TYPE iface, ULONG bus)
{
    ULONG size = sizeof(CM_RESOURCE_LIST);
    PCM_RESOURCE_LIST list;

    list = (PCM_RESOURCE_LIST)ExAllocatePoolWithTag(PagedPool, size, 'rkxN');
    if (!list)
        return NULL;
    RtlZeroMemory(list, size);
    list->Count = 1;
    list->List[0].InterfaceType = iface;
    list->List[0].BusNumber = bus;
    list->List[0].PartialResourceList.Version = 1;
    list->List[0].PartialResourceList.Revision = 1;
    list->List[0].PartialResourceList.Count = 0;
    return list;
}

/* Build a CM_RESOURCE_LIST from the static table -- one full descriptor on
 * the PCI bus.  Caller-owned (PagedPool, freed by the driver after START
 * via ExFreePool). */
static PCM_RESOURCE_LIST
BuildResourceList(const struct nxk_device *d)
{
    ULONG j;
    ULONG size;
    PCM_RESOURCE_LIST list;
    PCM_FULL_RESOURCE_DESCRIPTOR full;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR pd;

    size = sizeof(CM_RESOURCE_LIST)
         + (d->res_count > 1 ? (d->res_count - 1)
                             * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR)
                             : 0);
    /* sizeof(CM_RESOURCE_LIST) already includes one partial descriptor. */
    list = (PCM_RESOURCE_LIST)ExAllocatePoolWithTag(PagedPool, size, 'rkxN');
    if (!list)
        return NULL;
    RtlZeroMemory(list, size);

    list->Count = 1;
    full = &list->List[0];
    full->InterfaceType = PCIBus;
    full->BusNumber = d->bus;
    full->PartialResourceList.Version = 1;
    full->PartialResourceList.Revision = 1;
    full->PartialResourceList.Count = d->res_count;

    for (j = 0; j < d->res_count; j++) {
        const struct nxk_resource *r = &d->res[j];
        pd = &full->PartialResourceList.PartialDescriptors[j];
        switch (r->kind) {
        case NXK_RES_IO:
            pd->Type = CmResourceTypePort;
            pd->ShareDisposition = CmResourceShareDeviceExclusive;
            pd->Flags = CM_RESOURCE_PORT_IO;
            pd->u.Port.Start.QuadPart = r->start;
            pd->u.Port.Length = r->length;
            break;
        case NXK_RES_MMIO:
            pd->Type = CmResourceTypeMemory;
            pd->ShareDisposition = CmResourceShareDeviceExclusive;
            pd->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
            pd->u.Memory.Start.QuadPart = r->start;
            pd->u.Memory.Length = r->length;
            break;
        case NXK_RES_IRQ:
            pd->Type = CmResourceTypeInterrupt;
            pd->ShareDisposition = CmResourceShareDeviceExclusive;
            pd->Flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
            pd->u.Interrupt.Level = r->start;
            pd->u.Interrupt.Vector = r->start;
            pd->u.Interrupt.Affinity = (KAFFINITY)-1;
            break;
        }
    }

    return list;
}

static NTSTATUS
PdoQueryInterface(PDEVICE_OBJECT pdo, PIO_STACK_LOCATION sp)
{
    PNXK_PDO_EXT ext = (PNXK_PDO_EXT)pdo->DeviceExtension;
    const GUID *iface = sp->Parameters.QueryInterface.InterfaceType;

    if (IsEqualGUIDAligned(iface, &GUID_BUS_INTERFACE_STANDARD)) {
        PBUS_INTERFACE_STANDARD out =
            (PBUS_INTERFACE_STANDARD)sp->Parameters.QueryInterface.Interface;

        if (sp->Parameters.QueryInterface.Size < sizeof(*out))
            return STATUS_BUFFER_TOO_SMALL;

        ext->BusIf.Size = sizeof(BUS_INTERFACE_STANDARD);
        ext->BusIf.Version = PCI_BUS_INTERFACE_STANDARD_VERSION;
        ext->BusIf.Context = pdo;
        ext->BusIf.InterfaceReference = BusIfRef;
        ext->BusIf.InterfaceDereference = BusIfDeref;
        ext->BusIf.TranslateBusAddress = BusTranslate;
        ext->BusIf.GetDmaAdapter = BusGetDmaAdapter;
        ext->BusIf.SetBusData = BusSetData;
        ext->BusIf.GetBusData = BusGetData;

        RtlCopyMemory(out, &ext->BusIf, sizeof(*out));
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS
PdoQueryResources(PDEVICE_OBJECT pdo, PIRP Irp)
{
    PNXK_PDO_EXT ext = (PNXK_PDO_EXT)pdo->DeviceExtension;
    PCM_RESOURCE_LIST list;

    if (ext->Entry->res_count == 0)
        return Irp->IoStatus.Status;

    list = BuildResourceList(ext->Entry);
    if (!list)
        return STATUS_INSUFFICIENT_RESOURCES;

    Irp->IoStatus.Information = (ULONG_PTR)list;
    return STATUS_SUCCESS;
}

static NTSTATUS
PdoQueryResourceRequirements(PDEVICE_OBJECT pdo, PIRP Irp)
{
    PNXK_PDO_EXT ext = (PNXK_PDO_EXT)pdo->DeviceExtension;
    const struct nxk_device *d = ext->Entry;
    ULONG j;
    ULONG size;
    PIO_RESOURCE_REQUIREMENTS_LIST req;
    PIO_RESOURCE_DESCRIPTOR rd;

    if (d->res_count == 0)
        return Irp->IoStatus.Status;

    size = sizeof(IO_RESOURCE_REQUIREMENTS_LIST)
         + (d->res_count > 1 ? (d->res_count - 1)
                              * sizeof(IO_RESOURCE_DESCRIPTOR)
                              : 0);
    req = (PIO_RESOURCE_REQUIREMENTS_LIST)
          ExAllocatePoolWithTag(PagedPool, size, 'rkxN');
    if (!req)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(req, size);

    req->ListSize = size;
    req->InterfaceType = PCIBus;
    req->BusNumber = d->bus;
    req->SlotNumber = ((ULONG)d->fn << 5) | d->dev;
    req->AlternativeLists = 1;
    req->List[0].Version = 1;
    req->List[0].Revision = 1;
    req->List[0].Count = d->res_count;

    for (j = 0; j < d->res_count; j++) {
        const struct nxk_resource *r = &d->res[j];
        rd = &req->List[0].Descriptors[j];
        rd->Option = 0; /* required */
        rd->ShareDisposition = CmResourceShareDeviceExclusive;
        switch (r->kind) {
        case NXK_RES_IO:
            rd->Type = CmResourceTypePort;
            rd->Flags = CM_RESOURCE_PORT_IO;
            rd->u.Port.Length = r->length;
            rd->u.Port.Alignment = 1;
            rd->u.Port.MinimumAddress.QuadPart = r->start;
            rd->u.Port.MaximumAddress.QuadPart = r->start + r->length - 1;
            break;
        case NXK_RES_MMIO:
            rd->Type = CmResourceTypeMemory;
            rd->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
            rd->u.Memory.Length = r->length;
            rd->u.Memory.Alignment = 1;
            rd->u.Memory.MinimumAddress.QuadPart = r->start;
            rd->u.Memory.MaximumAddress.QuadPart = r->start + r->length - 1;
            break;
        case NXK_RES_IRQ:
            rd->Type = CmResourceTypeInterrupt;
            rd->Flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
            rd->u.Interrupt.MinimumVector = r->start;
            rd->u.Interrupt.MaximumVector = r->start;
            break;
        }
    }

    Irp->IoStatus.Information = (ULONG_PTR)req;
    return STATUS_SUCCESS;
}

static NTSTATUS
PdoQueryCapabilities(PDEVICE_OBJECT pdo, PIO_STACK_LOCATION sp)
{
    PNXK_PDO_EXT ext = (PNXK_PDO_EXT)pdo->DeviceExtension;
    PDEVICE_CAPABILITIES caps = sp->Parameters.DeviceCapabilities.Capabilities;

    if (caps->Version != 1)
        return STATUS_REVISION_MISMATCH;

    caps->UniqueID = TRUE;
    caps->RawDeviceOK = FALSE;
    /* PCI HAL packing: high 8 bits function, low 8 bits device. */
    caps->Address = ((ULONG)ext->Fn << 16) | ext->Dev;
    caps->UINumber = ((ULONG)ext->Fn << 16) | ext->Dev;

    return STATUS_SUCCESS;
}

static
NTSTATUS NTAPI
PdoDispatchPnp(PDEVICE_OBJECT pdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = Irp->IoStatus.Status;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_INTERFACE:
        status = PdoQueryInterface(pdo, sp);
        break;

    case IRP_MN_QUERY_RESOURCES:
        status = PdoQueryResources(pdo, Irp);
        break;

    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        status = PdoQueryResourceRequirements(pdo, Irp);
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        status = PdoQueryCapabilities(pdo, sp);
        break;

    case IRP_MN_QUERY_PNP_DEVICE_STATE:
    case IRP_MN_QUERY_DEVICE_RELATIONS:
    case IRP_MN_QUERY_BUS_INFORMATION:
    case IRP_MN_QUERY_ID:
    case IRP_MN_QUERY_DEVICE_TEXT:
    case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
    case IRP_MN_QUERY_LEGACY_BUS_INFORMATION:
        /* Leave status unchanged so the requester gets back what it
         * expected (typically STATUS_NOT_SUPPORTED). */
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        status = STATUS_SUCCESS;
        break;

    default:
        DPRINT("[devtree]   pdo dispatch: unhandled MN=0x%02x\n",
                 sp->MinorFunction);
        break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS NTAPI
PdoDispatchPower(PDEVICE_OBJECT pdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    UNREFERENCED_PARAMETER(pdo);

    /* PoStartNextPowerIrp dropped on Xbox; no Po power-IRP queue. */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    if (sp->MinorFunction == IRP_MN_SET_POWER ||
        sp->MinorFunction == IRP_MN_QUERY_POWER) {
        /* good enough */
    }
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
PdoDispatchDefault(PDEVICE_OBJECT pdo, PIRP Irp)
{
    UNREFERENCED_PARAMETER(pdo);
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS NTAPI
BusDriverEntryStub(_In_ PDRIVER_OBJECT DriverObject,
                       _In_ PUNICODE_STRING RegistryPath)
{
    ULONG i;
    UNREFERENCED_PARAMETER(RegistryPath);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = PdoDispatchDefault;

    DriverObject->MajorFunction[IRP_MJ_PNP] = PdoDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = PdoDispatchPower;
    return STATUS_SUCCESS;
}

/* Lazily create the placeholder DRIVER_OBJECT that owns every synthetic
 * PDO.  IoCreateDevice insists on a DriverObject, but PnP only ever sees
 * it through DeviceObject->DriverObject -- there's no PnP enumeration that
 * walks it on Xbox. */
static PDRIVER_OBJECT
EnsureBusDriver(VOID)
{
    UNICODE_STRING name;
    NTSTATUS status;

    if (BusDriverObject)
        return BusDriverObject;

    RtlInitUnicodeString(&name, L"\\Driver\\NxkBus");
    status = IoCreateDriver(&name, BusDriverEntryStub);
    if (!NT_SUCCESS(status)) {
        DPRINT("[devtree] IoCreateDriver(NxkBus) failed: 0x%08x\n", status);
        return NULL;
    }

    /* Look up the resulting DRIVER_OBJECT -- IoCreateDriver doesn't hand
     * it back directly. */
    BusDriverObject = ResolveDriver(L"\\Driver\\NxkBus");
    return BusDriverObject;
}

/* Fabricate the minimum DEVICE_NODE that classpnp's
 * IopIsValidPhysicalDeviceObject expects: the PDO's DeviceObjectExtension
 * has to point at a node whose Flags include DNF_ENUMERATED, and the node
 * needs a non-empty InstancePath so IoRegisterDeviceInterface can build a
 * key name from it.  Mirrors the root-PDO setup in PnP's pnpinit.c so the
 * shape is identical to what NT's enumeration would produce. */
static PDEVICE_NODE
AllocateDevNode(PDEVICE_OBJECT pdo, PCWSTR instance_path)
{
    PDEVICE_NODE node;

    if (!pdo)
        return NULL;

    node = NxkAllocateDevNode(pdo);
    if (!node) {
        DPRINT("[devtree]   NxkAllocateDevNode(%p) failed\n", pdo);
        return NULL;
    }

    node->Flags |= DNF_MADEUP | DNF_ENUMERATED |
                   DNF_IDS_QUERIED | DNF_NO_RESOURCE_REQUIRED;

    if (instance_path) {
        if (!RtlCreateUnicodeString(&node->InstancePath, instance_path)) {
            DPRINT("[devtree]   InstancePath alloc failed\n");
            /* Continue; classpnp only insists on length>0 when it goes to
             * build registry paths.  A NULL buffer here just means the
             * IoRegisterDeviceInterface path will short-circuit. */
        }
    }

    NxkSetDevNodeState(node, DeviceNodeStarted);
    return node;
}

/* Create the synthetic PDO for one tree entry. */
static PDEVICE_OBJECT
CreatePdo(const struct nxk_device *d)
{
    PDRIVER_OBJECT bus = EnsureBusDriver();
    PDEVICE_OBJECT pdo = NULL;
    PNXK_PDO_EXT ext;
    WCHAR pathbuf[64];
    UNICODE_STRING pathU;
    ANSI_STRING pathA;
    CHAR ansibuf[64];
    NTSTATUS status;

    if (!bus)
        return NULL;

    status = IoCreateDevice(bus,
                            sizeof(NXK_PDO_EXT),
                            NULL,
                            FILE_DEVICE_BUS_EXTENDER,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &pdo);
    if (!NT_SUCCESS(status)) {
        DPRINT("[devtree]   IoCreateDevice(pdo) failed: 0x%08x\n", status);
        return NULL;
    }

    pdo->Flags |= DO_BUS_ENUMERATED_DEVICE | DO_POWER_PAGABLE;
    pdo->Flags &= ~DO_DEVICE_INITIALIZING;

    ext = (PNXK_PDO_EXT)pdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));
    ext->Entry = d;
    ext->Bus = d->bus;
    ext->Dev = d->dev;
    ext->Fn  = d->fn;

    /* Build a synthetic instance path that is unique per BDF.  Format
     * mirrors what NT's PCI enumerator would generate. */
    pathA.Buffer = ansibuf;
    pathA.MaximumLength = sizeof(ansibuf);
    pathA.Length = (USHORT)_snprintf(ansibuf, sizeof(ansibuf),
                                     "NXK\\PCI\\BUS%u\\DEV%02X\\FUN%X",
                                     (unsigned)d->bus,
                                     (unsigned)d->dev,
                                     (unsigned)d->fn);
    pathU.Buffer = pathbuf;
    pathU.MaximumLength = sizeof(pathbuf);
    pathU.Length = 0;
    /* FALSE: in-place conversion into pre-sized pathbuf; only fails on a
     * too-small destination, which the buffer size precludes. */
    NT_VERIFY(NT_SUCCESS(RtlAnsiStringToUnicodeString(&pathU, &pathA, FALSE)));
    pathbuf[pathU.Length / sizeof(WCHAR)] = L'\0';

    (VOID)AllocateDevNode(pdo, pathbuf);
    return pdo;
}

/* Send IRP_MN_START_DEVICE to the top of pdo's stack with the supplied
 * resource lists.  Mirrors PiIrpStartDevice's IopSynchronousCall. */
static NTSTATUS
SendStart(PDEVICE_OBJECT pdo,
              PCM_RESOURCE_LIST Raw,
              PCM_RESOURCE_LIST Translated)
{
    PDEVICE_OBJECT top;
    PIRP irp;
    PIO_STACK_LOCATION sp;
    IO_STATUS_BLOCK iosb;
    KEVENT done;
    NTSTATUS status;

    top = IoGetAttachedDeviceReference(pdo);
    if (!top)
        return STATUS_INVALID_DEVICE_REQUEST;

    irp = IoAllocateIrp(top->StackSize, FALSE);
    if (!irp) {
        ObDereferenceObject(top);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irp->IoStatus.Status = iosb.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = iosb.Information = 0;

    KeInitializeEvent(&done, SynchronizationEvent, FALSE);
    irp->UserIosb = &iosb;
    irp->UserEvent = &done;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    IoQueueThreadIrp(irp);

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_PNP;
    sp->MinorFunction = IRP_MN_START_DEVICE;
    sp->Parameters.StartDevice.AllocatedResources = Raw;
    sp->Parameters.StartDevice.AllocatedResourcesTranslated = Translated;

    status = IoCallDriver(top, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    ObDereferenceObject(top);
    return status;
}

/* Send IRP_MN_QUERY_DEVICE_RELATIONS(BusRelations) to the top of pdo's
 * stack and return the resulting DEVICE_RELATIONS list. */
static NTSTATUS
SendQueryBusRelations(PDEVICE_OBJECT pdo,
                          PDEVICE_RELATIONS *RelationsOut)
{
    PDEVICE_OBJECT top;
    PIRP irp;
    PIO_STACK_LOCATION sp;
    IO_STATUS_BLOCK iosb;
    KEVENT done;
    NTSTATUS status;

    *RelationsOut = NULL;

    top = IoGetAttachedDeviceReference(pdo);
    if (!top)
        return STATUS_INVALID_DEVICE_REQUEST;

    irp = IoAllocateIrp(top->StackSize, FALSE);
    if (!irp) {
        ObDereferenceObject(top);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irp->IoStatus.Status = iosb.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = iosb.Information = 0;

    KeInitializeEvent(&done, SynchronizationEvent, FALSE);
    irp->UserIosb = &iosb;
    irp->UserEvent = &done;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    IoQueueThreadIrp(irp);

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_PNP;
    sp->MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS;
    sp->Parameters.QueryDeviceRelations.Type = BusRelations;

    status = IoCallDriver(top, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    if (NT_SUCCESS(status))
        *RelationsOut = (PDEVICE_RELATIONS)iosb.Information;

    ObDereferenceObject(top);
    return status;
}

/* Send IRP_MN_QUERY_ID(BusQueryDeviceID) to pdo and return the resulting
 * wide string (caller frees with ExFreePool). */
static PWCHAR
SendQueryDeviceId(PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT top;
    PIRP irp;
    PIO_STACK_LOCATION sp;
    IO_STATUS_BLOCK iosb;
    KEVENT done;
    NTSTATUS status;

    top = IoGetAttachedDeviceReference(pdo);
    if (!top)
        return NULL;

    irp = IoAllocateIrp(top->StackSize, FALSE);
    if (!irp) {
        ObDereferenceObject(top);
        return NULL;
    }

    irp->IoStatus.Status = iosb.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = iosb.Information = 0;
    KeInitializeEvent(&done, SynchronizationEvent, FALSE);
    irp->UserIosb = &iosb;
    irp->UserEvent = &done;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    IoQueueThreadIrp(irp);

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_PNP;
    sp->MinorFunction = IRP_MN_QUERY_ID;
    sp->Parameters.QueryId.IdType = BusQueryDeviceID;

    status = IoCallDriver(top, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    ObDereferenceObject(top);

    if (!NT_SUCCESS(status))
        return NULL;
    return (PWCHAR)iosb.Information;
}

/* Send IRP_MN_QUERY_RESOURCES to a child PDO (created by a bus driver)
 * and return the CM_RESOURCE_LIST. */
static NTSTATUS
SendQueryResources(PDEVICE_OBJECT pdo, PCM_RESOURCE_LIST *Out)
{
    PDEVICE_OBJECT top;
    PIRP irp;
    PIO_STACK_LOCATION sp;
    IO_STATUS_BLOCK iosb;
    KEVENT done;
    NTSTATUS status;

    *Out = NULL;
    top = IoGetAttachedDeviceReference(pdo);
    if (!top)
        return STATUS_INVALID_DEVICE_REQUEST;

    irp = IoAllocateIrp(top->StackSize, FALSE);
    if (!irp) {
        ObDereferenceObject(top);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irp->IoStatus.Status = iosb.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = iosb.Information = 0;
    KeInitializeEvent(&done, SynchronizationEvent, FALSE);
    irp->UserIosb = &iosb;
    irp->UserEvent = &done;
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    IoQueueThreadIrp(irp);

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_PNP;
    sp->MinorFunction = IRP_MN_QUERY_RESOURCES;

    status = IoCallDriver(top, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    if (NT_SUCCESS(status))
        *Out = (PCM_RESOURCE_LIST)iosb.Information;
    ObDereferenceObject(top);
    return status;
}

/* Clone a CM_RESOURCE_LIST (so we can translate the copy in place and
 * keep the raw version intact). */
static PCM_RESOURCE_LIST
CloneResourceList(PCM_RESOURCE_LIST in)
{
    PCM_FULL_RESOURCE_DESCRIPTOR full;
    ULONG size, i;
    PCM_RESOURCE_LIST out;

    if (!in)
        return NULL;
    size = sizeof(CM_RESOURCE_LIST);
    full = &in->List[0];
    for (i = 0; i < in->Count; i++) {
        ULONG fsize = FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR,
                                   PartialResourceList.PartialDescriptors)
                    + full->PartialResourceList.Count
                    * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
        size = (i == 0) ? fsize : size + fsize;
        full = (PCM_FULL_RESOURCE_DESCRIPTOR)
               &full->PartialResourceList.PartialDescriptors[
                   full->PartialResourceList.Count];
    }
    /* Add the leading Count field. */
    size += FIELD_OFFSET(CM_RESOURCE_LIST, List);
    out = (PCM_RESOURCE_LIST)ExAllocatePoolWithTag(PagedPool, size, 'rkxN');
    if (!out)
        return NULL;
    RtlCopyMemory(out, in, size);
    return out;
}

/* Get the child PDO's raw resources; produce a translated copy too.  For
 * resourceless PDOs (e.g. classpnp's ATA disk PDOs which inherit through
 * the channel), default to empty PCI-bus lists so drivers that expect
 * non-NULL parameters don't NPE. */
static NTSTATUS
FetchChildResources(PDEVICE_OBJECT pdo,
                        PCM_RESOURCE_LIST *Raw,
                        PCM_RESOURCE_LIST *Translated)
{
    PCM_RESOURCE_LIST queried = NULL;
    NTSTATUS status;

    *Raw = NULL;
    *Translated = NULL;

    status = SendQueryResources(pdo, &queried);
    if (NT_SUCCESS(status) && queried) {
        *Raw = queried;
        *Translated = CloneResourceList(queried);
        if (*Translated)
            TranslateResourceList(*Translated);
    } else {
        *Raw = BuildEmptyResourceList(PCIBus, 0);
        *Translated = BuildEmptyResourceList(PCIBus, 0);
    }

    if (!*Raw || !*Translated) {
        if (*Raw) ExFreePool(*Raw);
        if (*Translated) ExFreePool(*Translated);
        *Raw = *Translated = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

/* Ensure a child PDO that came back from BusRelations carries a
 * DeviceNode with DNF_ENUMERATED before we hand it to a class driver --
 * classpnp's IopIsValidPhysicalDeviceObject short-circuits the
 * IoOpenDeviceRegistryKey path on its absence. */
static VOID
EnsureChildDevNode(PDEVICE_OBJECT child, const char *Tag)
{
    PEXTENDED_DEVOBJ_EXTENSION dext;
    WCHAR pathbuf[64];
    ANSI_STRING pathA;
    UNICODE_STRING pathU;
    CHAR ansibuf[64];

    if (!child)
        return;

    dext = (PEXTENDED_DEVOBJ_EXTENSION)child->DeviceObjectExtension;
    if (dext && dext->DeviceNode) {
        /* The bus driver (atapi, pciide) attached its own node already --
         * just promote the flags. */
        dext->DeviceNode->Flags |= DNF_MADEUP | DNF_ENUMERATED |
                                   DNF_IDS_QUERIED | DNF_NO_RESOURCE_REQUIRED;
        if (dext->DeviceNode->InstancePath.Length == 0) {
            pathA.Buffer = ansibuf;
            pathA.MaximumLength = sizeof(ansibuf);
            pathA.Length = (USHORT)_snprintf(ansibuf, sizeof(ansibuf),
                                             "NXK\\%s\\%p", Tag, child);
            pathU.Buffer = pathbuf;
            pathU.MaximumLength = sizeof(pathbuf);
            pathU.Length = 0;
            NT_VERIFY(NT_SUCCESS(RtlAnsiStringToUnicodeString(&pathU, &pathA, FALSE)));
            pathbuf[pathU.Length / sizeof(WCHAR)] = L'\0';
            NT_VERIFY(RtlCreateUnicodeString(&dext->DeviceNode->InstancePath, pathbuf));
        }
        if (dext->DeviceNode->State != DeviceNodeStarted)
            NxkSetDevNodeState(dext->DeviceNode, DeviceNodeStarted);
        return;
    }

    pathA.Buffer = ansibuf;
    pathA.MaximumLength = sizeof(ansibuf);
    pathA.Length = (USHORT)_snprintf(ansibuf, sizeof(ansibuf),
                                     "NXK\\%s\\%p", Tag, child);
    pathU.Buffer = pathbuf;
    pathU.MaximumLength = sizeof(pathbuf);
    pathU.Length = 0;
    NT_VERIFY(NT_SUCCESS(RtlAnsiStringToUnicodeString(&pathU, &pathA, FALSE)));
    pathbuf[pathU.Length / sizeof(WCHAR)] = L'\0';

    (VOID)AllocateDevNode(child, pathbuf);
}

/* AddDevice the named class driver on top of a child PDO, then START it.
 * "Stack" is a debug tag. */
static NTSTATUS
AttachAndStart(const WCHAR *DriverName,
                   PDEVICE_OBJECT child,
                   const char *Stack)
{
    PDRIVER_OBJECT drv = ResolveDriver(DriverName);
    NTSTATUS status;
    PCM_RESOURCE_LIST raw = NULL, translated = NULL;

    if (!drv) {
        DPRINT("[devtree]   [%s] resolve %ws failed\n", Stack, DriverName);
        return STATUS_DRIVER_INTERNAL_ERROR;
    }
    if (!drv->DriverExtension || !drv->DriverExtension->AddDevice) {
        DPRINT("[devtree]   [%s] %ws has no AddDevice\n",
                 Stack, DriverName);
        ObDereferenceObject(drv);
        return STATUS_NOT_IMPLEMENTED;
    }

    EnsureChildDevNode(child, Stack);

    status = drv->DriverExtension->AddDevice(drv, child);
    DPRINT("[devtree]   [%s] %ws AddDevice(%p) = 0x%08x\n",
             Stack, DriverName, child, status);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(drv);
        return status;
    }

    status = FetchChildResources(child, &raw, &translated);
    if (!NT_SUCCESS(status)) {
        DPRINT("[devtree]   [%s] %ws res alloc failed\n", Stack, DriverName);
        ObDereferenceObject(drv);
        return status;
    }
    status = SendStart(child, raw, translated);
    DPRINT("[devtree]   [%s] %ws START(%p) = 0x%08x\n",
             Stack, DriverName, child, status);

    ObDereferenceObject(drv);
    return status;
}

/* Recursively bring up the storage chain rooted at the IDE controller's
 * FDO.  pciide BusRelations -> channel PDO -> atapi AddDevice -> START
 * (which probes the ATA bus and creates device PDOs as children of the
 * channel FDO) -> BusRelations -> disk/cdrom AddDevice + START -> partmgr
 * (HDD only) AddDevice + START. */
static VOID
DriveStorageStack(PDEVICE_OBJECT pciidePdo)
{
    PDEVICE_RELATIONS channels = NULL;
    NTSTATUS status;
    ULONG c;

    status = SendQueryBusRelations(pciidePdo, &channels);
    DPRINT("[devtree]   pciide QueryBusRelations = 0x%08x channels=%p\n",
             status, channels);
    if (!NT_SUCCESS(status) || !channels)
        return;
    DPRINT("[devtree]   pciide enumerated %u channel(s)\n",
             channels->Count);

    for (c = 0; c < channels->Count; c++) {
        PDEVICE_OBJECT chanPdo = channels->Objects[c];
        PDEVICE_RELATIONS devs = NULL;
        ULONG di;

        if (!chanPdo)
            continue;

        status = AttachAndStart(L"\\Driver\\atapi", chanPdo, "atapi");
        if (!NT_SUCCESS(status))
            continue;

        status = SendQueryBusRelations(chanPdo, &devs);
        DPRINT("[devtree]   atapi ch=%u QueryBusRelations = 0x%08x"
                 " devs=%p\n", c, status, devs);
        if (!NT_SUCCESS(status) || !devs) {
            if (devs) ExFreePool(devs);
            continue;
        }

        for (di = 0; di < devs->Count; di++) {
            PDEVICE_OBJECT devPdo = devs->Objects[di];
            PWCHAR devId;
            BOOLEAN isCdrom;

            if (!devPdo)
                continue;

            /* Ask the atapi PDO for its BusQueryDeviceID.  ATAPI/CD
             * peripherals come back as IDE\CdRom*; ATA disks as IDE\Disk*
             * (see atapi/pdo.c AtaTypeCodeToName). */
            devId = SendQueryDeviceId(devPdo);
            isCdrom = devId && (wcsstr(devId, L"CdRom") != NULL);
            DPRINT("[devtree]   atapi ch=%u dev=%u id=%ws -> %s\n",
                     c, di, devId ? devId : L"(none)",
                     isCdrom ? "cdrom" : "disk");
            if (devId)
                ExFreePool(devId);

            if (isCdrom) {
                status = AttachAndStart(L"\\Driver\\Cdrom", devPdo,
                                            "cdrom");
            } else {
                status = AttachAndStart(L"\\Driver\\Disk", devPdo,
                                            "disk");
                if (NT_SUCCESS(status)) {
                    status = AttachAndStart(L"\\Driver\\partmgr",
                                                devPdo, "partmgr");
                    if (NT_SUCCESS(status)) {
                        /* partmgr publishes partition PDOs via its FDO's
                         * BusRelations response.  In NT PnP that triggers
                         * START on each partition PDO -- here we have to
                         * drive both ourselves. */
                        PDEVICE_RELATIONS parts = NULL;
                        ULONG pi;
                        status = SendQueryBusRelations(devPdo, &parts);
                        DPRINT("[devtree]   partmgr QueryBusRelations ="
                                 " 0x%08x parts=%p\n", status, parts);
                        if (NT_SUCCESS(status) && parts) {
                            for (pi = 0; pi < parts->Count; pi++) {
                                PDEVICE_OBJECT pPdo = parts->Objects[pi];
                                PCM_RESOURCE_LIST praw = NULL, ptr = NULL;
                                if (!pPdo)
                                    continue;
                                EnsureChildDevNode(pPdo, "part");
                                praw = BuildEmptyResourceList(PCIBus, 0);
                                ptr  = BuildEmptyResourceList(PCIBus, 0);
                                if (!praw || !ptr) {
                                    if (praw) ExFreePool(praw);
                                    if (ptr) ExFreePool(ptr);
                                    continue;
                                }
                                status = SendStart(pPdo, praw, ptr);
                                DPRINT("[devtree]   [part%u] START(%p)"
                                         " = 0x%08x\n", pi, pPdo, status);
                            }
                            ExFreePool(parts);
                        }
                    }
                }
            }
        }
        ExFreePool(devs);
    }
    ExFreePool(channels);
}

/*
 * Top-level: walk the table, create a synthetic PDO per driver-bound
 * entry, invoke the folded driver's AddDevice, send START.  For the IDE
 * controller, recurse into the storage stack.
 */
VOID
NxkDriveDeviceTree(VOID)
{
    ULONG i;

    DPRINT("[devtree] driving %u device tree entries\n",
             NxkDeviceTreeCount);

    for (i = 0; i < NxkDeviceTreeCount; i++) {
        const struct nxk_device *d = &NxkDeviceTree[i];
        PDEVICE_OBJECT pdo;
        PDRIVER_OBJECT drv;
        PCM_RESOURCE_LIST raw = NULL;
        PCM_RESOURCE_LIST translated = NULL;
        NTSTATUS status;

        if (!d->driver_name)
            continue;

        drv = ResolveDriver(d->driver_name);
        if (!drv || !drv->DriverExtension ||
            !drv->DriverExtension->AddDevice) {
            DPRINT("[devtree] entry %02u %s: driver %ws unavailable\n",
                     i, d->name, d->driver_name);
            if (drv) ObDereferenceObject(drv);
            continue;
        }

        pdo = CreatePdo(d);
        if (!pdo) {
            ObDereferenceObject(drv);
            continue;
        }

        DPRINT("[devtree] entry %02u %s: PDO=%p driver=%p\n",
                 i, d->name, pdo, drv);

        status = drv->DriverExtension->AddDevice(drv, pdo);
        DPRINT("[devtree]   AddDevice = 0x%08x\n", status);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(drv);
            continue;
        }

        if (d->res_count > 0) {
            raw = BuildResourceList(d);
            translated = BuildResourceList(d);
        } else {
            /* Drivers (pci.sys) require non-NULL AllocatedResources even
             * for entries with no assigned resources. */
            raw = BuildEmptyResourceList(PCIBus, d->bus);
            translated = BuildEmptyResourceList(PCIBus, d->bus);
        }
        if (!raw || !translated) {
            DPRINT("[devtree]   res alloc failed (raw=%p translated=%p)\n",
                     raw, translated);
            if (raw) ExFreePool(raw);
            if (translated) ExFreePool(translated);
            ObDereferenceObject(drv);
            continue;
        }
        if (d->res_count > 0) {
            status = TranslateResourceList(translated);
            if (!NT_SUCCESS(status)) {
                DPRINT("[devtree]   translate = 0x%08x\n", status);
                ExFreePool(raw);
                ExFreePool(translated);
                ObDereferenceObject(drv);
                continue;
            }
        }

        status = SendStart(pdo, raw, translated);
        DPRINT("[devtree]   START = 0x%08x\n", status);

        /* Resource lists are typically captured by the driver in its
         * FDO extension during START; freeing them here would dangle.  We
         * leak the small allocations -- there are at most NxkDeviceTreeCount
         * of them per boot. */

        /* Storage stack chain rides on pciide. */
        if (d->driver == PciideDriverEntry && NT_SUCCESS(status))
            DriveStorageStack(pdo);

        ObDereferenceObject(drv);
    }

    DPRINT("[devtree] drive complete\n");
}

/* Walk every folded driver and clear DO_DEVICE_INITIALIZING on every FDO
 * it created at DriverEntry time.  Some folded drivers (notably MountMgr)
 * publish their primary device object during their own DriverEntry and
 * leave the flag set, expecting upstream PnP to clear it before any IRP
 * lands. */
VOID
NxkClearDeviceInitFlags(VOID)
{
    static const PCWSTR foldedNames[] = {
        L"\\Driver\\Pci",
        L"\\Driver\\pciide",
        L"\\Driver\\atapi",
        L"\\Driver\\Disk",
        L"\\Driver\\Cdrom",
        L"\\Driver\\partmgr",
        L"\\Driver\\MountMgr",
        L"\\FileSystem\\Cdfs",
        L"\\FileSystem\\Xdvdfs",
        L"\\FileSystem\\Vfatfs",
        L"\\FileSystem\\RAW",
    };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(foldedNames); i++) {
        PDRIVER_OBJECT drv = ResolveDriver(foldedNames[i]);
        PDEVICE_OBJECT dev;
        ULONG cleared = 0;

        if (!drv)
            continue;
        for (dev = drv->DeviceObject; dev; dev = dev->NextDevice) {
            if (dev->Flags & DO_DEVICE_INITIALIZING) {
                dev->Flags &= ~DO_DEVICE_INITIALIZING;
                cleared++;
            }
        }
        if (cleared) {
            DPRINT("[devtree] cleared DO_DEVICE_INITIALIZING on %u FDO(s)"
                     " of %ws\n", cleared, foldedNames[i]);
        }
        ObDereferenceObject(drv);
    }
}

#endif /* SARCH_XBOX */
