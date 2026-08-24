/*
 * File-information set/query paths on the FATX data partition:
 * position, basic (timestamps/attributes), allocation, rename,
 * delete-on-disposition, and volume information queries.
 */

#include "../harness.h"
#include <string.h>

#ifndef STATUS_OBJECT_NAME_NOT_FOUND
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#endif

static const char DIR_PATH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-finfo";

static NTSTATUS open_at(const char *path, ACCESS_MASK access,
                        ULONG disposition, ULONG opts, HANDLE *h)
{
    ANSI_STRING name = {
        .Length        = (USHORT)strlen(path),
        .MaximumLength = (USHORT)(strlen(path) + 1),
        .Buffer        = (PCHAR)path,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(h, access | SYNCHRONIZE, &oa, &iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL, 0, disposition,
                        opts | FILE_SYNCHRONOUS_IO_NONALERT);
}

static bool ensure_dir(void)
{
    HANDLE h;
    NTSTATUS s = open_at(DIR_PATH, GENERIC_READ, FILE_OPEN_IF,
                         FILE_DIRECTORY_FILE, &h);
    if (!NT_SUCCESS(s))
        return false;
    NtClose(h);
    return true;
}

static void leaf_path(char *out, size_t cap, const char *leaf)
{
    strcpy(out, DIR_PATH);
    strcat(out, "\\");
    strcat(out, leaf);
    (void)cap;
}

static NTSTATUS make_file(const char *leaf, HANDLE *h)
{
    char path[160];
    leaf_path(path, sizeof(path), leaf);
    return open_at(path, GENERIC_READ | GENERIC_WRITE | DELETE,
                   FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, h);
}

static bool t_position_info_roundtrip(void)
{
    ASSERT_TRUE(ensure_dir());

    HANDLE h;
    NTSTATUS s = make_file("pos.bin", &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    IO_STATUS_BLOCK iosb;
    char data[100];
    memset(data, 0xAB, sizeof(data));
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, data, sizeof(data),
                    NULL);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("NtWriteFile -> 0x%08x", (unsigned)s);
    }

    FILE_POSITION_INFORMATION pos;
    s = NtQueryInformationFile(h, &iosb, &pos, sizeof(pos),
                               FilePositionInformation);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("query position -> 0x%08x", (unsigned)s);
    }
    if (pos.CurrentByteOffset.QuadPart != (LONGLONG)sizeof(data)) {
        NtClose(h);
        FAIL_AND_RETURN("position after write: %d",
                        (int)pos.CurrentByteOffset.QuadPart);
    }

    pos.CurrentByteOffset.QuadPart = 10;
    s = NtSetInformationFile(h, &iosb, &pos, sizeof(pos),
                             FilePositionInformation);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("set position -> 0x%08x", (unsigned)s);
    }

    char back[4];
    s = NtReadFile(h, NULL, NULL, NULL, &iosb, back, sizeof(back), NULL);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(back[0] == (char)0xAB);
    return true;
}

/* The XAPI shape: timestamps are read via FileNetworkOpenInformation
 * (retail FATX rejects a FileBasicInformation *query* with
 * STATUS_INVALID_PARAMETER) and written via FileBasicInformation. */
static bool t_basic_info_query_set(void)
{
    ASSERT_TRUE(ensure_dir());

    HANDLE h;
    NTSTATUS s = make_file("basic.bin", &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    IO_STATUS_BLOCK iosb;
    FILE_NETWORK_OPEN_INFORMATION ni;
    s = NtQueryInformationFile(h, &iosb, &ni, sizeof(ni),
                               FileNetworkOpenInformation);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("query network-open -> 0x%08x", (unsigned)s);
    }
    if ((ni.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        NtClose(h);
        FAIL_AND_RETURN("file has DIRECTORY attribute");
    }

    /* Set a recognizable write time (2004-01-01 00:00:00 UTC) and read
     * it back.  Zero timestamps mean "leave unchanged". */
    FILE_BASIC_INFORMATION set;
    memset(&set, 0, sizeof(set));
    set.LastWriteTime.QuadPart = 126578592000000000LL;
    s = NtSetInformationFile(h, &iosb, &set, sizeof(set),
                             FileBasicInformation);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("set basic -> 0x%08x", (unsigned)s);
    }

    s = NtQueryInformationFile(h, &iosb, &ni, sizeof(ni),
                               FileNetworkOpenInformation);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    /* FATX stores 2-second-granularity local-epoch stamps; accept any
     * value within a day of what we set rather than bit equality. */
    LONGLONG delta = ni.LastWriteTime.QuadPart - 126578592000000000LL;
    if (delta < 0)
        delta = -delta;
    if (delta > (LONGLONG)24 * 3600 * 10000000)
        FAIL_AND_RETURN("write time not applied: %08x%08x",
                        (unsigned)(ni.LastWriteTime.QuadPart >> 32),
                        (unsigned)ni.LastWriteTime.QuadPart);
    return true;
}

static bool t_allocation_info_extend(void)
{
    ASSERT_TRUE(ensure_dir());

    HANDLE h;
    NTSTATUS s = make_file("alloc.bin", &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    IO_STATUS_BLOCK iosb;
    FILE_ALLOCATION_INFORMATION ai;
    ai.AllocationSize.QuadPart = 64 * 1024;
    s = NtSetInformationFile(h, &iosb, &ai, sizeof(ai),
                             FileAllocationInformation);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("set allocation -> 0x%08x", (unsigned)s);
    }

    /* EOF must not have moved.  Retail reports AllocationSize from the
     * on-disk FileSize, so the reserved space is not asserted. */
    FILE_NETWORK_OPEN_INFORMATION ni;
    s = NtQueryInformationFile(h, &iosb, &ni, sizeof(ni),
                               FileNetworkOpenInformation);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(ni.EndOfFile.QuadPart == 0);
    return true;
}

