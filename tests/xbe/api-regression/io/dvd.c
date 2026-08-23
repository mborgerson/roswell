/*
 * Explicit DVD enumeration check. If the very suite is running, the OS
 * already mounted \Device\CdRom0 to find default.xbe -- so this test is
 * mostly a positive assertion for the PCI BusRelations / ATAPI bring-up
 * surface that gating mistakes have broken in the past.
 */

#include "../harness.h"

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

static const test_entry_t io_dvd_entries[] = {
    {"open_root", t_open_root},
    {"open_self", t_open_self},
};

DEFINE_GROUP(io_dvd, "io/dvd");
