/*
 * NtDeleteFile: open-and-unlink in one call, driven straight off an
 * object-attributes block with no handle in between.
 *
 * Same scratch path as io/file -- \Device\Harddisk0\Partition1\ directly,
 * because DOS-name resolution needs a mount step that has not necessarily
 * run when the test XBE boots off the DVD.
 *
 * nxdk declares NtDeleteFile as returning BOOLEAN, which would read back
 * only AL; the ordinal really returns an NTSTATUS, so the calls go through
 * a correctly-typed pointer.
 */

#include "../harness.h"
#include <string.h>

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef STATUS_NO_SUCH_FILE
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#endif
#ifndef STATUS_SHARING_VIOLATION
#define STATUS_SHARING_VIOLATION ((NTSTATUS)0xC0000043L)
#endif

typedef NTSTATUS (NTAPI *delete_file_fn)(POBJECT_ATTRIBUTES);
static const delete_file_fn p_NtDeleteFile =
    (delete_file_fn)(void *)NtDeleteFile;

static const char SCRATCH_PATH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-apireg-del.tmp";
static const char MISSING_PATH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-apireg-none.tmp";

static void init_oa(OBJECT_ATTRIBUTES *oa, ANSI_STRING *name,
                    const char *path, USHORT len)
{
    name->Length        = len;
    name->MaximumLength = (USHORT)(len + 1);
    name->Buffer        = (PCHAR)path;
    oa->RootDirectory   = NULL;
    oa->ObjectName      = name;
    oa->Attributes      = OBJ_CASE_INSENSITIVE;
}

static NTSTATUS open_scratch(HANDLE *h, ACCESS_MASK access, ULONG disposition,
                             ULONG share)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    init_oa(&oa, &name, SCRATCH_PATH, sizeof(SCRATCH_PATH) - 1);
    return NtCreateFile(h, access | SYNCHRONIZE, &oa, &iosb, NULL,
                        FILE_ATTRIBUTE_NORMAL, share, disposition,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

static NTSTATUS delete_scratch(void)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;

    init_oa(&oa, &name, SCRATCH_PATH, sizeof(SCRATCH_PATH) - 1);
    return p_NtDeleteFile(&oa);
}

/* Both "gone" spellings the FSD may pick; io/file accepts the same pair. */
static bool is_not_found(NTSTATUS s)
{
    return s == STATUS_OBJECT_NAME_NOT_FOUND || s == STATUS_NO_SUCH_FILE;
}

static bool t_delete_existing(void)
{
    HANDLE h;
    NTSTATUS s = open_scratch(&h, GENERIC_WRITE, FILE_OVERWRITE_IF, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);

    s = delete_scratch();
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* The name must be gone, not merely marked. */
    s = open_scratch(&h, GENERIC_READ, FILE_OPEN, 0);
    if (!is_not_found(s)) {
        if (NT_SUCCESS(s)) NtClose(h);
        FAIL_AND_RETURN("file survived NtDeleteFile: 0x%08x", (unsigned)s);
    }
    return true;
}

static bool t_delete_missing(void)
{
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;

    init_oa(&oa, &name, MISSING_PATH, sizeof(MISSING_PATH) - 1);
    NTSTATUS s = p_NtDeleteFile(&oa);
    if (!is_not_found(s))
        FAIL_AND_RETURN("delete of a missing file -> 0x%08x", (unsigned)s);
    return true;
}

static bool t_delete_twice(void)
{
    HANDLE h;
    NTSTATUS s = open_scratch(&h, GENERIC_WRITE, FILE_OVERWRITE_IF, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);

    s = delete_scratch();
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = delete_scratch();
    if (!is_not_found(s))
        FAIL_AND_RETURN("second delete -> 0x%08x", (unsigned)s);
    return true;
}

static bool t_delete_while_open(void)
{
    HANDLE h;
    NTSTATUS s = open_scratch(&h, GENERIC_WRITE, FILE_OVERWRITE_IF, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* The open handle grants no share access, so the delete's own open
     * cannot be satisfied. */
    NTSTATUS del = delete_scratch();
    NtClose(h);

    if (del != STATUS_SHARING_VIOLATION) {
        /* Leave nothing behind for the next case either way. */
        delete_scratch();
        FAIL_AND_RETURN("delete of an exclusively open file -> 0x%08x",
                        (unsigned)del);
    }

    /* Closing the holder releases it; the file must then delete cleanly. */
    s = delete_scratch();
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    return true;
}

static const test_entry_t io_delete_entries[] = {
    {"delete_existing",   t_delete_existing},
    {"delete_missing",    t_delete_missing},
    {"delete_twice",      t_delete_twice},
    {"delete_while_open", t_delete_while_open},
};

DEFINE_GROUP(io_delete, "io/delete");