static bool t_rename_same_directory(void)
{
    ASSERT_TRUE(ensure_dir());

    HANDLE h;
    NTSTATUS s = make_file("ren-old.bin", &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    char newpath[160];
    leaf_path(newpath, sizeof(newpath), "ren-new.bin");

    struct {
        FILE_RENAME_INFORMATION ri;
        char pad[160];
    } r;
    memset(&r, 0, sizeof(r));
    r.ri.ReplaceIfExists = TRUE;
    r.ri.RootDirectory = NULL;
    r.ri.FileName.Buffer = newpath;
    r.ri.FileName.Length = (USHORT)strlen(newpath);
    r.ri.FileName.MaximumLength = r.ri.FileName.Length + 1;

    IO_STATUS_BLOCK iosb;
    s = NtSetInformationFile(h, &iosb, &r.ri, sizeof(r),
                             FileRenameInformation);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Old name gone, new name opens. */
    char oldpath[160];
    leaf_path(oldpath, sizeof(oldpath), "ren-old.bin");
    s = open_at(oldpath, GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE,
                &h);
    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_NOT_FOUND);

    s = open_at(newpath, GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE,
                &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    return true;
}

static bool t_disposition_deletes(void)
{
    ASSERT_TRUE(ensure_dir());

    HANDLE h;
    NTSTATUS s = make_file("doomed.bin", &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    FILE_DISPOSITION_INFORMATION di = { .DeleteFile = TRUE };
    IO_STATUS_BLOCK iosb;
    s = NtSetInformationFile(h, &iosb, &di, sizeof(di),
                             FileDispositionInformation);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    char path[160];
    leaf_path(path, sizeof(path), "doomed.bin");
    s = open_at(path, GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &h);
    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_NOT_FOUND);
    return true;
}

static bool t_volume_info_queries(void)
{
    ASSERT_TRUE(ensure_dir());

    HANDLE h;
    NTSTATUS s = open_at(DIR_PATH, GENERIC_READ, FILE_OPEN,
                         FILE_DIRECTORY_FILE, &h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    IO_STATUS_BLOCK iosb;
    FILE_FS_SIZE_INFORMATION sz;
    s = NtQueryVolumeInformationFile(h, &iosb, &sz, sizeof(sz),
                                     FileFsSizeInformation);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("FsSize -> 0x%08x", (unsigned)s);
    }
    if (sz.BytesPerSector == 0 || sz.SectorsPerAllocationUnit == 0 ||
        sz.TotalAllocationUnits.QuadPart <= 0) {
        NtClose(h);
        FAIL_AND_RETURN("FsSize fields: bps=%u spau=%u",
                        (unsigned)sz.BytesPerSector,
                        (unsigned)sz.SectorsPerAllocationUnit);
    }

    struct {
        FILE_FS_ATTRIBUTE_INFORMATION ai;
        char pad[32];
    } attr;
    s = NtQueryVolumeInformationFile(h, &iosb, &attr.ai, sizeof(attr),
                                     FileFsAttributeInformation);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(attr.ai.FileSystemNameLength > 0);
    return true;
}


#ifndef STATUS_DIRECTORY_NOT_EMPTY
#define STATUS_DIRECTORY_NOT_EMPTY ((NTSTATUS)0xC0000101L)
#endif

/* Delete-disposition on a non-empty directory must be refused, and
 * succeed once the directory is emptied. */
static bool t_delete_nonempty_dir_rejected(void)
{
    ASSERT_TRUE(ensure_dir());

    char subdir[160];
    leaf_path(subdir, sizeof(subdir), "subdir");
    HANDLE hd;
    NTSTATUS s = open_at(subdir, GENERIC_READ, FILE_OPEN_IF,
                         FILE_DIRECTORY_FILE, &hd);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(hd);

    char inner[160];
    leaf_path(inner, sizeof(inner), "subdir\\inner.bin");
    HANDLE hf;
    s = open_at(inner, GENERIC_WRITE, FILE_OVERWRITE_IF,
                FILE_NON_DIRECTORY_FILE, &hf);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(hf);

    FILE_DISPOSITION_INFORMATION di = { .DeleteFile = TRUE };
    IO_STATUS_BLOCK iosb;
    s = open_at(subdir, GENERIC_READ | DELETE, FILE_OPEN,
                FILE_DIRECTORY_FILE, &hd);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = NtSetInformationFile(hd, &iosb, &di, sizeof(di),
                             FileDispositionInformation);
    NtClose(hd);
    ASSERT_NTSTATUS(s, STATUS_DIRECTORY_NOT_EMPTY);

    /* Empty it, then the delete must go through. */
    s = open_at(inner, GENERIC_READ | DELETE, FILE_OPEN,
                FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, &hf);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(hf);

    s = open_at(subdir, GENERIC_READ | DELETE, FILE_OPEN,
                FILE_DIRECTORY_FILE, &hd);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = NtSetInformationFile(hd, &iosb, &di, sizeof(di),
                             FileDispositionInformation);
    NtClose(hd);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static const test_entry_t io_finfo_entries[] = {
    {"position_info_roundtrip", t_position_info_roundtrip},
    {"basic_info_query_set",    t_basic_info_query_set},
    {"allocation_info_extend",  t_allocation_info_extend},
    {"rename_same_directory",   t_rename_same_directory},
    {"disposition_deletes",     t_disposition_deletes},
    {"volume_info_queries",     t_volume_info_queries},
    {"delete_nonempty_dir_rejected", t_delete_nonempty_dir_rejected},
};

DEFINE_GROUP(io_finfo, "io/finfo");
