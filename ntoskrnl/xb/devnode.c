/*
 * Xbox device-node infrastructure.
 *
 * Owns the small set of globals + helpers that the kept Xbox boot path
 * (po/power.c PoInitSystem(BootPhase=1), xb/devtree.c) needs
 * out of the legacy PnP manager.  The full DEVICE_NODE state machine,
 * arbiters, and registry-driven enumeration are no-op'd on Xbox; only
 * the small slice of DEVICE_NODE the kept callers actually dereference
 * is populated -- PhysicalDeviceObject, Flags, State, InstancePath,
 * PreviousState/StateHistory bookkeeping (set by NxkSetDevNodeState).
 *
 * Replaces ntoskrnl/io/pnpmgr/{devnode.c,pnpinit.c} on the SARCH=xbox
 * build.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#ifdef SARCH_XBOX

#include "internal/nx-devtree.h"

/* --- Globals --------------------------------------------------------- */

PDRIVER_OBJECT IopRootDriverObject = NULL;
PDEVICE_NODE   IopRootDeviceNode   = NULL;

/* Other PnP globals normally owned by pnpinit.c.  Defined here so the
 * extern decls in include/internal/io.h still resolve when pnpinit.c is
 * dropped from the source list. */
KSPIN_LOCK IopDeviceTreeLock;
BOOLEAN    PnPBootDriversLoaded = FALSE;
BOOLEAN    PnPBootDriversInitialized = FALSE;

/* --- Helpers --------------------------------------------------------- */

/* Trivial accessor.  Mirrors the upstream pnpmgr/devnode.c version. */
PDEVICE_NODE
FASTCALL
IopGetDeviceNode(_In_ PDEVICE_OBJECT DeviceObject)
{
    return IoGetDevObjExtension(DeviceObject)->DeviceNode;
}

/* Release a DEVICE_NODE.  We don't link nodes into a parent/child tree on
 * Xbox (no enumeration), so the destructor reduces to detaching from the
 * PDO + freeing the embedded strings and the node itself. */
NTSTATUS
IopFreeDeviceNode(_In_ PDEVICE_NODE DeviceNode)
{
    if (!DeviceNode)
        return STATUS_INVALID_PARAMETER;

    if (DeviceNode->PhysicalDeviceObject)
        IoGetDevObjExtension(DeviceNode->PhysicalDeviceObject)->DeviceNode = NULL;

    RtlFreeUnicodeString(&DeviceNode->InstancePath);
    RtlFreeUnicodeString(&DeviceNode->ServiceName);

    if (DeviceNode->ResourceList)
        ExFreePool(DeviceNode->ResourceList);
    if (DeviceNode->ResourceListTranslated)
        ExFreePool(DeviceNode->ResourceListTranslated);
    if (DeviceNode->ResourceRequirements)
        ExFreePool(DeviceNode->ResourceRequirements);
    if (DeviceNode->BootResources)
        ExFreePool(DeviceNode->BootResources);

    ExFreePoolWithTag(DeviceNode, 'nDvN');
    return STATUS_SUCCESS;
}

/* No parent/child tree on Xbox -- every DEVICE_NODE we allocate is a
 * standalone leaf.  The only caller (PopSetSystemPowerState) never runs
 * during normal title execution, so visiting just the start node keeps
 * the contract without dragging in the full traversal helper. */
NTSTATUS
IopTraverseDeviceTree(_In_ PDEVICETREE_TRAVERSE_CONTEXT Context)
{
    NTSTATUS Status;

    if (!Context || !Context->FirstDeviceNode || !Context->Action)
        return STATUS_INVALID_PARAMETER;

    Context->DeviceNode = Context->FirstDeviceNode;
    Status = (Context->Action)(Context->DeviceNode, Context->Context);
    if (Status == STATUS_UNSUCCESSFUL)
        Status = STATUS_SUCCESS;
    return Status;
}


/* Allocate a DEVICE_NODE and link it into the PDO's extended extension.
 * Mirrors the small surface of PipAllocateDeviceNode that the kept Xbox
 * callers exercise: store the PDO, clear DO_DEVICE_INITIALIZING, hand
 * back the node.  No arbiter / notification / list-head init -- those
 * fields are touched only by the upstream state machine, which is
 * stubbed out on Xbox. */
