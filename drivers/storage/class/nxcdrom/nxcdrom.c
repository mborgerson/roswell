/*
 * PROJECT:     nxkrnl -- the unified Xbox kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal read-only optical (CD/DVD) class driver
 *
 * Replaces the stock ReactOS cdrom driver (a 32 KLOC KMDF driver) to avoid
 * dragging in KMDF (~2 MB) against the one-PE goal.  A thin CD class driver
 * on classpnp, which owns the PnP state machine, issues READ(10) SRBs, and
 * answers the storage IOCTLs; this driver only adds the FDO, one-time
 * SRB/sense setup + READ CAPACITY probe, read bounds-checking, and the few
 * IOCTL_CDROM_* requests cdfs issues to mount the disc.  Linked into the
 * kernel image as a static library; DriverEntry is renamed to
 * CdromDriverEntry by the build to avoid colliding with other folded-in
 * drivers.
 */

#include "ntddk.h"
#include "scsi.h"
#include "classpnp.h"
#include "ntddcdrm.h"
#include "ntstrsafe.h"
#include <ndk/section_attribs.h>

/* "Nxcd" -- pool tag for this driver's allocations. */
#define CDROM_TAG               'dcxN'

/* SCSI command timeout, in seconds.  Optical spin-up is slow on real
 * hardware; under the emulated drive it is immediate. */
#define CDROM_TIMEOUT           30

/* SRB lookaside depth -- this driver issues only the occasional probe SRB
 * itself; classpnp's read path has its own transfer-packet pool. */
#define CDROM_SRB_LIST_SIZE     2

/* Per-FDO driver data, carried in the class device extension after the
 * FUNCTIONAL_DEVICE_EXTENSION (see CdromDriverEntry's DeviceExtensionSize). */
typedef struct _CDROM_DEVICE_DATA
{
    BOOLEAN DosLinkCreated;
} CDROM_DEVICE_DATA, *PCDROM_DEVICE_DATA;

DRIVER_INITIALIZE DriverEntry;          /* renamed to CdromDriverEntry */

/* Build the legacy \DosDevices\CdRomN name into Name, using the caller's
 * WCHAR scratch (>= 32 chars). */
static VOID
CdromDosDeviceName(
    _In_ ULONG DeviceNumber,
    _Out_ PUNICODE_STRING Name,
    _Out_writes_(32) PWCHAR Scratch)
{
    RtlStringCchPrintfW(Scratch, 32, L"\\DosDevices\\CdRom%d", DeviceNumber);
    RtlInitUnicodeString(Name, Scratch);
}

/*
 * CdromAddDevice -- classpnp AddDevice callback.
 *
 * Build the optical FDO over the PDO atapi enumerated, attach it to the
 * device stack, name it \Device\CdRomN and give it the legacy
 * \DosDevices\CdRomN symlink.  classpnp drives the rest of the PnP
 * lifecycle through the CLASS_INIT_DATA callbacks.
 */
static NTSTATUS
NTAPI
CdromAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PCONFIGURATION_INFORMATION configInfo;
    PDEVICE_OBJECT lowerDevice = NULL;
    PDEVICE_OBJECT deviceObject = NULL;
    PFUNCTIONAL_DEVICE_EXTENSION fdoExtension;
    PCDROM_DEVICE_DATA cdData;
    CHAR deviceName[32];
    WCHAR linkScratch[32];
    UNICODE_STRING linkName;
    ULONG deviceNumber;
    NTSTATUS status;

    PAGED_CODE();

    configInfo = IoGetConfigurationInformation();
    deviceNumber = configInfo->CdRomCount;

    status = RtlStringCchPrintfA(deviceName, RTL_NUMBER_OF(deviceName),
                                 "\\Device\\CdRom%d", deviceNumber);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    /* Reserve the device on the bus so no other class driver claims it. */
    lowerDevice = IoGetAttachedDeviceReference(Pdo);

    status = ClassClaimDevice(lowerDevice, FALSE);
    if (!NT_SUCCESS(status))
    {
        ObDereferenceObject(lowerDevice);
        return status;
    }

    status = ClassCreateDeviceObject(DriverObject, deviceName, Pdo, TRUE,
                                     &deviceObject);
    if (!NT_SUCCESS(status))
    {
        ClassClaimDevice(lowerDevice, TRUE);
        ObDereferenceObject(lowerDevice);
        return status;
    }

    fdoExtension = deviceObject->DeviceExtension;
    cdData = fdoExtension->CommonExtension.DriverData;

    /* Reads carry MDLs -- classpnp's transfer packets expect direct I/O. */
    SET_FLAG(deviceObject->Flags, DO_DIRECT_IO);

    if (lowerDevice->AlignmentRequirement > deviceObject->AlignmentRequirement)
    {
        deviceObject->AlignmentRequirement = lowerDevice->AlignmentRequirement;
    }

    fdoExtension->LowerPdo = Pdo;
    fdoExtension->CommonExtension.LowerDeviceObject =
        IoAttachDeviceToDeviceStack(deviceObject, Pdo);
    if (fdoExtension->CommonExtension.LowerDeviceObject == NULL)
    {
        IoDeleteDevice(deviceObject);
        ClassClaimDevice(lowerDevice, TRUE);
        ObDereferenceObject(lowerDevice);
        return STATUS_UNSUCCESSFUL;
    }

    fdoExtension->DeviceNumber = deviceNumber;

    /* The legacy \DosDevices\CdRomN name.  The boot path reaches the disc
     * through the ARC name -> \Device\CdRomN; this link is for completeness
     * and for anything that still opens the drive the old way. */
    CdromDosDeviceName(deviceNumber, &linkName, linkScratch);
    if (NT_SUCCESS(IoCreateSymbolicLink(&linkName,
                                        &fdoExtension->CommonExtension.DeviceName)))
    {
        cdData->DosLinkCreated = TRUE;
    }

    CLEAR_FLAG(deviceObject->Flags, DO_DEVICE_INITIALIZING);

    configInfo->CdRomCount++;

    ObDereferenceObject(lowerDevice);
    return STATUS_SUCCESS;
}

