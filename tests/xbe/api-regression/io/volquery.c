/*
 * IoQueryVolumeInformation: the volume query, on a file object.
 *
 * Like its file-level sibling the console answers a fixed set of
 * classes -- four here -- and refuses the rest with
 * STATUS_INVALID_PARAMETER.  The set is the same on the disc and on the
 * hard disk, so it is the kernel's and not any one file system's.
 * Unlike that sibling, the caller's length does reach the file system:
 * the attribute class truncates against it.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_BUFFER_OVERFLOW
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#endif
#ifndef FILE_DEVICE_DISK
#define FILE_DEVICE_DISK 0x00000007
#endif
#ifndef FILE_REMOVABLE_MEDIA
#define FILE_REMOVABLE_MEDIA 0x00000001
#endif

/* The four the console answers. */
#define CLASS_VOLUME    1
#define CLASS_SIZE      3
#define CLASS_DEVICE    4
#define CLASS_ATTRIBUTE 5

/* Head of FILE_FS_VOLUME_INFORMATION: the label follows it. */
#define VOLUME_HEAD_BYTES 17
/* Head of FILE_FS_ATTRIBUTE_INFORMATION: the name follows it. */
#define ATTRIBUTE_HEAD_BYTES 12

static const char SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-volquery.tmp";

typedef struct {
    LARGE_INTEGER VolumeCreationTime;
    ULONG         VolumeSerialNumber;
    ULONG         VolumeLabelLength;
    BOOLEAN       SupportsObjects;
    WCHAR         VolumeLabel[1];
} volume_t;

typedef struct {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER AvailableAllocationUnits;
    ULONG         SectorsPerAllocationUnit;
    ULONG         BytesPerSector;
} size_t_;

typedef struct {
    ULONG DeviceType;
    ULONG Characteristics;
} device_t;

typedef struct {
    ULONG FileSystemAttributes;
    LONG  MaximumComponentNameLength;
    ULONG FileSystemNameLength;
    WCHAR FileSystemName[1];
} attribute_t;

static ANSI_STRING str(const char *s)
{
    ANSI_STRING a;

    a.Buffer = (PCHAR)s;
    a.Length = (USHORT)strlen(s);
    a.MaximumLength = (USHORT)(a.Length + 1);
    return a;
}

/* A scratch file on the data partition, plus the file object behind it.
 * The handle holds the reference the caller needs, so the pointer
 * reference taken here is released straight away. */
static NTSTATUS open_scratch(HANDLE *h, PVOID *fo)
{
    ANSI_STRING name = str(SCRATCH);
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;

    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;

    *h = NULL;
    *fo = NULL;
    s = NtCreateFile(h, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa,
                     &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     FILE_OVERWRITE_IF,
                     FILE_SYNCHRONOUS_IO_NONALERT |
                         FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(s))
        return s;

    s = ObReferenceObjectByHandle(*h, NULL, fo);
    if (!NT_SUCCESS(s)) {
        NtClose(*h);
        *h = NULL;
        return s;
    }
    ObfDereferenceObject(*fo);
    return STATUS_SUCCESS;
}

static void close_scratch(HANDLE h)
{
    ANSI_STRING name = str(SCRATCH);
    OBJECT_ATTRIBUTES oa;

    if (h != NULL) NtClose(h);
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    NtDeleteFile(&oa);
}

static bool t_only_four_classes_are_answered(void)
{
    static char line[16];
    UCHAR buffer[256];
    HANDLE h;
    PVOID fo;
    NTSTATUS s;
    bool ok = true;
    int cls;

    s = open_scratch(&h, &fo);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    for (cls = 1; cls <= 8; cls++) {
        bool expected = (cls == CLASS_VOLUME || cls == CLASS_SIZE ||
                         cls == CLASS_DEVICE || cls == CLASS_ATTRIBUTE);
        ULONG returned = 0;

        memset(buffer, 0, sizeof(buffer));
        s = IoQueryVolumeInformation(fo, (FS_INFORMATION_CLASS)cls,
                                     sizeof(buffer), buffer, &returned);
        line[cls - 1] = (s == STATUS_SUCCESS) ? '.'
                      : (s == STATUS_INVALID_PARAMETER) ? 'p' : '?';
        if ((s == STATUS_SUCCESS) != expected)
            ok = false;
    }
    line[8] = '\0';
    close_scratch(h);

    if (!ok) FAIL_AND_RETURN("classes 1-8: %s", line);
    return true;
}

/* The label rides behind the fixed head, and the reported length says
 * how much of it there is. */
