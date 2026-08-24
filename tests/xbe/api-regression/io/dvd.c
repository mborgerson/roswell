/*
 * Explicit DVD enumeration check. If the very suite is running, the OS
 * already mounted \Device\CdRom0 to find default.xbe -- so this test is
 * mostly a positive assertion for the PCI BusRelations / ATAPI bring-up
 * surface that gating mistakes have broken in the past.
 */

#include "../harness.h"
#include <string.h>

static const char DVD_ROOT[] = "\\Device\\CdRom0\\";
static const char DVD_SELF[] = "\\Device\\CdRom0\\default.xbe";

static NTSTATUS open_path(const char *path, ACCESS_MASK access, ULONG opts,
                          HANDLE *h, IO_STATUS_BLOCK *iosb)
{
    ANSI_STRING name;
    name.Length        = (USHORT)__builtin_strlen(path);
    name.MaximumLength = name.Length + 1;
    name.Buffer        = (PCHAR)path;

    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    return NtCreateFile(h, access | SYNCHRONIZE, &oa, iosb, NULL,
                        0, FILE_SHARE_READ, FILE_OPEN,
                        opts | FILE_SYNCHRONOUS_IO_NONALERT);
}

static bool t_open_root(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_path(DVD_ROOT, GENERIC_READ, FILE_DIRECTORY_FILE,
                           &h, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    return true;
}

static bool t_open_self(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_path(DVD_SELF, GENERIC_READ, FILE_NON_DIRECTORY_FILE,
                           &h, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    return true;
}

/* Enumerate the DVD root and expect to find default.xbe (the running
 * suite).  Exercises the xiso FSD's directory-control dispatch. */
static bool t_list_root(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_path(DVD_ROOT, GENERIC_READ, FILE_DIRECTORY_FILE,
                           &h, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    union {
        FILE_DIRECTORY_INFORMATION di;
        char raw[512];
    } buf;
    bool found = false;
    int entries = 0;
    BOOLEAN restart = TRUE;

    for (;;) {
        s = NtQueryDirectoryFile(h, NULL, NULL, NULL, &iosb, &buf,
                                 sizeof(buf), FileDirectoryInformation,
                                 NULL, restart);
        restart = FALSE;
        if (!NT_SUCCESS(s))
            break;
        entries++;
        /* Xbox NtQueryDirectoryFile returns single entries with ANSI
         * names; FileNameLength counts characters. */
        if (buf.di.FileNameLength == 11 &&
            memcmp(buf.di.FileName, "default.xbe", 11) == 0)
            found = true;
        if (entries > 256)
            break;
    }
    NtClose(h);

    if (entries == 0)
        FAIL_AND_RETURN("first NtQueryDirectoryFile -> 0x%08x", (unsigned)s);
    ASSERT_TRUE(found);
    return true;
}

/* NtQueryInformationFile against the xiso FSD. */
static bool t_query_self_info(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_path(DVD_SELF, GENERIC_READ, FILE_NON_DIRECTORY_FILE,
                           &h, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    FILE_NETWORK_OPEN_INFORMATION ni;
    s = NtQueryInformationFile(h, &iosb, &ni, sizeof(ni),
                               FileNetworkOpenInformation);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("network-open info -> 0x%08x", (unsigned)s);
    }
    if (ni.EndOfFile.QuadPart <= 0 ||
        (ni.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        NtClose(h);
        FAIL_AND_RETURN("bad EOF/attrs: eof=%d attrs=%08x",
                        (int)ni.EndOfFile.QuadPart,
                        (unsigned)ni.FileAttributes);
    }

    FILE_POSITION_INFORMATION pos;
    s = NtQueryInformationFile(h, &iosb, &pos, sizeof(pos),
                               FilePositionInformation);
    NtClose(h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_TRUE(pos.CurrentByteOffset.QuadPart == 0);
    return true;
}

/* The DVD is read-only: writes and delete dispositions must be
 * rejected, exercising the FSD's failure dispatch. */
static bool t_write_rejected(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_path(DVD_SELF, GENERIC_READ, FILE_NON_DIRECTORY_FILE,
                           &h, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    char data[16];
    memset(data, 0, sizeof(data));
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, data, sizeof(data), NULL);
    NtClose(h);
    ASSERT_TRUE(!NT_SUCCESS(s));
    return true;
}

static const test_entry_t io_dvd_entries[] = {
    {"open_root",       t_open_root},
    {"open_self",       t_open_self},
    {"list_root",       t_list_root},
    {"query_self_info", t_query_self_info},
    {"write_rejected",  t_write_rejected},
};

DEFINE_GROUP(io_dvd, "io/dvd");
