/*
 * PROJECT:     nxkrnl -- the unified Xbox kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Read-only XDVDFS (Xbox optical) file-system driver.
 *
 * XDVDFS is the on-disc file system used by retail Xbox titles.  It is a
 * 2048-byte-sector, read-only layout consisting of one volume descriptor at
 * LBA 32 and a tree of directory-entry tables.  The driver mounts
 * \Device\CdRom0 (the optical FDO published by nxcdrom), turns title-side
 * NtCreateFile("D:\\...") into a B-tree walk over those dirent tables, and
 * services NtReadFile by issuing READ(10) SRBs on the underlying disc.
 *
 * Scope: lookup, read, info-query, directory enumeration.  No write path,
 * no cache manager, no FSCTLs beyond MOUNT/VERIFY.
 *
 * Wiring: built as a static library; ntoskrnl's IopInitializeBootDrivers
 * runs XdvdfsDriverEntry via IoCreateDriver(\FileSystem\Xdvdfs) BEFORE
 * the stock CdfsDriverEntry, so XDVDFS gets first shot at MOUNT_VOLUME.
 * Stock cdfs (ISO9660) remains in the build as a fallback for non-Xbox media.
 *
 * XDVDFS layout reference (xdvdfs-core, xboxdevwiki):
 *   - Volume descriptor at LBA 32 (file offset 0x10000), 2048 bytes:
 *       +0x000 [20] magic "MICROSOFT*XBOX*MEDIA"
 *       +0x014 u32 root_table_sector
 *       +0x018 u32 root_table_size (bytes, multiple of 0x800)
 *       +0x01C u64 filetime
 *       +0x7EC [20] same magic
 *   - Directory entry node (variable length, dword-aligned within the table):
 *       +0x00 u16 left_offset    (in 4-byte units; 0 or 0xFFFF = no child)
 *       +0x02 u16 right_offset   (same convention)
 *       +0x04 u32 data_sector
 *       +0x08 u32 data_size      (bytes)
 *       +0x0C u8  attributes     (DIRECTORY=0x10, RO=0x01, HID=0x02, ...)
 *       +0x0D u8  name_length
 *       +0x0E [name_length] ASCII name, then pad to 4-byte alignment
 *     Empty slots in a dirent-table sector are filled with 0xFF, so a
 *     name_length of 0xFF means "skip rest of sector".
 *   - Walk: case-insensitive ASCII compare; left child if needle < node,
 *     right child if needle > node.
 */

#include <ntifs.h>
#include <ntdddisk.h>
#include <ntddcdrm.h>
#include <ntddstor.h>
#include <ntstrsafe.h>

/* --- on-disc layout ------------------------------------------------------ */

#define XDVD_SECTOR_SIZE                2048u
#define XDVD_SECTOR_SHIFT               11u
#define XDVD_VOLUME_DESCRIPTOR_LBA      32u

#define XDVD_ATTR_READONLY              0x01u
#define XDVD_ATTR_HIDDEN                0x02u
#define XDVD_ATTR_SYSTEM                0x04u
#define XDVD_ATTR_DIRECTORY             0x10u
#define XDVD_ATTR_ARCHIVE               0x20u
#define XDVD_ATTR_NORMAL                0x80u

#define XDVD_VOLUME_MAGIC               "MICROSOFT*XBOX*MEDIA"
#define XDVD_VOLUME_MAGIC_LEN           20u
#define XDVD_VOLUME_MAGIC_OFF_2         0x7ECu

#include <pshpack1.h>
#include <ndk/section_attribs.h>
typedef struct _XDVD_VOLUME_DESCRIPTOR
{
    UCHAR  Magic0[XDVD_VOLUME_MAGIC_LEN];
    ULONG  RootTableSector;
    ULONG  RootTableSize;
    ULONGLONG Filetime;
    UCHAR  Unused[0x7C8];
    UCHAR  Magic1[XDVD_VOLUME_MAGIC_LEN];
} XDVD_VOLUME_DESCRIPTOR, *PXDVD_VOLUME_DESCRIPTOR;
C_ASSERT(sizeof(XDVD_VOLUME_DESCRIPTOR) == 0x800);

typedef struct _XDVD_DIRENT
{
    USHORT LeftOffset;       /* in 4-byte units */
    USHORT RightOffset;      /* in 4-byte units */
    ULONG  DataSector;
    ULONG  DataSize;
    UCHAR  Attributes;
    UCHAR  NameLength;
    CHAR   Name[1];          /* NameLength bytes; not NUL-terminated */
} XDVD_DIRENT, *PXDVD_DIRENT;
#include <poppack.h>

/* Fixed prefix size of a dirent on disc (everything before the name). */
#define XDVD_DIRENT_FIXED_SIZE          0x0Eu
#define XDVD_DIRENT_EMPTY_SENTINEL      0xFFu  /* NameLength==0xFF -> skip slot */

/* --- driver-wide state --------------------------------------------------- */

#define XDVD_TAG                      'dvdX'  /* "Xdvd" */
#define XDVD_FCB_TAG                  'FvdX'  /* "XdvF" */
#define XDVD_VCB_TAG                  'VvdX'  /* "XdvV" */
#define XDVD_CCB_TAG                  'CvdX'  /* "XdvC" */
#define XDVD_ENUM_TAG                 'EvdX'  /* "XdvE" */

typedef struct _XDVD_GLOBALS
{
    PDRIVER_OBJECT     DriverObject;
    PDEVICE_OBJECT     ControlDevice;          /* \FileSystem\Xdvdfs */
    KSPIN_LOCK         VcbListLock;
    LIST_ENTRY         VcbList;
} XDVD_GLOBALS;

static XDVD_GLOBALS XdvdGlobals;

/*
 * Volume control block.  Carried as DeviceExtension on the volume device
 * object we publish when a disc is mounted.
 */
typedef struct _XDVD_VCB
{
    LIST_ENTRY         ListEntry;
    PDEVICE_OBJECT     VolumeDevice;           /* this device (us)            */
    PDEVICE_OBJECT     TargetDevice;           /* lower stack: \Device\CdRom0 */
    PVPB               Vpb;
    ULONG              RootSector;
    ULONG              RootSize;               /* bytes                       */
    LONG               OpenCount;
    BOOLEAN            Mounted;
} XDVD_VCB, *PXDVD_VCB;

/*
 * File control block.  Sits in FILE_OBJECT::FsContext.  Self-contained --
 * holds the dirent fields we care about plus the file's name for query.
 * The Name buffer is allocated with the FCB and follows in memory.
 */
typedef struct _XDVD_FCB
{
    PXDVD_VCB        Vcb;
    ULONG              DataSector;
    ULONG              DataSize;
    UCHAR              Attributes;
    BOOLEAN            IsDirectory;
    BOOLEAN            IsRoot;
    UNICODE_STRING     Name;                   /* trailing buffer; basename   */
} XDVD_FCB, *PXDVD_FCB;

/*
 * Per-handle (context-control-block) state.  Sits in FILE_OBJECT::FsContext2.
 * Allocated lazily for directory enumeration; NULL on plain files.
 */