static bool t_volume_information(void)
{
    UCHAR buffer[256];
    volume_t *vol = (volume_t *)buffer;
    HANDLE h;
    PVOID fo;
    ULONG returned = 0;
    NTSTATUS s;

    s = open_scratch(&h, &fo);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    memset(buffer, 0, sizeof(buffer));
    s = IoQueryVolumeInformation(fo, (FS_INFORMATION_CLASS)CLASS_VOLUME,
                                 sizeof(buffer), buffer, &returned);
    close_scratch(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(returned, VOLUME_HEAD_BYTES + vol->VolumeLabelLength);
    ASSERT_EQ_U32(vol->SupportsObjects, FALSE);
    return true;
}

/* Cluster geometry, and free space that fits inside the volume. */
static bool t_size_information(void)
{
    size_t_ info;
    HANDLE h;
    PVOID fo;
    ULONG returned = 0;
    NTSTATUS s;

    s = open_scratch(&h, &fo);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    memset(&info, 0xCC, sizeof(info));
    s = IoQueryVolumeInformation(fo, (FS_INFORMATION_CLASS)CLASS_SIZE,
                                 sizeof(info), &info, &returned);
    close_scratch(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(returned, sizeof(info));
    ASSERT_EQ_U32(info.BytesPerSector, 512);
    ASSERT_TRUE(info.SectorsPerAllocationUnit != 0);
    ASSERT_TRUE(info.TotalAllocationUnits.QuadPart > 0);
    ASSERT_TRUE(info.AvailableAllocationUnits.QuadPart <=
                info.TotalAllocationUnits.QuadPart);
    return true;
}

/* The volume behind a file on the data partition is a disk. */
static bool t_device_information(void)
{
    device_t info;
    HANDLE h;
    PVOID fo;
    ULONG returned = 0;
    NTSTATUS s;

    s = open_scratch(&h, &fo);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    memset(&info, 0xCC, sizeof(info));
    s = IoQueryVolumeInformation(fo, (FS_INFORMATION_CLASS)CLASS_DEVICE,
                                 sizeof(info), &info, &returned);
    close_scratch(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(returned, sizeof(info));
    ASSERT_EQ_U32(info.DeviceType, FILE_DEVICE_DISK);
    ASSERT_EQ_U32(info.Characteristics & FILE_REMOVABLE_MEDIA, 0);
    return true;
}

/* The caller's length reaches the file system here: a buffer with no
 * room for the name is an overflow, and only the head is written. */
static bool t_attribute_information_truncates(void)
{
    UCHAR buffer[256];
    attribute_t *attr = (attribute_t *)buffer;
    HANDLE h;
    PVOID fo;
    ULONG full = 0, head = 0;
    NTSTATUS s, t;

    s = open_scratch(&h, &fo);
    if (!NT_SUCCESS(s)) FAIL_AND_RETURN("open -> 0x%08x", (unsigned)s);

    memset(buffer, 0, sizeof(buffer));
    s = IoQueryVolumeInformation(fo, (FS_INFORMATION_CLASS)CLASS_ATTRIBUTE,
                                 sizeof(buffer), buffer, &full);
    if (!NT_SUCCESS(s)) {
        close_scratch(h);
        FAIL_AND_RETURN("query -> 0x%08x", (unsigned)s);
    }
    if (full != ATTRIBUTE_HEAD_BYTES + attr->FileSystemNameLength) {
        close_scratch(h);
        FAIL_AND_RETURN("returned %u, name %u", (unsigned)full,
                        (unsigned)attr->FileSystemNameLength);
    }

    memset(buffer, 0, sizeof(buffer));
    t = IoQueryVolumeInformation(fo, (FS_INFORMATION_CLASS)CLASS_ATTRIBUTE,
                                 ATTRIBUTE_HEAD_BYTES, buffer, &head);
    close_scratch(h);

    ASSERT_TRUE(attr->MaximumComponentNameLength > 0);
    ASSERT_NTSTATUS(t, STATUS_BUFFER_OVERFLOW);
    ASSERT_EQ_U32(head, ATTRIBUTE_HEAD_BYTES);
    return true;
}

static const test_entry_t io_volquery_entries[] = {
    { "only_four_classes_are_answered", t_only_four_classes_are_answered,
      NULL },
    { "volume_information", t_volume_information, NULL },
    { "size_information", t_size_information, NULL },
    { "device_information", t_device_information, NULL },
    { "attribute_information_truncates",
      t_attribute_information_truncates, NULL },
};

DEFINE_GROUP(io_volquery, "io/volquery");