PDEVICE_NODE
NxkAllocateDevNode(_In_ PDEVICE_OBJECT pdo)
{
    PDEVICE_NODE node;

    PAGED_CODE();

    node = ExAllocatePoolWithTag(NonPagedPool, sizeof(DEVICE_NODE), 'nDvN');
    if (!node)
        return NULL;

    RtlZeroMemory(node, sizeof(*node));
    node->State = DeviceNodeUninitialized;
    node->InterfaceType = InterfaceTypeUndefined;
    node->BusNumber = (ULONG)-1;
    node->ChildInterfaceType = InterfaceTypeUndefined;
    node->ChildBusNumber = (ULONG)-1;
    node->ChildBusTypeIndex = (USHORT)-1;

    if (pdo) {
        node->PhysicalDeviceObject = pdo;
        IoGetDevObjExtension(pdo)->DeviceNode = node;
        pdo->Flags &= ~DO_DEVICE_INITIALIZING;
    }

    return node;
}

/* No state machine on Xbox.  Just stash the State and roll the history
 * ring so callers that read these back (classpnp checking
 * DeviceNodeStarted, etc.) see something sensible. */
VOID
NxkSetDevNodeState(_In_ PDEVICE_NODE node, _In_ PNP_DEVNODE_STATE state)
{
    PNP_DEVNODE_STATE prev;

    if (!node)
        return;

    prev = node->State;
    if (prev == state)
        return;

    node->State = state;
    node->PreviousState = prev;
    node->StateHistory[node->StateHistoryEntry++] = prev;
    node->StateHistoryEntry %= DEVNODE_HISTORY_SIZE;
}

/* DriverEntry stub for the root PnP driver.  IoCreateDriver requires a
 * non-NULL entry point and indirect-calls it after ObInsertObject. */
static NTSTATUS NTAPI
RootPnpDriverEntry(_In_ PDRIVER_OBJECT DriverObject,
                       _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    return STATUS_SUCCESS;
}

/* Create the root PnP driver + PDO + DeviceNode.  PoInitSystem(BootPhase=1)
 * reads IopRootDeviceNode->PhysicalDeviceObject->DriverObject; the kept
 * IopInitializeSystemDrivers / IopInitializeBuiltinDrivers Xbox paths
 * indirect through IopRootDeviceNode->PhysicalDeviceObject when handing
 * the root PDO to the (stubbed) PiQueueDeviceAction. */
NTSTATUS
NxkInitRootDeviceNode(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Driver\\PnpManager");
    OBJECT_ATTRIBUTES oa;
    HANDLE drvHandle;
    PDEVICE_OBJECT pdo = NULL;
    NTSTATUS status;

    KeInitializeSpinLock(&IopDeviceTreeLock);

    status = IoCreateDriver(&name, RootPnpDriverEntry);
    if (!NT_SUCCESS(status))
        return status;

    InitializeObjectAttributes(&oa, &name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    status = ObOpenObjectByName(&oa, IoDriverObjectType, KernelMode, NULL,
                                0, NULL, &drvHandle);
    if (!NT_SUCCESS(status))
        return status;

    status = ObReferenceObjectByHandle(drvHandle, 0, IoDriverObjectType,
                                       KernelMode,
                                       (PVOID *)&IopRootDriverObject,
                                       NULL);
    NtClose(drvHandle);
    if (!NT_SUCCESS(status))
        return status;

    status = IoCreateDevice(IopRootDriverObject,
                            0,
                            NULL,
                            FILE_DEVICE_CONTROLLER,
                            0,
                            FALSE,
                            &pdo);
    if (!NT_SUCCESS(status))
        return status;

    pdo->Flags |= DO_BUS_ENUMERATED_DEVICE;

    IopRootDeviceNode = NxkAllocateDevNode(pdo);
    if (!IopRootDeviceNode)
        return STATUS_NO_MEMORY;

    IopRootDeviceNode->Flags |= DNF_MADEUP | DNF_ENUMERATED |
                                DNF_IDS_QUERIED | DNF_NO_RESOURCE_REQUIRED;

    /* Non-empty InstancePath so IoRegisterDeviceInterface doesn't reject
     * the root node on zero length. */
    if (!RtlCreateUnicodeString(&IopRootDeviceNode->InstancePath,
                                L"NXK\\ROOT"))
        return STATUS_NO_MEMORY;

    NxkSetDevNodeState(IopRootDeviceNode, DeviceNodeStarted);
    return STATUS_SUCCESS;
}

#endif /* SARCH_XBOX */
