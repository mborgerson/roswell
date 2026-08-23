/*
 * NtCreateFile + NtWriteFile + NtReadFile + NtClose.
 *
 * We use \Device\Harddisk0\Partition1\ directly rather than e:\. DOS-name
 * resolution depends on the dashboard or an earlier mount step that does
 * not necessarily run when the test XBE boots straight off the DVD.
 * Partition1 is the standard E: data partition.
 */

#include "../harness.h"
#include <string.h>

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef STATUS_NO_SUCH_FILE
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#endif
#ifndef STATUS_DELETE_PENDING
#define STATUS_DELETE_PENDING ((NTSTATUS)0xC0000056L)
#endif

static const char SCRATCH_PATH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-regression.tmp";

static NTSTATUS open_scratch(HANDLE *h, ACCESS_MASK access, ULONG disposition,
                             ULONG opts, IO_STATUS_BLOCK *iosb)
{
    ANSI_STRING name = {
        .Length        = (USHORT)(sizeof(SCRATCH_PATH) - 1),
        .MaximumLength = sizeof(SCRATCH_PATH),
        .Buffer        = (PCHAR)SCRATCH_PATH,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    return NtCreateFile(h, access | SYNCHRONIZE, &oa, iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL, 0,
                        disposition, opts | FILE_SYNCHRONOUS_IO_NONALERT
                                          | FILE_NON_DIRECTORY_FILE);
}

static void unlink_scratch(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_scratch(&h, GENERIC_READ, FILE_OPEN,
                              FILE_DELETE_ON_CLOSE, &iosb);
    if (NT_SUCCESS(s)) NtClose(h);
}

static bool t_open_missing(void)
{
    /* Make sure the file is not there. */
    unlink_scratch();
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_scratch(&h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    if (s != STATUS_OBJECT_NAME_NOT_FOUND && s != STATUS_NO_SUCH_FILE) {
        if (NT_SUCCESS(s)) NtClose(h);
        FAIL_AND_RETURN("open missing returned 0x%08x", (unsigned)s);
    }
    return true;
}

static bool t_create_then_open(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_scratch(&h, GENERIC_WRITE, FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);

    s = open_scratch(&h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);

    unlink_scratch();
    return true;
}

static bool t_write_read_4kb(void)
{
    static unsigned char buf_w[4096];
    static unsigned char buf_r[4096];
    for (int i = 0; i < 4096; i++) buf_w[i] = (unsigned char)(i ^ 0x5A);

    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_scratch(&h, GENERIC_WRITE, FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = NtWriteFile(h, NULL, NULL, NULL, &iosb, buf_w, sizeof(buf_w), NULL);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, sizeof(buf_w));
    NtClose(h);

    s = open_scratch(&h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    LARGE_INTEGER zero = { .QuadPart = 0 };
    s = NtReadFile(h, NULL, NULL, NULL, &iosb, buf_r, sizeof(buf_r), &zero);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, sizeof(buf_w));
    NtClose(h);

    for (int i = 0; i < 4096; i++) {
        if (buf_r[i] != buf_w[i]) {
            unlink_scratch();
            FAIL_AND_RETURN("byte %d: wrote 0x%02x, read 0x%02x",
                            i, buf_w[i], buf_r[i]);
        }
    }
    unlink_scratch();
    return true;
}

static bool t_delete_on_close(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_scratch(&h, GENERIC_WRITE, FILE_OVERWRITE_IF, 0, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);

    s = open_scratch(&h, GENERIC_READ, FILE_OPEN, FILE_DELETE_ON_CLOSE, &iosb);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);

    /* Subsequent open should fail because the file is now gone. */
    s = open_scratch(&h, GENERIC_READ, FILE_OPEN, 0, &iosb);
    if (s != STATUS_OBJECT_NAME_NOT_FOUND && s != STATUS_NO_SUCH_FILE) {
        if (NT_SUCCESS(s)) NtClose(h);
        FAIL_AND_RETURN("file survived delete-on-close: 0x%08x", (unsigned)s);
    }
    return true;
}

static const test_entry_t io_file_entries[] = {
    {"open_missing",     t_open_missing},
    {"create_then_open", t_create_then_open},
    {"write_read_4kb",   t_write_read_4kb},
    {"delete_on_close",  t_delete_on_close},
};

DEFINE_GROUP(io_file, "io/file");