/*
 * CdromInitDevice -- classpnp one-time device init callback.
 *
 * classpnp has already fetched the adapter and device descriptors, so SRBs
 * can be sent from here.  Set up the sense buffer, SRB flags and timeout the
 * class ABI expects, then probe the disc geometry with READ CAPACITY.
 */
static NTSTATUS
NTAPI
CdromInitDevice(
    _In_ PDEVICE_OBJECT Fdo)
{
    PFUNCTIONAL_DEVICE_EXTENSION fdoExtension = Fdo->DeviceExtension;

    PAGED_CODE();

    ClassInitializeSrbLookasideList(&fdoExtension->CommonExtension,
                                    CDROM_SRB_LIST_SIZE);

    fdoExtension->SenseData = ExAllocatePoolWithTag(NonPagedPoolCacheAligned,
                                                    SENSE_BUFFER_SIZE_EX,
                                                    CDROM_TAG);
    if (fdoExtension->SenseData == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    fdoExtension->SenseDataLength = SENSE_BUFFER_SIZE_EX;

    /* Let the port driver allocate a fresh sense buffer per request. */
    SET_FLAG(fdoExtension->SrbFlags, SRB_FLAGS_PORT_DRIVER_ALLOCSENSE);

    fdoExtension->TimeOutValue = ClassQueryTimeOutRegistryValue(Fdo);
    if (fdoExtension->TimeOutValue == 0)
    {
        fdoExtension->TimeOutValue = CDROM_TIMEOUT;
    }

    fdoExtension->CommonExtension.StartingOffset.QuadPart = 0;

    /* Probe the disc.  classpnp issues READ CAPACITY and fills DiskGeometry
     * (2048-byte sectors) and CommonExtension.PartitionLength.  A failure
     * here is not fatal -- it just means no media yet. */
    (VOID)ClassReadDriveCapacity(Fdo);

    return STATUS_SUCCESS;
}

/*
 * CdromStartDevice -- classpnp start callback, called on every PnP start.
 * Re-probe the disc so the geometry reflects whatever is in the drive now.
 */
static
NTSTATUS
NTAPI
CdromStartDevice(
    _In_ PDEVICE_OBJECT Fdo)
{
    PAGED_CODE();

    (VOID)ClassReadDriveCapacity(Fdo);
    return STATUS_SUCCESS;
}

#ifndef SARCH_XBOX
/* The classpnp STOP/REMOVE dispatch arms are gated on Xbox, so these
 * callbacks can never be invoked. */
static NTSTATUS
NTAPI
CdromStopDevice(
    _In_ PDEVICE_OBJECT Fdo,
    _In_ UCHAR Type)
{
    UNREFERENCED_PARAMETER(Fdo);
    UNREFERENCED_PARAMETER(Type);
    return STATUS_SUCCESS;
}

/*
 * CdromRemoveDevice -- classpnp remove callback.  Release this driver's
 * resources; classpnp detaches and deletes the device object itself.
 */
static NTSTATUS
NTAPI
CdromRemoveDevice(
    _In_ PDEVICE_OBJECT Fdo,
    _In_ UCHAR Type)
{
    PFUNCTIONAL_DEVICE_EXTENSION fdoExtension = Fdo->DeviceExtension;
    PCDROM_DEVICE_DATA cdData = fdoExtension->CommonExtension.DriverData;

    PAGED_CODE();

    if (Type == IRP_MN_QUERY_REMOVE_DEVICE ||
        Type == IRP_MN_CANCEL_REMOVE_DEVICE)
    {
        return STATUS_SUCCESS;
    }

    if (cdData->DosLinkCreated)
    {
        WCHAR linkScratch[32];
        UNICODE_STRING linkName;

        CdromDosDeviceName(fdoExtension->DeviceNumber, &linkName, linkScratch);
        IoDeleteSymbolicLink(&linkName);
        cdData->DosLinkCreated = FALSE;
    }

    if (Type == IRP_MN_REMOVE_DEVICE)
    {
        if (fdoExtension->SenseData != NULL)
        {
            ExFreePoolWithTag(fdoExtension->SenseData, CDROM_TAG);
            fdoExtension->SenseData = NULL;
        }
        ClassDeleteSrbLookasideList(&fdoExtension->CommonExtension);
    }

    return STATUS_SUCCESS;
}
#endif /* SARCH_XBOX */

/*
 * CdromReadWriteVerification -- classpnp read/write pre-pass filter.
 *
 * The disc is read-only, so reject writes.  For reads, bounds- and
 * alignment-check the request.  A request that is sector-aligned but runs
 * off the end of the media is passed through as success: the cache manager
 * rounds reads up to a page, so a read of the last sectors legitimately
 * extends past PartitionLength -- let the hardware return what it has.
 */
static NTSTATUS
NTAPI
CdromReadWriteVerification(
    _In_ PDEVICE_OBJECT Fdo,
    _In_ PIRP Irp)
{
    PFUNCTIONAL_DEVICE_EXTENSION fdoExtension = Fdo->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG sectorSize = fdoExtension->DiskGeometry.BytesPerSector;
    LARGE_INTEGER offset = irpSp->Parameters.Read.ByteOffset;
    ULONG length = irpSp->Parameters.Read.Length;
    NTSTATUS status = STATUS_SUCCESS;

    if (irpSp->MajorFunction == IRP_MJ_WRITE)
    {
        status = STATUS_MEDIA_WRITE_PROTECTED;
    }
    else if (sectorSize == 0 ||
             (length & (sectorSize - 1)) != 0 ||
             (offset.LowPart & (sectorSize - 1)) != 0)
    {
        /* Misaligned -- the SCSI READ would be malformed. */
        status = STATUS_INVALID_PARAMETER;
    }
    else if (offset.QuadPart < 0)
    {
        status = STATUS_INVALID_PARAMETER;
    }
    /* else: aligned.  Even if it runs past PartitionLength, allow it. */

    Irp->IoStatus.Status = status;
    return status;
}

/*
 * CdromReadToc -- service IOCTL_CDROM_READ_TOC.
 *
 * Issue a real SCSI READ TOC (format 0, LBA addresses) and hand the drive's
 * response straight back: the SCSI READ TOC reply layout is exactly the
 * CDROM_TOC structure cdfs expects.  classpnp's ClassSendSrbSynchronous
 * fills in the SRB function, length, sense and data-buffer fields and waits
 * for completion; this routine only sets the CDB.
 */
static NTSTATUS
CdromReadToc(
    _In_ PDEVICE_OBJECT Fdo,
    _In_ PIRP Irp)
{
    PFUNCTIONAL_DEVICE_EXTENSION fdoExtension = Fdo->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG outputLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID toc = Irp->AssociatedIrp.SystemBuffer;
    SCSI_REQUEST_BLOCK srb;
    PCDB cdb;
    ULONG transferLength;
    NTSTATUS status;

    if (toc == NULL || outputLength < sizeof(CDROM_TOC))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* The READ TOC reply for a data disc is tiny (a header plus the track 1
     * and lead-out descriptors); cap the request at one CDROM_TOC. */
    transferLength = sizeof(CDROM_TOC);

    RtlZeroMemory(&srb, sizeof(srb));
    srb.CdbLength = 10;
    srb.TimeOutValue = fdoExtension->TimeOutValue;

    cdb = (PCDB)srb.Cdb;
    cdb->READ_TOC.OperationCode = SCSIOP_READ_TOC;
    cdb->READ_TOC.Format2 = 0;          /* format 0: the TOC */
    cdb->READ_TOC.StartingTrack = 0;
    cdb->READ_TOC.AllocationLength[0] = (UCHAR)(transferLength >> 8);
    cdb->READ_TOC.AllocationLength[1] = (UCHAR)(transferLength & 0xFF);
    /* Msf left 0 -> the descriptors carry LBA addresses. */

    status = ClassSendSrbSynchronous(Fdo, &srb, toc, transferLength, FALSE);

    /* A short transfer (the drive returned fewer bytes than requested) is
     * reported as SRB_STATUS_DATA_OVERRUN but is the normal, healthy case. */
    if (NT_SUCCESS(status) ||
        SRB_STATUS(srb.SrbStatus) == SRB_STATUS_DATA_OVERRUN)
    {
        Irp->IoStatus.Information = srb.DataTransferLength;
        return STATUS_SUCCESS;
    }

    return status;
}

/*
 * CdromDeviceControl -- classpnp device-control callback.
 *
 * Handle the IOCTL_CDROM_* requests cdfs issues to recognise and mount the
 * boot disc; forward everything else to classpnp, which answers the
 * IOCTL_STORAGE_* family and passes the remainder down to atapi.
 */
static NTSTATUS
NTAPI
CdromDeviceControl(
    _In_ PDEVICE_OBJECT Fdo,
    _In_ PIRP Irp)
{
    PFUNCTIONAL_DEVICE_EXTENSION fdoExtension = Fdo->DeviceExtension;
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG controlCode = irpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG outputLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status;

    Irp->IoStatus.Information = 0;

    switch (controlCode)
    {
        case IOCTL_CDROM_GET_DRIVE_GEOMETRY:
        {
            if (outputLength < sizeof(DISK_GEOMETRY))
            {
                status = STATUS_BUFFER_TOO_SMALL;
            }
            else
            {
                RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                              &fdoExtension->DiskGeometry,
                              sizeof(DISK_GEOMETRY));
                Irp->IoStatus.Information = sizeof(DISK_GEOMETRY);
                status = STATUS_SUCCESS;
            }
            break;
        }

        case IOCTL_CDROM_READ_TOC:
        {
            status = CdromReadToc(Fdo, Irp);
            break;
        }

        case IOCTL_CDROM_READ_TOC_EX:
        {
            /* Not supported -- cdfs falls back to plain IOCTL_CDROM_READ_TOC
             * when READ_TOC_EX returns STATUS_NOT_IMPLEMENTED. */
            status = STATUS_NOT_IMPLEMENTED;
            break;
        }

        case IOCTL_CDROM_CHECK_VERIFY:
        {
            /* classpnp implements the storage-base equivalent (TEST UNIT
             * READY plus the media-change count); re-dispatch as that. */
            irpSp->Parameters.DeviceIoControl.IoControlCode =
                IOCTL_STORAGE_CHECK_VERIFY;
            return ClassDeviceControl(Fdo, Irp);
        }

        default:
        {
            /* IOCTL_STORAGE_*, IOCTL_SCSI_GET_CAPABILITIES, the remaining
             * IOCTL_CDROM_* -- classpnp answers the storage IOCTLs and
             * forwards the rest down to the port driver. */
            return ClassDeviceControl(Fdo, Irp);
        }
    }

    Irp->IoStatus.Status = status;
    ClassReleaseRemoveLock(Fdo, Irp);
    ClassCompleteRequest(Fdo, Irp, IO_NO_INCREMENT);
    return status;
}

