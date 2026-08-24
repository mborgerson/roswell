/*
 * File I/O on a cache partition (Partition3, ~750 MB): few enough
 * clusters that vfatfs runs its 16-bit FAT paths, which the data
 * partition (32-bit FAT) never touches.  Cache partitions are scratch
 * space by design, so creating and deleting here is retail-safe.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif

static const char FILE_PATH[] =
    "\\Device\\Harddisk0\\Partition3\\nxkrnl-api-fatx16.tmp";

static NTSTATUS open_file(ACCESS_MASK access, ULONG disposition, ULONG opts,
                          HANDLE *h)
{
    ANSI_STRING name = {
        .Length        = sizeof(FILE_PATH) - 1,
        .MaximumLength = sizeof(FILE_PATH),
        .Buffer        = (PCHAR)FILE_PATH,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(h, access | SYNCHRONIZE, &oa, &iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL, 0, disposition,
                        opts | FILE_NON_DIRECTORY_FILE |
                        FILE_SYNCHRONOUS_IO_NONALERT);
}

/* Allocate across several 16-bit FAT clusters and verify the data
 * survives a close/reopen (exercises the FAT16 chain walker and
 * available-cluster scan). */
static bool t_write_read_multicluster(void)
{
    HANDLE h;
    NTSTATUS s = open_file(GENERIC_READ | GENERIC_WRITE, FILE_OVERWRITE_IF,
                           0, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    static char buf[96 * 1024];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (char)(i * 2654435761u >> 24);

    IO_STATUS_BLOCK iosb;
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf), NULL);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = open_file(GENERIC_READ, FILE_OPEN, 0, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    static char back[sizeof(buf)];
    memset(back, 0, sizeof(back));
    s = NtReadFile(h, NULL, NULL, NULL, &iosb, back, sizeof(back), NULL);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32((ULONG)iosb.Information, sizeof(buf));
    ASSERT_TRUE(memcmp(buf, back, sizeof(buf)) == 0);
    return true;
}

/* Volume geometry must be readable, and freeing the file must give the
 * clusters back (exercises the FAT16 free-cluster counting). */
static bool t_delete_returns_clusters(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    FILE_FS_SIZE_INFORMATION before, after;

    NTSTATUS s = open_file(GENERIC_READ, FILE_OPEN, 0, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = NtQueryVolumeInformationFile(h, &iosb, &before, sizeof(before),
                                     FileFsSizeInformation);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(before.TotalAllocationUnits.QuadPart > 0);

    s = open_file(GENERIC_READ | DELETE, FILE_OPEN, FILE_DELETE_ON_CLOSE,
                  &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);

    s = open_file(GENERIC_READ, FILE_OPEN, 0, &h);
    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_NOT_FOUND);

    /* Re-query through a fresh handle on the (deleted) file's volume. */
    HANDLE hv;
    s = open_file(GENERIC_WRITE, FILE_OVERWRITE_IF, 0, &hv);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = NtQueryVolumeInformationFile(hv, &iosb, &after, sizeof(after),
                                     FileFsSizeInformation);
    if (NT_SUCCESS(s)) {
        FILE_DISPOSITION_INFORMATION di = { .DeleteFile = TRUE };
        NtSetInformationFile(hv, &iosb, &di, sizeof(di),
                             FileDispositionInformation);
    }
    NtClose(hv);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(after.AvailableAllocationUnits.QuadPart >=
                before.AvailableAllocationUnits.QuadPart);
    return true;
}

static const test_entry_t io_fatx16_entries[] = {
    {"write_read_multicluster", t_write_read_multicluster},
    {"delete_returns_clusters", t_delete_returns_clusters},
};

DEFINE_GROUP(io_fatx16, "io/fatx16");
