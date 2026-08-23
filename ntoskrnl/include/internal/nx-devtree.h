/*
 * Static Xbox device tree -- replaces PnP enumeration on the SARCH=xbox
 * build.  Hardware is fixed so the topology is a constant array.
 */

#pragma once

#ifdef SARCH_XBOX

#ifdef __cplusplus
extern "C" {
#endif

enum nxk_res_kind {
    NXK_RES_IO,
    NXK_RES_MMIO,
    NXK_RES_IRQ,
};

struct nxk_resource {
    enum nxk_res_kind kind;
    ULONG start;
    ULONG length;
};

struct nxk_device {
    const char *name;            /* object-namespace path, or descriptive tag */
    PDRIVER_INITIALIZE driver;   /* DriverEntry of binding driver, or NULL */
    const WCHAR *driver_name;    /* \Driver\<X> matching driver, or NULL */
    UCHAR  bus, dev, fn;        /* PCI BDF, or 0xff if not on PCI */
    USHORT vendor, device;      /* PCI VID/DID, or 0 */
    const struct nxk_resource *res;
    ULONG res_count;
};

extern const struct nxk_device NxkDeviceTree[];
extern const ULONG NxkDeviceTreeCount;

VOID NxkInitDeviceTree(VOID);

/* Direct Type-1 PCI configuration access (port 0xCF8/0xCFC).  Does not
 * depend on HAL's BusHandler / PnP registry state. */
ULONG  PciCfgRead32(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset);
VOID   PciCfgWrite32(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset, ULONG value);
USHORT PciCfgRead16(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset);
VOID   PciCfgWrite16(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset, USHORT value);
UCHAR  PciCfgRead8(UCHAR bus, UCHAR dev, UCHAR fn, UCHAR offset);

/* Walk the device tree and drive AddDevice + IRP_MN_START_DEVICE for each
 * driver-bound entry, then recursively enumerate child PDOs over the
 * resulting FDO stack.  Replaces PnP enumeration when SARCH_XBOX. */
VOID NxkDriveDeviceTree(VOID);

/* After the IopFoldedInDrivers loop creates driver objects, every FDO
 * they happen to publish at DriverEntry time still carries
 * DO_DEVICE_INITIALIZING -- normally cleared by NT's PnP framework via
 * IoInitializeDriverInitFlag.  With PnP enumeration no-op'd on Xbox the
 * flag would strand the device's IRP queue.  Walk DriverObject->DeviceObject
 * for every folded driver and clear the flag. */
VOID NxkClearDeviceInitFlags(VOID);

/* Xbox-side replacements for the PnP devnode helpers we used to reach
 * into pnpmgr/ for.  Owned by ntoskrnl/xb/devnode.c. */
PDEVICE_NODE NxkAllocateDevNode(_In_ PDEVICE_OBJECT pdo);
VOID         NxkSetDevNodeState(_In_ PDEVICE_NODE node,
                                _In_ PNP_DEVNODE_STATE state);

/* Build the root PnP driver, root PDO, and root DeviceNode the kept Xbox
 * boot path (PoInitSystem(BootPhase=1), IopInitializeSystemDrivers, ...)
 * dereferences via IopRootDeviceNode.  Called once from IoInitSystem
 * before NxkInitDeviceTree / NxkDriveDeviceTree. */
NTSTATUS NxkInitRootDeviceNode(VOID);

#ifdef __cplusplus
}
#endif

#endif /* SARCH_XBOX */
