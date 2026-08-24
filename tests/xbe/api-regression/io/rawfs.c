/*
 * Raw filesystem: a DASD open of Partition0 (whole disk, no FATX
 * superblock at LBA 0) must fall through the FSD mount chain to the raw
 * filesystem, which then serves sector-aligned reads and volume-size
 * queries directly against the disk.
 */

#include "../harness.h"
#include <string.h>

static const char RAW_VOLUME[] = "\\Device\\Harddisk0\\Partition0";

static NTSTATUS open_volume(HANDLE *h)
{
    ANSI_STRING name = {
        .Length        = sizeof(RAW_VOLUME) - 1,
        .MaximumLength = sizeof(RAW_VOLUME),
        .Buffer        = (PCHAR)RAW_VOLUME,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    IO_STATUS_BLOCK iosb;
    return NtOpenFile(h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb,
                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                      FILE_SYNCHRONOUS_IO_NONALERT);
}

static bool t_dasd_open_mounts_raw(void)
{
    HANDLE h;
    NTSTATUS s = open_volume(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    return true;
}

static bool t_sector_read(void)
{
    HANDLE h;
    NTSTATUS s = open_volume(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    static __attribute__((aligned(512))) char sector[512];
    memset(sector, 0xCC, sizeof(sector));

    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off = { .QuadPart = 0 };
    s = NtReadFile(h, NULL, NULL, NULL, &iosb, sector, sizeof(sector),
                   &off);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32((ULONG)iosb.Information, sizeof(sector));

    /* The read must have actually replaced the fill pattern (a truly
     * 0xCC-filled config sector is not a thing FATX images produce). */
    int changed = 0;
    for (size_t i = 0; i < sizeof(sector); i++)
        if (sector[i] != (char)0xCC)
            changed = 1;
    ASSERT_TRUE(changed);
    return true;
}

static bool t_fs_size_query(void)
{
    HANDLE h;
    NTSTATUS s = open_volume(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    IO_STATUS_BLOCK iosb;
    FILE_FS_SIZE_INFORMATION sz;
    s = NtQueryVolumeInformationFile(h, &iosb, &sz, sizeof(sz),
                                     FileFsSizeInformation);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(sz.BytesPerSector, 512);
    ASSERT_TRUE(sz.TotalAllocationUnits.QuadPart > 0);
    return true;
}


static const test_entry_t io_rawfs_entries[] = {
    {"dasd_open_mounts_raw", t_dasd_open_mounts_raw},
    {"sector_read",          t_sector_read},
    {"fs_size_query",        t_fs_size_query},
};

DEFINE_GROUP(io_rawfs, "io/rawfs");