typedef struct _XDVD_CCB
{
    /* Pre-walked directory contents, used when the handle enumerates the
     * directory through IRP_MJ_DIRECTORY_CONTROL.  Each entry is one dirent
     * found in the tree (preorder).  Allocated on first IRP_MN_QUERY_DIRECTORY.
     */
    struct {
        ULONG  DataSector;
        ULONG  DataSize;
        UCHAR  Attributes;
        UCHAR  NameLength;
        CHAR   Name[256];
    } *Entries;
    ULONG              EntryCount;
    ULONG              NextIndex;
    UNICODE_STRING     SearchPattern;          /* allocated; NULL = "*"       */
    BOOLEAN            EnumLoaded;
} XDVD_CCB, *PXDVD_CCB;

/* --- forward decls ------------------------------------------------------- */

DRIVER_INITIALIZE  DriverEntry;                /* renamed to XdvdfsDriverEntry */

static NTSTATUS NTAPI XdvdDispatchCreate(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchCleanup(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchClose(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchRead(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchWrite(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchQueryInformation(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchSetInformation(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchQueryVolumeInformation(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchDirectoryControl(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchFileSystemControl(PDEVICE_OBJECT, PIRP);
static NTSTATUS NTAPI XdvdDispatchDeviceControl(PDEVICE_OBJECT, PIRP);

/* --- small utils --------------------------------------------------------- */

static NTSTATUS
XdvdCompleteRequest(_Inout_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Info)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/*
 * Case-insensitive ASCII compare with the title-side ASCII name.  We compare
 * the leading min(len_a, len_b) bytes; if equal, shorter sorts before longer.
 * Matches the convention used by xdvdfs-core's `cmp_ignore_case_utf8` on plain
 * ASCII names (Xbox dirent names are always ASCII).
 */
static int
XdvdAsciiCompare(_In_reads_(LenA) PCSTR A, _In_ ULONG LenA,
                   _In_reads_(LenB) PCSTR B, _In_ ULONG LenB)
{
    ULONG i, n;
    UCHAR ca, cb;

    n = LenA < LenB ? LenA : LenB;
    for (i = 0; i < n; ++i)
    {
        ca = (UCHAR)A[i];
        cb = (UCHAR)B[i];
        if (ca >= 'a' && ca <= 'z') ca -= ('a' - 'A');
        if (cb >= 'a' && cb <= 'z') cb -= ('a' - 'A');
        if (ca != cb) return (int)ca - (int)cb;
    }
    if (LenA == LenB) return 0;
    return (LenA < LenB) ? -1 : 1;
}

/*
 * Issue a synchronous read on the lower (target) device.  The buffer must be
 * a non-paged virtual address; IoBuildSynchronousFsdRequest builds an MDL
 * over it for the underlying DO_DIRECT_IO stack.
 *
 * Returns STATUS_SUCCESS on full read; partial reads (e.g., last sector of
 * the disc) are reflected in *BytesRead and the call returns STATUS_SUCCESS.
 */
static NTSTATUS
XdvdTargetRead(_In_ PDEVICE_OBJECT Target,
                 _In_ ULONGLONG Offset,
                 _In_ ULONG Length,
                 _Out_writes_bytes_(Length) PVOID Buffer,
                 _Out_opt_ PULONG BytesRead)
{
    KEVENT event;
    PIRP irp;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    NTSTATUS status;

    off.QuadPart = (LONGLONG)Offset;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, Target, Buffer, Length,
                                       &off, &event, &iosb);
    if (irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = IoCallDriver(Target, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    if (BytesRead != NULL)
        *BytesRead = (ULONG)iosb.Information;

    return status;
}

/* --- dirent walk --------------------------------------------------------- */

/*
 * Read one whole dirent table into a fresh nonpaged buffer.  TableSizeOut
 * receives the table size in bytes (rounded up to a sector if needed).
 * Caller frees with ExFreePoolWithTag(buf, XDVD_TAG).
 */
static NTSTATUS
XdvdReadDirentTable(_In_ PXDVD_VCB Vcb,
                      _In_ ULONG TableSector,
                      _In_ ULONG TableSize,
                      _Outptr_ PVOID *TableOut,
                      _Out_ PULONG TableSizeOut)
{
    ULONG aligned;
    PVOID buf;
    NTSTATUS status;
    ULONG got = 0;

    *TableOut = NULL;
    *TableSizeOut = 0;

    if (TableSize == 0)
        return STATUS_NOT_FOUND;

    aligned = (TableSize + XDVD_SECTOR_SIZE - 1) & ~(XDVD_SECTOR_SIZE - 1);
    buf = ExAllocatePoolWithTag(NonPagedPool, aligned, XDVD_TAG);
    if (buf == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = XdvdTargetRead(Vcb->TargetDevice,
                              (ULONGLONG)TableSector << XDVD_SECTOR_SHIFT,
                              aligned, buf, &got);
    if (!NT_SUCCESS(status) || got < TableSize)
    {
        ExFreePoolWithTag(buf, XDVD_TAG);
        return NT_SUCCESS(status) ? STATUS_DISK_CORRUPT_ERROR : status;
    }

    *TableOut = buf;
    *TableSizeOut = aligned;
    return STATUS_SUCCESS;
}

/*
 * Look up Name inside a single dirent table (already in memory) by B-tree
 * walk.  Returns a pointer to the dirent struct on hit, or NULL on miss.
 */
static PXDVD_DIRENT
XdvdFindInTable(_In_reads_bytes_(TableSize) PUCHAR Table,
                  _In_ ULONG TableSize,
                  _In_reads_(NameLen) PCSTR Name,
                  _In_ ULONG NameLen)
{
    ULONG off = 0;
    PXDVD_DIRENT d;
    int cmp;
    ULONG next;

    for (;;)
    {
        if (off + XDVD_DIRENT_FIXED_SIZE > TableSize)
            return NULL;

        d = (PXDVD_DIRENT)(Table + off);

        /* Empty slot sentinel: rest of this sector is unused.  Tree edges
         * to an unused slot are encoded as the 0/0xFFFF child offsets, so
         * reaching one via a walk is "not found". */
        if (d->NameLength == XDVD_DIRENT_EMPTY_SENTINEL)
            return NULL;

        if (off + XDVD_DIRENT_FIXED_SIZE + d->NameLength > TableSize)
            return NULL;

        cmp = XdvdAsciiCompare(Name, NameLen, d->Name, d->NameLength);
        if (cmp == 0)
            return d;

        next = (cmp < 0) ? d->LeftOffset : d->RightOffset;
        if (next == 0 || next == 0xFFFF)
            return NULL;

        off = next * 4u;
    }
}

/*
 * Walk Path within the volume.  Path is a UNICODE_STRING using '\' separators.
 * On success fills FcbOut with the resolved dirent fields (or marks root).
 *
 * Caller-allocated Name buffer is filled with the resolved file's basename
 * (ASCII converted to UTF-16).  If Path is empty or "\", FcbOut->IsRoot=TRUE.
 */
static NTSTATUS
XdvdResolvePath(_In_ PXDVD_VCB Vcb,
                  _In_ PUNICODE_STRING Path,
                  _Out_ PXDVD_FCB Fcb,
                  _Out_writes_(256) PWCHAR NameBuf)
{
    ULONG sector = Vcb->RootSector;
    ULONG size = Vcb->RootSize;
    PVOID table = NULL;
    ULONG tableSize = 0;
    PWCHAR p, end;
    PXDVD_DIRENT d;
    NTSTATUS status;
    CHAR seg[256];
    ULONG segLen;
    BOOLEAN haveSegment = FALSE;

    RtlZeroMemory(Fcb, sizeof(*Fcb));
    Fcb->Vcb = Vcb;

    if (Path->Length == 0 ||
        (Path->Length == sizeof(WCHAR) && Path->Buffer[0] == L'\\'))
    {
        Fcb->IsRoot = TRUE;
        Fcb->IsDirectory = TRUE;
        Fcb->Attributes = XDVD_ATTR_DIRECTORY;
        Fcb->DataSector = Vcb->RootSector;
        Fcb->DataSize = Vcb->RootSize;
        Fcb->Name.Buffer = NameBuf;
        Fcb->Name.Length = 0;
        Fcb->Name.MaximumLength = 256 * sizeof(WCHAR);
        return STATUS_SUCCESS;
    }

    /* Split path on '\' and walk one segment at a time. */
    p = Path->Buffer;
    end = (PWCHAR)((PUCHAR)Path->Buffer + Path->Length);
    if (p < end && *p == L'\\') ++p;

    while (p < end)
    {
        PWCHAR segStart = p;
        ULONG i;

        while (p < end && *p != L'\\') ++p;
        segLen = (ULONG)(p - segStart);
        if (segLen == 0 || segLen > sizeof(seg))
        {
            if (table) ExFreePoolWithTag(table, XDVD_TAG);
            return STATUS_OBJECT_NAME_INVALID;
        }

        /* UTF-16 -> ASCII, lossy.  Xbox dirent names are ASCII; anything
         * outside that range can't exist on disc so a high byte = miss. */
        for (i = 0; i < segLen; ++i)
        {
            if (segStart[i] > 0x7F)
            {
                if (table) ExFreePoolWithTag(table, XDVD_TAG);
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            seg[i] = (CHAR)segStart[i];
        }

        if (table) { ExFreePoolWithTag(table, XDVD_TAG); table = NULL; }
        status = XdvdReadDirentTable(Vcb, sector, size, &table, &tableSize);
        if (!NT_SUCCESS(status))
            return status;

        d = XdvdFindInTable(table, tableSize, seg, segLen);
        if (d == NULL)
        {
            BOOLEAN intermediate = (p < end);
            ExFreePoolWithTag(table, XDVD_TAG);
            /* Win32 convention: missing intermediate directory yields
             * STATUS_OBJECT_PATH_NOT_FOUND; missing leaf yields
             * STATUS_OBJECT_NAME_NOT_FOUND.  ntoskrnl/mm/ARM3/sysldr.c's
             * boot-driver loader treats PATH_NOT_FOUND as a soft miss
             * but bugchecks on NAME_NOT_FOUND for required drivers, so
             * tagging "\reactos\..." lookups (whose intermediate dir is
             * absent on real game discs) as PATH_NOT_FOUND keeps PnP
             * from killing the boot. */
            return intermediate ? STATUS_OBJECT_PATH_NOT_FOUND
                                : STATUS_OBJECT_NAME_NOT_FOUND;
        }

        /* If more segments follow, this must be a directory. */
        if (p < end && *p == L'\\')
        {
            if (!(d->Attributes & XDVD_ATTR_DIRECTORY))
            {
                ExFreePoolWithTag(table, XDVD_TAG);
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }
            sector = d->DataSector;
            size = d->DataSize;
            ++p; /* skip '\' */
            continue;
        }

        /* Terminal segment.  Snapshot and we're done. */
        Fcb->DataSector = d->DataSector;
        Fcb->DataSize = d->DataSize;
        Fcb->Attributes = d->Attributes;
        Fcb->IsDirectory = (d->Attributes & XDVD_ATTR_DIRECTORY) != 0;
        Fcb->IsRoot = FALSE;

        /* Copy the resolved basename back as UTF-16. */
        for (i = 0; i < d->NameLength; ++i)
            NameBuf[i] = (WCHAR)(UCHAR)d->Name[i];
        Fcb->Name.Buffer = NameBuf;
        Fcb->Name.Length = (USHORT)(d->NameLength * sizeof(WCHAR));
        Fcb->Name.MaximumLength = 256 * sizeof(WCHAR);
        haveSegment = TRUE;
        break;
    }

    if (table) ExFreePoolWithTag(table, XDVD_TAG);
    return haveSegment ? STATUS_SUCCESS : STATUS_OBJECT_NAME_INVALID;
}

/* --- volume mount -------------------------------------------------------- */

/*
 * Mount handler.  Probe the device for the XDVDFS volume descriptor; if
 * present, create a volume device object, link it through the VPB, and
 * return success.  On any failure the I/O manager tries the next FSD in the
 * fs-recognizer queue.
 */
static NTSTATUS
XdvdMountVolume(_In_ PIO_STACK_LOCATION IrpSp)
{
    PDEVICE_OBJECT target = IrpSp->Parameters.MountVolume.DeviceObject;
    PVPB vpb = IrpSp->Parameters.MountVolume.Vpb;
    PVOID secBuf;
    PXDVD_VOLUME_DESCRIPTOR vd;
    PDEVICE_OBJECT volDev = NULL;
    PXDVD_VCB vcb;
    NTSTATUS status;
    ULONG got = 0;
    KIRQL irql;

    /* Only mount on optical devices.  Stops us from intercepting HDD reads. */
    if (target->DeviceType != FILE_DEVICE_CD_ROM)
        return STATUS_UNRECOGNIZED_VOLUME;

    secBuf = ExAllocatePoolWithTag(NonPagedPool, XDVD_SECTOR_SIZE, XDVD_TAG);
    if (secBuf == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    status = XdvdTargetRead(target,
                              (ULONGLONG)XDVD_VOLUME_DESCRIPTOR_LBA << XDVD_SECTOR_SHIFT,
                              XDVD_SECTOR_SIZE, secBuf, &got);
    if (!NT_SUCCESS(status) || got != XDVD_SECTOR_SIZE)
    {
        ExFreePoolWithTag(secBuf, XDVD_TAG);
        return NT_SUCCESS(status) ? STATUS_UNRECOGNIZED_VOLUME : status;
    }

    vd = (PXDVD_VOLUME_DESCRIPTOR)secBuf;
    if (RtlCompareMemory(vd->Magic0, XDVD_VOLUME_MAGIC, XDVD_VOLUME_MAGIC_LEN)
            != XDVD_VOLUME_MAGIC_LEN ||
        RtlCompareMemory(vd->Magic1, XDVD_VOLUME_MAGIC, XDVD_VOLUME_MAGIC_LEN)
            != XDVD_VOLUME_MAGIC_LEN)
    {
        ExFreePoolWithTag(secBuf, XDVD_TAG);
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    /* Unnamed device -- the volume is reached through the lower stack's VPB,
     * not through a global namespace name.  Type must match the underlying
     * media (CD-ROM here), matching cdfs's pattern. */
    status = IoCreateDevice(XdvdGlobals.DriverObject, sizeof(XDVD_VCB),
                            NULL, FILE_DEVICE_CD_ROM_FILE_SYSTEM,
                            0, FALSE, &volDev);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(secBuf, XDVD_TAG);
        return status;
    }

    volDev->StackSize = (UCHAR)(target->StackSize + 1);
    volDev->Flags |= DO_DIRECT_IO;
    volDev->SectorSize = XDVD_SECTOR_SIZE;
    /* Initial alignment must match the underlying device's. */
    if (target->AlignmentRequirement > volDev->AlignmentRequirement)
        volDev->AlignmentRequirement = target->AlignmentRequirement;
    volDev->Flags &= ~DO_DEVICE_INITIALIZING;

    vcb = (PXDVD_VCB)volDev->DeviceExtension;
    RtlZeroMemory(vcb, sizeof(*vcb));
    vcb->VolumeDevice = volDev;
    vcb->TargetDevice = target;
    vcb->Vpb = vpb;
    vcb->RootSector = vd->RootTableSector;
    vcb->RootSize = vd->RootTableSize;
    vcb->Mounted = TRUE;

    /* Link the volume device through the VPB.  IopMountInitializeVpb (the
     * IO manager's wrapper around the FSD's mount) sets VPB_MOUNTED itself
     * and fixes up the volume device's stack size + extension back-pointer
     * after we return; we only need to populate Vpb->DeviceObject so it
     * has a valid target to attach to.  Mirrors cdfs/fsctrl.c. */
    vpb->DeviceObject = volDev;
    volDev->Vpb = vpb;
    vpb->VolumeLabelLength = 0;
    /* No real serial in XDVDFS; derive from the filetime LSW. */
    vpb->SerialNumber = (ULONG)(vd->Filetime & 0xFFFFFFFFu);

    /* Take an extra Vpb reference so we are the one holding the last
     * dereference -- mirrors cdfs/fsctrl.c:974.  Without this, the
     * IopMountInitializeVpb-set refcount of 1 gets dropped to 0 by
     * IopParseDevice's failure-path IopDereferenceVpbAndFree (file.c:1096)
     * the first time a CREATE on this volume fails, which frees the Vpb
     * out from under \Device\CdRom0->Vpb.  The next ZwOpenFile then reads
     * a freed Vpb and crashes in IopParseDevice when it dereferences
     * Vpb->DeviceObject.  See volume.c:195 -- the free condition is
     * (refcount==0 && RealDevice->Vpb==self && !VPB_PERSISTENT). */
    vpb->ReferenceCount += 1;

    KeAcquireSpinLock(&XdvdGlobals.VcbListLock, &irql);
    InsertTailList(&XdvdGlobals.VcbList, &vcb->ListEntry);
    KeReleaseSpinLock(&XdvdGlobals.VcbListLock, irql);

    DbgPrint("mounted CdRom volume rootSec=%u rootSize=%u serial=%08lx\n",
             vcb->RootSector, vcb->RootSize, vpb->SerialNumber);

    ExFreePoolWithTag(secBuf, XDVD_TAG);
    return STATUS_SUCCESS;
}

/* --- IRP_MJ_CREATE ------------------------------------------------------- */

static NTSTATUS NTAPI
XdvdDispatchCreate(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = sp->FileObject;
    PXDVD_VCB vcb;
    XDVD_FCB scratch;
    PXDVD_FCB fcb;
    PWCHAR nameBuf;
    NTSTATUS status;
    ULONG nameBytes;

    /* CREATE on the control device opens the FSD itself -- always succeed. */
    if (DeviceObject == XdvdGlobals.ControlDevice)
    {
        fo->FsContext = NULL;
        fo->FsContext2 = NULL;
        Irp->IoStatus.Information = FILE_OPENED;
        return XdvdCompleteRequest(Irp, STATUS_SUCCESS, FILE_OPENED);
    }

    vcb = (PXDVD_VCB)DeviceObject->DeviceExtension;
    if (vcb == NULL || !vcb->Mounted)
        return XdvdCompleteRequest(Irp, STATUS_VOLUME_DISMOUNTED, 0);

    /* Read-only volume.  Refuse any create that asks to modify state. */
    if (sp->Parameters.Create.Options & FILE_DELETE_ON_CLOSE)
        return XdvdCompleteRequest(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);

    nameBuf = ExAllocatePoolWithTag(PagedPool, 256 * sizeof(WCHAR),
                                    XDVD_FCB_TAG);
    if (nameBuf == NULL)
        return XdvdCompleteRequest(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    status = XdvdResolvePath(vcb, &fo->FileName, &scratch, nameBuf);
    if (!NT_SUCCESS(status))
    {
        ExFreePoolWithTag(nameBuf, XDVD_FCB_TAG);
        return XdvdCompleteRequest(Irp, status, 0);
    }

    /* Directory/file mismatch checks against caller's expectations. */
    if ((sp->Parameters.Create.Options & FILE_NON_DIRECTORY_FILE) &&
        scratch.IsDirectory)
    {
        ExFreePoolWithTag(nameBuf, XDVD_FCB_TAG);
        return XdvdCompleteRequest(Irp, STATUS_FILE_IS_A_DIRECTORY, 0);
    }
    if ((sp->Parameters.Create.Options & FILE_DIRECTORY_FILE) &&
        !scratch.IsDirectory)
    {
        ExFreePoolWithTag(nameBuf, XDVD_FCB_TAG);
        return XdvdCompleteRequest(Irp, STATUS_NOT_A_DIRECTORY, 0);
    }

    nameBytes = sizeof(XDVD_FCB);
    fcb = ExAllocatePoolWithTag(NonPagedPool, nameBytes, XDVD_FCB_TAG);
    if (fcb == NULL)
    {
        ExFreePoolWithTag(nameBuf, XDVD_FCB_TAG);
        return XdvdCompleteRequest(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }

    *fcb = scratch;
    fcb->Name.Buffer = nameBuf;  /* hand-off ownership */

    fo->FsContext = fcb;
    fo->FsContext2 = NULL;
    fo->Vpb = vcb->Vpb;
    fo->SectionObjectPointer = NULL;

    InterlockedIncrement(&vcb->OpenCount);

    Irp->IoStatus.Information = FILE_OPENED;
    return XdvdCompleteRequest(Irp, STATUS_SUCCESS, FILE_OPENED);
}

/* --- IRP_MJ_CLEANUP / IRP_MJ_CLOSE -------------------------------------- */

static NTSTATUS NTAPI
XdvdDispatchCleanup(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return XdvdCompleteRequest(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI
XdvdDispatchClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = sp->FileObject;
    PXDVD_FCB fcb;
    PXDVD_CCB ccb;
    PXDVD_VCB vcb;

    if (DeviceObject == XdvdGlobals.ControlDevice)
        return XdvdCompleteRequest(Irp, STATUS_SUCCESS, 0);

    fcb = (PXDVD_FCB)fo->FsContext;
    ccb = (PXDVD_CCB)fo->FsContext2;

    if (ccb != NULL)
    {
        if (ccb->Entries != NULL)
            ExFreePoolWithTag(ccb->Entries, XDVD_ENUM_TAG);
        if (ccb->SearchPattern.Buffer != NULL)
            ExFreePoolWithTag(ccb->SearchPattern.Buffer, XDVD_CCB_TAG);
        ExFreePoolWithTag(ccb, XDVD_CCB_TAG);
        fo->FsContext2 = NULL;
    }

    if (fcb != NULL)
    {
        vcb = fcb->Vcb;
        if (fcb->Name.Buffer != NULL)
            ExFreePoolWithTag(fcb->Name.Buffer, XDVD_FCB_TAG);
        ExFreePoolWithTag(fcb, XDVD_FCB_TAG);
        fo->FsContext = NULL;
        if (vcb != NULL)
            InterlockedDecrement(&vcb->OpenCount);
    }

    return XdvdCompleteRequest(Irp, STATUS_SUCCESS, 0);
}

/* --- IRP_MJ_READ -------------------------------------------------------- */

/* Read [AbsStart, AbsStart+Length) of the disc into Dest.  The
 * sector-aligned bulk goes straight into Dest -- no allocation, titles
 * stream whole maps through here with RAM nearly exhausted -- and only
 * partial head/tail sectors bounce through one sector buffer. */
static NTSTATUS
XdvdReadIntoBuffer(_In_ PDEVICE_OBJECT Target,
                     _In_ ULONGLONG AbsStart,
                     _In_ ULONG Length,
                     _Out_writes_bytes_(Length) PUCHAR Dest,
                     _Out_ PULONG Done)
{
    PUCHAR secBuf = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG done = 0;

    while (done < Length)
    {
        ULONGLONG pos = AbsStart + done;
        ULONG posSlack = (ULONG)(pos & (XDVD_SECTOR_SIZE - 1));
        ULONG remain = Length - done;
        ULONG got = 0;

        if (posSlack == 0 && remain >= XDVD_SECTOR_SIZE)
        {
            ULONG bulk = remain & ~(XDVD_SECTOR_SIZE - 1);

            status = XdvdTargetRead(Target, pos, bulk, Dest + done, &got);
            if (!NT_SUCCESS(status))
                break;
            done += (got < bulk) ? got : bulk;
            if (got < bulk)
                break;
        }
        else
        {
            ULONG copy = XDVD_SECTOR_SIZE - posSlack;
            if (copy > remain)
                copy = remain;

            if (secBuf == NULL)
            {
                secBuf = ExAllocatePoolWithTag(NonPagedPool,
                                               XDVD_SECTOR_SIZE, XDVD_TAG);
                if (secBuf == NULL)
                {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                    break;
                }
            }
            status = XdvdTargetRead(Target, pos - posSlack,
                                      XDVD_SECTOR_SIZE, secBuf, &got);
            if (!NT_SUCCESS(status))
                break;
            if (got <= posSlack)
                break;
            if (got - posSlack < copy)
                copy = got - posSlack;
            RtlCopyMemory(Dest + done, secBuf + posSlack, copy);
            done += copy;
            if (posSlack + copy < XDVD_SECTOR_SIZE && done < Length)
                break;          /* device came up short */
        }
    }

    if (secBuf != NULL)
        ExFreePoolWithTag(secBuf, XDVD_TAG);
    *Done = done;
    return status;
}

static NTSTATUS NTAPI
XdvdDispatchRead(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = sp->FileObject;
    PXDVD_FCB fcb;
    ULONGLONG fileOff;
    ULONG userLen;
    ULONGLONG absStart;
    PVOID userVa;
    NTSTATUS status;
    ULONG got = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    fcb = (PXDVD_FCB)fo->FsContext;
    if (fcb == NULL || fcb->Vcb == NULL || fcb->IsDirectory)
        return XdvdCompleteRequest(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    fileOff = (ULONGLONG)sp->Parameters.Read.ByteOffset.QuadPart;
    userLen = sp->Parameters.Read.Length;

    if (userLen == 0)
        return XdvdCompleteRequest(Irp, STATUS_SUCCESS, 0);

    /* Past-EOF -> end-of-file; partial-tail -> truncate. */
    if (fileOff >= fcb->DataSize)
        return XdvdCompleteRequest(Irp, STATUS_END_OF_FILE, 0);
    if (fileOff + userLen > fcb->DataSize)
        userLen = (ULONG)(fcb->DataSize - fileOff);

    /* Disc-absolute byte position of the request (the underlying optical
     * class only does 2048-byte sector reads; XdvdReadIntoBuffer handles
     * the alignment). */
    absStart = ((ULONGLONG)fcb->DataSector << XDVD_SECTOR_SHIFT) + fileOff;

    /* Volume device set DO_DIRECT_IO: data lands in the caller's MDL. */
    userVa = MmGetSystemAddressForMdlSafe(Irp->MdlAddress,
                                          NormalPagePriority);
    if (userVa == NULL)
        return XdvdCompleteRequest(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    status = XdvdReadIntoBuffer(fcb->Vcb->TargetDevice, absStart, userLen,
                                  userVa, &got);
    if (!NT_SUCCESS(status) || got < userLen)
        return XdvdCompleteRequest(Irp,
            NT_SUCCESS(status) ? STATUS_END_OF_FILE : status, 0);

    /* Track stream position so subsequent reads without an explicit offset
     * see the right CurrentByteOffset (titles using Xapi do this). */
    fo->CurrentByteOffset.QuadPart = fileOff + userLen;

    return XdvdCompleteRequest(Irp, STATUS_SUCCESS, userLen);
}

static NTSTATUS NTAPI
XdvdDispatchWrite(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return XdvdCompleteRequest(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);
}

/* --- IRP_MJ_QUERY_INFORMATION ------------------------------------------- */

static NTSTATUS NTAPI
XdvdDispatchQueryInformation(_In_ PDEVICE_OBJECT DeviceObject,
                               _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = sp->FileObject;
    PXDVD_FCB fcb = (PXDVD_FCB)fo->FsContext;
    ULONG cls = sp->Parameters.QueryFile.FileInformationClass;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    ULONG bufLen = sp->Parameters.QueryFile.Length;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG ret = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (fcb == NULL)
        return XdvdCompleteRequest(Irp, STATUS_INVALID_PARAMETER, 0);

    RtlZeroMemory(buf, bufLen);

    switch (cls)
    {
    case FileBasicInformation:
    {
        PFILE_BASIC_INFORMATION fbi = buf;
        if (bufLen < sizeof(*fbi)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        fbi->FileAttributes = fcb->IsDirectory
            ? FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY
            : FILE_ATTRIBUTE_READONLY;
        ret = sizeof(*fbi);
        break;
    }
    case FileStandardInformation:
    {
        PFILE_STANDARD_INFORMATION fsi = buf;
        if (bufLen < sizeof(*fsi)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        fsi->AllocationSize.QuadPart =
            (fcb->DataSize + XDVD_SECTOR_SIZE - 1) &
            ~(LONGLONG)(XDVD_SECTOR_SIZE - 1);
        fsi->EndOfFile.QuadPart = fcb->DataSize;
        fsi->NumberOfLinks = 1;
        fsi->DeletePending = FALSE;
        fsi->Directory = (BOOLEAN)fcb->IsDirectory;
        ret = sizeof(*fsi);
        break;
    }
    case FilePositionInformation:
    {
        PFILE_POSITION_INFORMATION fpi = buf;
        if (bufLen < sizeof(*fpi)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        fpi->CurrentByteOffset = fo->CurrentByteOffset;
        ret = sizeof(*fpi);
        break;
    }
    case FileNetworkOpenInformation:
    {
        PFILE_NETWORK_OPEN_INFORMATION fno = buf;
        if (bufLen < sizeof(*fno)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        fno->AllocationSize.QuadPart =
            (fcb->DataSize + XDVD_SECTOR_SIZE - 1) &
            ~(LONGLONG)(XDVD_SECTOR_SIZE - 1);
        fno->EndOfFile.QuadPart = fcb->DataSize;
        fno->FileAttributes = fcb->IsDirectory
            ? FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY
            : FILE_ATTRIBUTE_READONLY;
        ret = sizeof(*fno);
        break;
    }
    case FileNameInformation:
    {
        PFILE_NAME_INFORMATION fni = buf;
        ULONG need = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) +
                     fcb->Name.Length;
        if (bufLen < FIELD_OFFSET(FILE_NAME_INFORMATION, FileName))
            { status = STATUS_BUFFER_TOO_SMALL; break; }
        fni->FileNameLength = fcb->Name.Length;
        if (bufLen < need)
        {
            RtlCopyMemory(fni->FileName, fcb->Name.Buffer,
                          bufLen - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName));
            ret = bufLen;
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        RtlCopyMemory(fni->FileName, fcb->Name.Buffer, fcb->Name.Length);
        ret = need;
        break;
    }
    default:
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }

    return XdvdCompleteRequest(Irp, status, ret);
}

static NTSTATUS NTAPI
XdvdDispatchSetInformation(_In_ PDEVICE_OBJECT DeviceObject,
                             _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = sp->FileObject;
    ULONG cls = sp->Parameters.SetFile.FileInformationClass;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* Allow position updates -- they don't mutate the volume. */
    if (cls == FilePositionInformation)
    {
        PFILE_POSITION_INFORMATION fpi = Irp->AssociatedIrp.SystemBuffer;
        if (sp->Parameters.SetFile.Length < sizeof(*fpi))
            return XdvdCompleteRequest(Irp, STATUS_INFO_LENGTH_MISMATCH, 0);
        fo->CurrentByteOffset = fpi->CurrentByteOffset;
        return XdvdCompleteRequest(Irp, STATUS_SUCCESS, 0);
    }

    return XdvdCompleteRequest(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);
}

/* --- IRP_MJ_QUERY_VOLUME_INFORMATION ------------------------------------ */

static NTSTATUS NTAPI
XdvdDispatchQueryVolumeInformation(_In_ PDEVICE_OBJECT DeviceObject,
                                     _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PXDVD_VCB vcb = (PXDVD_VCB)DeviceObject->DeviceExtension;
    ULONG cls = sp->Parameters.QueryVolume.FsInformationClass;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    ULONG bufLen = sp->Parameters.QueryVolume.Length;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG ret = 0;

    if (vcb == NULL || !vcb->Mounted)
        return XdvdCompleteRequest(Irp, STATUS_VOLUME_DISMOUNTED, 0);

    RtlZeroMemory(buf, bufLen);

    switch (cls)
    {
    case FileFsVolumeInformation:
    {
        PFILE_FS_VOLUME_INFORMATION info = buf;
        if (bufLen < sizeof(*info)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        info->VolumeSerialNumber = vcb->Vpb->SerialNumber;
        info->SupportsObjects = FALSE;
        info->VolumeLabelLength = 0;
        ret = sizeof(*info);
        break;
    }
    case FileFsAttributeInformation:
    {
        PFILE_FS_ATTRIBUTE_INFORMATION info = buf;
        static const WCHAR kFsName[] = L"XDVDFS";
        ULONG need = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION,
                                  FileSystemName) + sizeof(kFsName) - sizeof(WCHAR);
        if (bufLen < need) { status = STATUS_BUFFER_TOO_SMALL; break; }
        info->FileSystemAttributes = FILE_READ_ONLY_VOLUME |
                                     FILE_CASE_PRESERVED_NAMES;
        info->MaximumComponentNameLength = 255;
        info->FileSystemNameLength = sizeof(kFsName) - sizeof(WCHAR);
        RtlCopyMemory(info->FileSystemName, kFsName,
                      info->FileSystemNameLength);
        ret = need;
        break;
    }
    case FileFsSizeInformation:
    {
        PFILE_FS_SIZE_INFORMATION info = buf;
        if (bufLen < sizeof(*info)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        info->TotalAllocationUnits.QuadPart = 0; /* unknown without TOC walk */
        info->AvailableAllocationUnits.QuadPart = 0;
        info->SectorsPerAllocationUnit = 1;
        info->BytesPerSector = XDVD_SECTOR_SIZE;
        ret = sizeof(*info);
        break;
    }
    case FileFsDeviceInformation:
    {
        PFILE_FS_DEVICE_INFORMATION info = buf;
        if (bufLen < sizeof(*info)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        info->DeviceType = FILE_DEVICE_CD_ROM;
        info->Characteristics = FILE_REMOVABLE_MEDIA | FILE_READ_ONLY_DEVICE;
        ret = sizeof(*info);
        break;
    }
    default:
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }

    return XdvdCompleteRequest(Irp, status, ret);
}

/* --- IRP_MJ_DIRECTORY_CONTROL ------------------------------------------- */

/*
 * Iterative preorder walk of a dirent table, capturing all entries into the
 * caller's flat array.  `Out` must hold at least `Cap` entries; on overflow
 * the walk stops and returns STATUS_BUFFER_OVERFLOW (we size the array up
 * front from the table size as an upper bound, so this is defensive).
 */
static NTSTATUS
XdvdEnumerateTable(_In_reads_bytes_(TableSize) PUCHAR Table,
                     _In_ ULONG TableSize,
                     _Inout_updates_(Cap) PVOID Out_,
                     _In_ ULONG Cap,
                     _Out_ PULONG CountOut)
{
    typedef struct _ENTRY {
        ULONG  DataSector;
        ULONG  DataSize;
        UCHAR  Attributes;
        UCHAR  NameLength;
        CHAR   Name[256];
    } ENTRY;
    ENTRY *out = (ENTRY *)Out_;
    ULONG stack[512];
    ULONG sp = 0;
    ULONG count = 0;
    PXDVD_DIRENT d;
    ULONG off;

    stack[sp++] = 0;
    while (sp > 0)
    {
        off = stack[--sp];
        if (off + XDVD_DIRENT_FIXED_SIZE > TableSize)
            continue;

        d = (PXDVD_DIRENT)(Table + off);
        if (d->NameLength == XDVD_DIRENT_EMPTY_SENTINEL)
            continue;
        if (off + XDVD_DIRENT_FIXED_SIZE + d->NameLength > TableSize)
            continue;

        if (count >= Cap)
        {
            *CountOut = count;
            return STATUS_BUFFER_OVERFLOW;
        }

        out[count].DataSector = d->DataSector;
        out[count].DataSize = d->DataSize;
        out[count].Attributes = d->Attributes;
        out[count].NameLength = d->NameLength;
        RtlCopyMemory(out[count].Name, d->Name, d->NameLength);
        ++count;

        if (d->RightOffset != 0 && d->RightOffset != 0xFFFF && sp < RTL_NUMBER_OF(stack))
            stack[sp++] = d->RightOffset * 4u;
        if (d->LeftOffset != 0 && d->LeftOffset != 0xFFFF && sp < RTL_NUMBER_OF(stack))
            stack[sp++] = d->LeftOffset * 4u;
    }

    *CountOut = count;
    return STATUS_SUCCESS;
}

/*
 * Match an ASCII name against the caller's UNICODE search pattern.  We
 * support '*' and exact-name only -- that's what titles typically pass for
 * D:\.  Anything more complex degrades to "match all" rather than failing.
 */
static BOOLEAN
XdvdMatchName(_In_reads_(NameLen) PCSTR Name, _In_ ULONG NameLen,
                _In_opt_ PUNICODE_STRING Pattern)
{
    ULONG i;

    if (Pattern == NULL || Pattern->Length == 0)
        return TRUE;

    /* "*" -- accept everything. */
    if (Pattern->Length == sizeof(WCHAR) && Pattern->Buffer[0] == L'*')
        return TRUE;

    /* Exact ASCII compare against the unicode pattern. */
    if (Pattern->Length != NameLen * sizeof(WCHAR))
        return FALSE;
    for (i = 0; i < NameLen; ++i)
    {
        UCHAR a = (UCHAR)Name[i];
        WCHAR b = Pattern->Buffer[i];
        if (a >= 'a' && a <= 'z') a -= ('a' - 'A');
        if (b >= L'a' && b <= L'z') b -= (L'a' - L'A');
        if ((WCHAR)a != b) return FALSE;
    }
    return TRUE;
}

static NTSTATUS NTAPI
XdvdDispatchDirectoryControl(_In_ PDEVICE_OBJECT DeviceObject,
                               _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT fo = sp->FileObject;
    PXDVD_FCB fcb = (PXDVD_FCB)fo->FsContext;
    PXDVD_CCB ccb;
    PUCHAR outBuf;
    ULONG outLen;
    ULONG cls;
    ULONG flags;
    PUNICODE_STRING pattern;
    ULONG i, used = 0, lastOff = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (sp->MinorFunction != IRP_MN_QUERY_DIRECTORY)
        return XdvdCompleteRequest(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    if (fcb == NULL || !fcb->IsDirectory)
        return XdvdCompleteRequest(Irp, STATUS_INVALID_PARAMETER, 0);

    cls = sp->Parameters.QueryDirectory.FileInformationClass;
    flags = sp->Flags;
    pattern = sp->Parameters.QueryDirectory.FileName;
    outBuf = (PUCHAR)Irp->UserBuffer;
    outLen = sp->Parameters.QueryDirectory.Length;

    if (outBuf == NULL && Irp->MdlAddress != NULL)
        outBuf = MmGetSystemAddressForMdlSafe(Irp->MdlAddress,
                                              NormalPagePriority);
    if (outBuf == NULL || outLen == 0)
        return XdvdCompleteRequest(Irp, STATUS_INVALID_USER_BUFFER, 0);

    /* Lazily allocate the CCB on first directory query. */
    ccb = (PXDVD_CCB)fo->FsContext2;
    if (ccb == NULL)
    {
        ccb = ExAllocatePoolWithTag(PagedPool, sizeof(*ccb), XDVD_CCB_TAG);
        if (ccb == NULL)
            return XdvdCompleteRequest(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        RtlZeroMemory(ccb, sizeof(*ccb));
        fo->FsContext2 = ccb;
    }

    /* Restart resets the enumeration cursor; supplying a new pattern also
     * resets (titles call FindFirstFile to start over). */
    if ((flags & SL_RESTART_SCAN) ||
        (pattern != NULL && pattern->Length != 0 &&
         !RtlEqualUnicodeString(pattern, &ccb->SearchPattern, TRUE)))
    {
        ccb->NextIndex = 0;
        if (pattern != NULL && pattern->Length > 0)
        {
            if (ccb->SearchPattern.Buffer != NULL)
                ExFreePoolWithTag(ccb->SearchPattern.Buffer, XDVD_CCB_TAG);
            ccb->SearchPattern.Buffer = ExAllocatePoolWithTag(PagedPool,
                pattern->Length, XDVD_CCB_TAG);
            if (ccb->SearchPattern.Buffer == NULL)
                return XdvdCompleteRequest(Irp,
                    STATUS_INSUFFICIENT_RESOURCES, 0);
            RtlCopyMemory(ccb->SearchPattern.Buffer, pattern->Buffer,
                          pattern->Length);
            ccb->SearchPattern.Length = pattern->Length;
            ccb->SearchPattern.MaximumLength = pattern->Length;
        }
    }

    /* Load the directory once per handle. */
    if (!ccb->EnumLoaded)
    {
        PVOID table;
        ULONG tableSize;
        ULONG cap;

        status = XdvdReadDirentTable(fcb->Vcb, fcb->DataSector,
                                       fcb->DataSize, &table, &tableSize);
        if (!NT_SUCCESS(status))
            return XdvdCompleteRequest(Irp, status, 0);

        /* Worst case: each entry is the minimum-size dirent (16 bytes
         * after pad), so cap = table / 16 covers any layout. */
        cap = tableSize / 16u + 1u;
        ccb->Entries = ExAllocatePoolWithTag(PagedPool,
            cap * sizeof(*ccb->Entries), XDVD_ENUM_TAG);
        if (ccb->Entries == NULL)
        {
            ExFreePoolWithTag(table, XDVD_TAG);
            return XdvdCompleteRequest(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        }
        (VOID)XdvdEnumerateTable(table, tableSize, ccb->Entries, cap,
                                   &ccb->EntryCount);
        ExFreePoolWithTag(table, XDVD_TAG);
        ccb->EnumLoaded = TRUE;
    }

    /* Emit matching entries until we run out of room or hits. */
    for (i = ccb->NextIndex; i < ccb->EntryCount; ++i)
    {
        UCHAR  attrs = ccb->Entries[i].Attributes;
        ULONG  nameLen = ccb->Entries[i].NameLength;
        ULONG  nameBytes = nameLen * sizeof(WCHAR);
        ULONG  entrySize;
        ULONG  fixedSize;
        PUCHAR e;
        ULONG  ulAttr;
        ULONG  k;

        if (!XdvdMatchName(ccb->Entries[i].Name, nameLen,
                             pattern && pattern->Length ? pattern :
                             (ccb->SearchPattern.Length ? &ccb->SearchPattern : NULL)))
            continue;

        ulAttr = (attrs & XDVD_ATTR_DIRECTORY)
            ? (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)
            : FILE_ATTRIBUTE_READONLY;

        switch (cls)
        {
        case FileDirectoryInformation:
            fixedSize = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
            break;
        case FileFullDirectoryInformation:
            fixedSize = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
            break;
        case FileBothDirectoryInformation:
            fixedSize = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
            break;
        case FileNamesInformation:
            fixedSize = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
            break;
        default:
            return XdvdCompleteRequest(Irp, STATUS_INVALID_INFO_CLASS, 0);
        }

        entrySize = (fixedSize + nameBytes + 7) & ~7u;
        if (used + entrySize > outLen)
        {
            if (used == 0)
            {
                status = STATUS_BUFFER_OVERFLOW;
                used = 0;
            }
            break;
        }

        e = outBuf + used;
        RtlZeroMemory(e, entrySize);
        *(PULONG)(e + 0) = entrySize;                  /* NextEntryOffset */
        /* FileIndex unused; leave zero. */

        switch (cls)
        {
        case FileDirectoryInformation:
        {
            PFILE_DIRECTORY_INFORMATION fdi = (PFILE_DIRECTORY_INFORMATION)e;
            fdi->EndOfFile.QuadPart = ccb->Entries[i].DataSize;
            fdi->AllocationSize.QuadPart =
                (ccb->Entries[i].DataSize + XDVD_SECTOR_SIZE - 1) &
                ~(LONGLONG)(XDVD_SECTOR_SIZE - 1);
            fdi->FileAttributes = ulAttr;
            fdi->FileNameLength = nameBytes;
            for (k = 0; k < nameLen; ++k)
                fdi->FileName[k] = (WCHAR)(UCHAR)ccb->Entries[i].Name[k];
            break;
        }
        case FileFullDirectoryInformation:
        {
            PFILE_FULL_DIR_INFORMATION ffi = (PFILE_FULL_DIR_INFORMATION)e;
            ffi->EndOfFile.QuadPart = ccb->Entries[i].DataSize;
            ffi->AllocationSize.QuadPart =
                (ccb->Entries[i].DataSize + XDVD_SECTOR_SIZE - 1) &
                ~(LONGLONG)(XDVD_SECTOR_SIZE - 1);
            ffi->FileAttributes = ulAttr;
            ffi->FileNameLength = nameBytes;
            for (k = 0; k < nameLen; ++k)
                ffi->FileName[k] = (WCHAR)(UCHAR)ccb->Entries[i].Name[k];
            break;
        }
        case FileBothDirectoryInformation:
        {
            PFILE_BOTH_DIR_INFORMATION fbi = (PFILE_BOTH_DIR_INFORMATION)e;
            fbi->EndOfFile.QuadPart = ccb->Entries[i].DataSize;
            fbi->AllocationSize.QuadPart =
                (ccb->Entries[i].DataSize + XDVD_SECTOR_SIZE - 1) &
                ~(LONGLONG)(XDVD_SECTOR_SIZE - 1);
            fbi->FileAttributes = ulAttr;
            fbi->FileNameLength = nameBytes;
            fbi->ShortNameLength = 0;
            for (k = 0; k < nameLen; ++k)
                fbi->FileName[k] = (WCHAR)(UCHAR)ccb->Entries[i].Name[k];
            break;
        }
        case FileNamesInformation:
        {
            PFILE_NAMES_INFORMATION fni = (PFILE_NAMES_INFORMATION)e;
            fni->FileNameLength = nameBytes;
            for (k = 0; k < nameLen; ++k)
                fni->FileName[k] = (WCHAR)(UCHAR)ccb->Entries[i].Name[k];
            break;
        }
        }

        lastOff = used;
        used += entrySize;
        ccb->NextIndex = i + 1;

        if (flags & SL_RETURN_SINGLE_ENTRY)
            break;
    }

    /* Terminator: last record's NextEntryOffset = 0. */
    if (used > 0)
        *(PULONG)(outBuf + lastOff) = 0;
    else if (status == STATUS_SUCCESS)
        status = STATUS_NO_MORE_FILES;

    return XdvdCompleteRequest(Irp, status, used);
}

/* --- IRP_MJ_FILE_SYSTEM_CONTROL ----------------------------------------- */

static NTSTATUS NTAPI
XdvdDispatchFileSystemControl(_In_ PDEVICE_OBJECT DeviceObject,
                                _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (sp->MinorFunction == IRP_MN_MOUNT_VOLUME)
    {
        status = XdvdMountVolume(sp);
        return XdvdCompleteRequest(Irp, status, 0);
    }

    if (sp->MinorFunction == IRP_MN_VERIFY_VOLUME)
    {
        /* No media-change tracking yet; report the disc is still valid. */
        return XdvdCompleteRequest(Irp, STATUS_SUCCESS, 0);
    }

    return XdvdCompleteRequest(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS NTAPI
XdvdDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PXDVD_VCB vcb = (PXDVD_VCB)DeviceObject->DeviceExtension;

    /* Forward to the lower device so IOCTL_CDROM_xxx and IOCTL_STORAGE_xxx
     * (READ_TOC, CHECK_VERIFY, GET_MEDIA_TYPES, ...) work. */
    if (DeviceObject == XdvdGlobals.ControlDevice || vcb == NULL ||
        !vcb->Mounted)
        return XdvdCompleteRequest(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(vcb->TargetDevice, Irp);
}

/* --- DriverEntry --------------------------------------------------------- */

/* Called once from IopInitializeBootDrivers; media-change remount goes
 * through the FSCTL dispatch, not here. */
CODE_SEG("INIT")
NTSTATUS NTAPI
DriverEntry(_In_ PDRIVER_OBJECT DriverObject,
            _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING name;
    PDEVICE_OBJECT dev;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlZeroMemory(&XdvdGlobals, sizeof(XdvdGlobals));
    XdvdGlobals.DriverObject = DriverObject;
    KeInitializeSpinLock(&XdvdGlobals.VcbListLock);
    InitializeListHead(&XdvdGlobals.VcbList);

    DriverObject->MajorFunction[IRP_MJ_CREATE] = XdvdDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = XdvdDispatchCleanup;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = XdvdDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_READ] = XdvdDispatchRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = XdvdDispatchWrite;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION] =
        XdvdDispatchQueryInformation;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION] =
        XdvdDispatchSetInformation;
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] =
        XdvdDispatchQueryVolumeInformation;
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] =
        XdvdDispatchDirectoryControl;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] =
        XdvdDispatchFileSystemControl;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] =
        XdvdDispatchDeviceControl;

    /* Control device must be FILE_DEVICE_CD_ROM_FILE_SYSTEM so it lands in
     * IopCdRomFileSystemQueueHead -- the I/O manager searches that queue
     * for IRP_MN_MOUNT_VOLUME on \Device\CdRom* targets.  cdfs uses the
     * same type.  See ntoskrnl/io/iomgr/volume.c IoRegisterFileSystem. */
    RtlInitUnicodeString(&name, L"\\Xdvdfs");
    status = IoCreateDevice(DriverObject, 0, &name,
                            FILE_DEVICE_CD_ROM_FILE_SYSTEM,
                            0, FALSE, &dev);
    if (!NT_SUCCESS(status))
        return status;

    XdvdGlobals.ControlDevice = dev;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;

    IoRegisterFileSystem(dev);

    DbgPrint("registered (XDVDFS read-only)\n");
    return STATUS_SUCCESS;
}
