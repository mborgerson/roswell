/*++

Copyright (C) Microsoft Corporation, 1991 - 2005

Module Name:

    dispatch.c

Abstract:

    Code to support multiple dispatch tables.

Environment:

    kernel mode only

Notes:


Revision History:

--*/

#include "classp.h"
#include <ndk/section_attribs.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, ClassInitializeDispatchTables)
#endif

DRIVER_DISPATCH ClassDispatchUnimplemented;

//
// Routines start
//


VOID
ClassInitializeDispatchTables(
    PCLASS_DRIVER_EXTENSION DriverExtension
    )
{
    ULONG idx;

    PAGED_CODE();

    //
    // Initialize the standard device dispatch table
    //

    for (idx = 0; idx <= IRP_MJ_MAXIMUM_FUNCTION; idx++) {
        DriverExtension->DeviceMajorFunctionTable[idx] = ClassDispatchUnimplemented;
    }

    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_CREATE]         = ClassCreateClose;
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_CLOSE]          = ClassCreateClose;
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_READ]           = ClassReadWrite;
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_WRITE]          = ClassReadWrite;
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_DEVICE_CONTROL] = ClassDeviceControlDispatch;
#ifndef SARCH_XBOX
    /* Xbox kernel exports no Scsi or Srb or ScsiPort symbols; no title
     * can issue IRP_MJ_SCSI to a classpnp upper-edge device. Intra-stack
     * SRB traffic goes classpnp -> IoCallDriver -> atapi FDO's MJ_SCSI
     * slot, which stays live. */
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_SCSI]           = ClassInternalIoControl;
#endif
#ifndef SARCH_XBOX
    /* NtShutdownSystem is not exported; titles route shutdown through
     * HalInitiateShutdown -> HalReturnToFirmware.  An FSD-issued
     * IRP_MJ_FLUSH_BUFFERS never reaches this table either: partmgr's
     * gated slot already completes it with the IO manager's default
     * STATUS_INVALID_DEVICE_REQUEST, which the FSD swallows. */
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_SHUTDOWN]       = ClassShutdownFlush;
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_FLUSH_BUFFERS]  = ClassShutdownFlush;
#endif
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_PNP]            = ClassDispatchPnp;
#ifndef SARCH_XBOX
    /* ntoskrnl/po and classpnp power.c are unlinked on Xbox; no Po* API
     * is exported, so no title can issue IRP_MJ_POWER. */
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_POWER]          = ClassDispatchPower;
#endif
#ifndef SARCH_XBOX
    /* Xbox has no WMI provider surface; leave the slot as the IO mgr's
     * default IopInvalidDeviceRequest so the dispatcher GCs out. */
    DriverExtension->DeviceMajorFunctionTable[IRP_MJ_SYSTEM_CONTROL] = ClassSystemControl;
#endif


    return;
}


NTSTATUS
NTAPI /* ReactOS Change: GCC Does not support STDCALL by default */
ClassGlobalDispatch(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

{
    PCOMMON_DEVICE_EXTENSION commonExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
    // Code Analysis cannot analyze the code paths specific to clients.
    _Analysis_assume_(FALSE);
    return (commonExtension->DispatchTable[irpStack->MajorFunction])(DeviceObject, Irp);

}

NTSTATUS
NTAPI /* ReactOS Change: GCC Does not support STDCALL by default */
ClassDispatchUnimplemented(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
/*++

Routine Description:

    This function is the default dispatch routine.  Its
    responsibility is simply to set the status in the packet to indicate
    that the operation requested is invalid for this device type, and then
    complete the packet.

Arguments:

    DeviceObject - Specifies the device object for which this request is
        bound.  Ignored by this routine.

    Irp - Specifies the address of the I/O Request Packet (IRP) for this
        request.

Return Value:

    The final status is always STATUS_INVALID_DEVICE_REQUEST.


--*/

{
    UNREFERENCED_PARAMETER( DeviceObject );

    //
    // Simply store the appropriate status, complete the request, and return
    // the same status stored in the packet.
    //

#ifndef SARCH_XBOX
    if ((IoGetCurrentIrpStackLocation(Irp))->MajorFunction == IRP_MJ_POWER) {
        PoStartNextPowerIrp(Irp);
    }
#endif
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    IoCompleteRequest( Irp, IO_NO_INCREMENT );
    return STATUS_INVALID_DEVICE_REQUEST;
}


