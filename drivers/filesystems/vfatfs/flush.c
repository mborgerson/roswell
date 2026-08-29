/*
 * PROJECT:     VFAT Filesystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Flushing routines
 * COPYRIGHT:   Copyright 2004-2013 Eric Kohl <eric.kohl@reactos.org>
 *              Copyright 2014-2018 Pierre Schweitzer <pierre@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "vfat.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

static
NTSTATUS
VfatFlushFile(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB Fcb)
{
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    DPRINT("VfatFlushFile(DeviceExt %p, Fcb %p) for '%wZ'\n", DeviceExt, Fcb, &Fcb->PathNameU);

    CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &IoStatus);
    if (IoStatus.Status == STATUS_INVALID_PARAMETER)
    {
        /* FIXME: Caching was possible not initialized */
        IoStatus.Status = STATUS_SUCCESS;
    }

    ExAcquireResourceExclusiveLite(&DeviceExt->DirResource, TRUE);
    if (BooleanFlagOn(Fcb->Flags, FCB_IS_DIRTY))
    {
        Status = VfatUpdateEntry(DeviceExt, Fcb);
        if (!NT_SUCCESS(Status))
        {
            IoStatus.Status = Status;
        }
    }
    ExReleaseResourceLite(&DeviceExt->DirResource);

    return IoStatus.Status;
}

NTSTATUS
VfatFlushVolume(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB VolumeFcb)
{
    PLIST_ENTRY ListEntry;
    PVFATFCB Fcb;
    NTSTATUS Status, ReturnStatus = STATUS_SUCCESS;
    PIRP Irp;
    KEVENT Event;
    IO_STATUS_BLOCK IoStatusBlock;

    DPRINT("VfatFlushVolume(DeviceExt %p, VolumeFcb %p)\n", DeviceExt, VolumeFcb);

    ASSERT(VolumeFcb == DeviceExt->VolumeFcb);

    ListEntry = DeviceExt->FcbListHead.Flink;
    while (ListEntry != &DeviceExt->FcbListHead)
    {
        Fcb = CONTAINING_RECORD(ListEntry, VFATFCB, FcbListEntry);
        ListEntry = ListEntry->Flink;
        if (!vfatFCBIsDirectory(Fcb))
        {
            ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
            Status = VfatFlushFile(DeviceExt, Fcb);
            ExReleaseResourceLite (&Fcb->MainResource);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("VfatFlushFile failed, status = %x\n", Status);
                ReturnStatus = Status;
            }
        }
        /* FIXME: Stop flushing if this is a removable media and the media was removed */
    }

    ListEntry = DeviceExt->FcbListHead.Flink;
    while (ListEntry != &DeviceExt->FcbListHead)
    {
        Fcb = CONTAINING_RECORD(ListEntry, VFATFCB, FcbListEntry);
        ListEntry = ListEntry->Flink;
        if (vfatFCBIsDirectory(Fcb))
        {
            ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
            Status = VfatFlushFile(DeviceExt, Fcb);
            ExReleaseResourceLite (&Fcb->MainResource);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("VfatFlushFile failed, status = %x\n", Status);
                ReturnStatus = Status;
            }
        }
        /* FIXME: Stop flushing if this is a removable media and the media was removed */
    }

    Fcb = (PVFATFCB) DeviceExt->FATFileObject->FsContext;

    ExAcquireResourceExclusiveLite(&DeviceExt->FatResource, TRUE);
    Status = VfatFlushFile(DeviceExt, Fcb);
    ExReleaseResourceLite(&DeviceExt->FatResource);

    /* Prepare an IRP to flush device buffers */
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_FLUSH_BUFFERS,
                                       DeviceExt->StorageDevice,
                                       NULL, 0, NULL, &Event,
                                       &IoStatusBlock);
    if (Irp != NULL)
    {
        KeInitializeEvent(&Event, NotificationEvent, FALSE);

        Status = IoCallDriver(DeviceExt->StorageDevice, Irp);
        if (Status == STATUS_PENDING)
        {
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = IoStatusBlock.Status;
        }

        /* Ignore device not supporting flush operation */
        if (Status == STATUS_INVALID_DEVICE_REQUEST)
        {
            DPRINT1("Flush not supported, ignored\n");
            Status = STATUS_SUCCESS;

        }
    }
    else
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("VfatFlushFile failed, status = %x\n", Status);
        ReturnStatus = Status;
    }

    return ReturnStatus;
}

/* Retail FATX survives an abrupt SMC power-off -- the console's power button
 * has no clean-shutdown path -- which the NT cache manager's lazy write-back
 * does not: a freshly created file/dir can reach the platter while the FAT
 * that records its cluster chain does not, so the next mount finds an entry
 * whose chain the FAT no longer holds, leaks the clusters, and eventually
 * reports a spurious STATUS_DISK_FULL (roswell#20).  When write-through is on
 * (default for Xbox, VFAT_WRITE_THROUGH), make an operation durable the way
 * retail does -- on every write and on close -- by flushing this file/dir's
 * own stream (its data, or a directory's child entries), every ancestor
 * directory stream up to the root (they hold its entry, and the entries the
 * operation rewrote when it extended a parent), and the FAT.  A no-op when
 * the flag is cleared, which relaxes power-loss durability for throughput. */
VOID
VfatFlushWriteThrough(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB pFcb)
{
    IO_STATUS_BLOCK IoStatus;
    PVFATFCB dir;

    if (!BooleanFlagOn(VfatGlobalData->Flags, VFAT_WRITE_THROUGH))
        return;

    for (dir = pFcb; dir != NULL; dir = dir->parentFcb)
    {
        /* CcFlushCache is a no-op when the stream was never cached, so call it
         * unconditionally: a file's own FCB carries no FileObject and never
         * sets FCB_CACHE_INITIALIZED (that flag is directory-only), yet its
         * written data must be flushed too, not just the ancestor entries. */
        CcFlushCache(&dir->SectionObjectPointers, NULL, 0, &IoStatus);
        if (vfatFCBIsRoot(dir))
            break;
    }

    if (DeviceExt->FATFileObject != NULL)
    {
        PVFATFCB FatFcb = (PVFATFCB)DeviceExt->FATFileObject->FsContext;
        if (FatFcb != NULL)
            CcFlushCache(&FatFcb->SectionObjectPointers, NULL, 0, &IoStatus);
    }
}

NTSTATUS
VfatFlush(
    PVFAT_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status;
    PVFATFCB Fcb;

    /* This request is not allowed on the main device object. */
    if (IrpContext->DeviceObject == VfatGlobalData->DeviceObject)
    {
        IrpContext->Irp->IoStatus.Information = 0;
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Fcb = (PVFATFCB)IrpContext->FileObject->FsContext;
    ASSERT(Fcb);

    if (BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME))
    {
        ExAcquireResourceExclusiveLite(&IrpContext->DeviceExt->DirResource, TRUE);
        Status = VfatFlushVolume(IrpContext->DeviceExt, Fcb);
        ExReleaseResourceLite(&IrpContext->DeviceExt->DirResource);
    }
    else
    {
        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
        Status = VfatFlushFile(IrpContext->DeviceExt, Fcb);
        ExReleaseResourceLite (&Fcb->MainResource);
    }

    IrpContext->Irp->IoStatus.Information = 0;
    return Status;
}

/* EOF */