static VOID
NTAPI
CdromUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    /* Folded into the kernel image -- never unloaded. */
    UNREFERENCED_PARAMETER(DriverObject);
}

/*
 * DriverEntry (CdromDriverEntry) -- run by IopInitializeBootDrivers through
 * IoCreateDriver.  Register the class callbacks with classpnp; classpnp
 * installs the dispatch table and calls CdromAddDevice when atapi enumerates
 * the optical drive.
 */
CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    CLASS_INIT_DATA initData = {0};

    initData.InitializationDataSize = sizeof(CLASS_INIT_DATA);

    initData.FdoData.DeviceExtensionSize =
        sizeof(FUNCTIONAL_DEVICE_EXTENSION) + sizeof(CDROM_DEVICE_DATA);
    initData.FdoData.DeviceType = FILE_DEVICE_CD_ROM;
    initData.FdoData.DeviceCharacteristics =
        FILE_DEVICE_SECURE_OPEN | FILE_REMOVABLE_MEDIA | FILE_READ_ONLY_DEVICE;

    initData.FdoData.ClassInitDevice = CdromInitDevice;
    initData.FdoData.ClassStartDevice = CdromStartDevice;
#ifndef SARCH_XBOX
    initData.FdoData.ClassStopDevice = CdromStopDevice;
    initData.FdoData.ClassRemoveDevice = CdromRemoveDevice;
#endif
    initData.FdoData.ClassReadWriteVerification = CdromReadWriteVerification;
    initData.FdoData.ClassDeviceControl = CdromDeviceControl;

    initData.ClassAddDevice = CdromAddDevice;
#ifndef SARCH_XBOX
    /* Xbox kernel folds nxcdrom into the kernel image and never unloads it;
     * the assignment / CdromUnload thunk is dead. */
    initData.ClassUnload = CdromUnload;
#endif

    return (NTSTATUS)ClassInitialize(DriverObject, RegistryPath, &initData);
}
