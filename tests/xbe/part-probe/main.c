/*
 * part-probe: enumerate \Device\Harddisk0\PartitionN and report the FATX
 * VolumeId of each.  Designed to run against both nxkrnl and the retail
 * Xbox kernel so we can confirm the canonical PartitionN <-> drive-letter
 * (E/C/X/Y/Z) mapping by comparing the volume serials each kernel reports
 * for the same physical disk.
 *
 * On a stock retail Xbox HDD the volume serials and FATX VolumeIds are
 * baked in at format time -- the values in the FATX superblock at the
 * start of each partition's physical offset.  Whichever kernel sees the
 * same disk should report identical VolumeIds for any given drive letter;
 * the *ordering* of those VolumeIds across Partition1..5 tells us how the
 * kernel maps device names.
 *
 * Output (one line per partition, port-0xE9 DbgPrint):
 *     partprobe: \Device\Harddisk0\Partition1 serial=XXXXXXXX entries=N first=<name>
 *
 * Title-side ANSI-string handling -- Xbox NtCreateFile takes an
 * OBJECT_STRING (ANSI) rather than NT's UNICODE_STRING, see
 * third_party/nxdk/lib/xboxkrnl/xboxdef.h.
 */

#include <xboxkrnl/xboxkrnl.h>
#include <hal/debug.h>
#include <string.h>

/* FILE_FS_VOLUME_INFORMATION returned by NtQueryVolumeInformationFile with
 * info class FileFsVolumeInformation -- VolumeCreationTime + Serial + ... */
typedef struct _NX_FS_VOLUME_INFO {
    LARGE_INTEGER VolumeCreationTime;
    ULONG         VolumeSerialNumber;
    ULONG         VolumeLabelLength;
    UCHAR         SupportsObjects;
    WCHAR         VolumeLabel[16];
} NX_FS_VOLUME_INFO;

#define FileFsVolumeInformation 1
#define FileDirectoryInformation 1

typedef struct _NX_FILE_DIRECTORY_INFO {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    CHAR  FileName[1];
} NX_FILE_DIRECTORY_INFO;

static const char *parts[] = {
    "\\Device\\Harddisk0\\Partition1\\",
    "\\Device\\Harddisk0\\Partition2\\",
    "\\Device\\Harddisk0\\Partition3\\",
    "\\Device\\Harddisk0\\Partition4\\",
    "\\Device\\Harddisk0\\Partition5\\",
    "\\Device\\Harddisk0\\Partition6\\",
    "\\Device\\Harddisk0\\Partition7\\",
};

static void probe_one(const char *path)
{
    OBJECT_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS status;
    NX_FS_VOLUME_INFO vi;
    UCHAR dbuf[512];

    name.Length = strlen(path);
    name.MaximumLength = name.Length + 1;
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;

    status = NtCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                          0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN,
                          FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(status)) {
        DbgPrint("partprobe: %s open failed (%08lx)\n", path, status);
        return;
    }

    /* Volume serial */
    status = NtQueryVolumeInformationFile(h, &iosb, &vi, sizeof(vi),
                                          FileFsVolumeInformation);
    ULONG serial = NT_SUCCESS(status) ? vi.VolumeSerialNumber : 0;

    /* First root entry */
    char first_name[64] = "(none)";
    ULONG first_size = 0;
    status = NtQueryDirectoryFile(h, NULL, NULL, NULL, &iosb, dbuf,
                                  sizeof(dbuf), FileDirectoryInformation,
                                  &name /* unused ptr */, TRUE);
    if (NT_SUCCESS(status)) {
        NX_FILE_DIRECTORY_INFO *di = (NX_FILE_DIRECTORY_INFO *)dbuf;
        ULONG n = di->FileNameLength;
        if (n > sizeof(first_name) - 1) n = sizeof(first_name) - 1;
        for (ULONG i = 0; i < n; i++) first_name[i] = di->FileName[i];
        first_name[n] = '\0';
        first_size = di->EndOfFile.LowPart;
    }

    DbgPrint("partprobe: %s serial=%08lx first=%s size=%lu\n",
             path, serial, first_name, first_size);
    NtClose(h);
}

int main(void)
{
    DbgPrint("partprobe: enumerating \\Device\\Harddisk0\\Partition[1-7]\n");
    for (int i = 0; i < (int)(sizeof(parts) / sizeof(parts[0])); i++)
        probe_one(parts[i]);
    DbgPrint("partprobe: done\n");
    return 0;
}
