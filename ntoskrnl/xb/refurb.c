/*
 * PROJECT:     nxkrnl -- a free kernel for the original Xbox
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The console's refurbishment record.
 *
 * A refurbished console carries a note of when it was reworked and how
 * many times it had been powered up by then.  It is not in the EEPROM
 * with the rest of the console's settings: it sits in the fourth sector
 * of the hard disk, ahead of every partition, as sixteen plain bytes
 * behind a four-byte marker.
 *
 * That placement, and the shapes below, were read off the console: the
 * record was written through the ordinal on a scratch disk and the
 * sector it landed in found by searching the image for it.
 */

#include <ntdef.h>
#include <ntifs.h>

#define REFURB_SECTOR      3
#define REFURB_SECTOR_SIZE 512

/* 'BRFR' as the console stores it.  A write stamps this over whatever
 * marker the caller brought, and a read that does not find it hands
 * back a zeroed record rather than the bytes that are there. */
#define REFURB_MARKER      0x52465242

typedef struct _XBOX_REFURB_INFO
{
    ULONG Signature;
    ULONG PowerCycleCount;
    LARGE_INTEGER FirstSetTime;
} XBOX_REFURB_INFO, *PXBOX_REFURB_INFO;

C_ASSERT(sizeof(XBOX_REFURB_INFO) == 16);

static const WCHAR NxkRefurbDeviceName[] = L"\\Device\\Harddisk0\\Partition0";

/*
 * The sector is read or written whole: nothing else lives in it, but
 * the disk cannot be addressed in smaller pieces than that.
 */
static NTSTATUS
NxkRefurbSector(_Inout_updates_bytes_(REFURB_SECTOR_SIZE) PVOID Sector,
                _In_ BOOLEAN Write)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING DeviceName;
    LARGE_INTEGER Offset;
    HANDLE Handle;
    NTSTATUS Status;

    RtlInitUnicodeString(&DeviceName, NxkRefurbDeviceName);
    InitializeObjectAttributes(&ObjectAttributes, &DeviceName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);

    Status = NtCreateFile(&Handle,
                          (Write ? FILE_WRITE_DATA : 0) | FILE_READ_DATA |
                              SYNCHRONIZE,
                          &ObjectAttributes, &IoStatusBlock, NULL,
                          FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                          FILE_NO_INTERMEDIATE_BUFFERING |
                              FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
    if (!NT_SUCCESS(Status))
        return Status;

    Offset.QuadPart = (LONGLONG)REFURB_SECTOR * REFURB_SECTOR_SIZE;
    if (Write)
        Status = NtWriteFile(Handle, NULL, NULL, NULL, &IoStatusBlock, Sector,
                             REFURB_SECTOR_SIZE, &Offset, NULL);
    else
        Status = NtReadFile(Handle, NULL, NULL, NULL, &IoStatusBlock, Sector,
                            REFURB_SECTOR_SIZE, &Offset, NULL);

    NtClose(Handle);
    return Status;
}

NTSTATUS NTAPI
ExReadWriteRefurbInfo(PXBOX_REFURB_INFO RefurbInfo, ULONG ValueLength,
                      BOOLEAN DoWrite)
{
    PXBOX_REFURB_INFO Stored;
    PVOID Sector;
    NTSTATUS Status;

    if (ValueLength != sizeof(XBOX_REFURB_INFO))
        return STATUS_INVALID_PARAMETER;

    Sector = ExAllocatePoolWithTag(NonPagedPool, REFURB_SECTOR_SIZE, 'fRxX');
    if (Sector == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NxkRefurbSector(Sector, FALSE);
    if (!NT_SUCCESS(Status))
    {
        ExFreePool(Sector);
        return Status;
    }

    Stored = (PXBOX_REFURB_INFO)Sector;
    if (DoWrite)
    {
        *Stored = *RefurbInfo;
        Stored->Signature = REFURB_MARKER;
        Status = NxkRefurbSector(Sector, TRUE);
    }
    else if (Stored->Signature == REFURB_MARKER)
    {
        *RefurbInfo = *Stored;
    }
    else
    {
        RtlZeroMemory(RefurbInfo, sizeof(*RefurbInfo));
    }

    ExFreePool(Sector);
    return Status;
}

/* EOF */
