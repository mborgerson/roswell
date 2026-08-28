/*
 * IoCreateFile -- the open path NtCreateFile is a thin caller of.
 *
 * The Xbox form drops NT's EA and create-file-type parameters but keeps
 * the trailing Options word, and that word is the whole difference
 * between the two exports: NtCreateFile passes zero, so a title reaching
 * IoCreateFile directly is the only caller that can ask for the
 * parameter validation.  These cases pin the disposition reported back
 * through the I/O status block, both sides of the Options gate, and that
 * a refused call leaves the caller's handle and status block alone.
 */

#include "../harness.h"
#include <string.h>

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#endif
#ifndef STATUS_NO_SUCH_FILE
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000FL)
#endif

/* The bit that turns the validation on. */
#define IO_CHECK_CREATE_PARAMETERS 0x0200

static const char SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-createfile.tmp";
static const char LEAF[] = "nxkrnl-api-createfile.tmp";
static const char ROOT[] = "\\Device\\Harddisk0\\Partition1\\";

typedef struct {
    ACCESS_MASK access;
    ULONG       share;
    ULONG       disposition;
    ULONG       create_options;
    ULONG       options;
    HANDLE      root;
    const char *path;
} create_args_t;

static NTSTATUS io_create(HANDLE *h, IO_STATUS_BLOCK *iosb,
                          const create_args_t *a)
{
    ANSI_STRING name = {
        .Length        = (USHORT)strlen(a->path),
        .MaximumLength = (USHORT)(strlen(a->path) + 1),
        .Buffer        = (PCHAR)a->path,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = a->root,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    return IoCreateFile(h, a->access, &oa, iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                        a->share, a->disposition, a->create_options,
                        a->options);
}

/* A plain synchronous open of the scratch file, validation off. */
static NTSTATUS open_scratch(HANDLE *h, IO_STATUS_BLOCK *iosb,
                             ACCESS_MASK access, ULONG disposition,
                             ULONG create_options)
{
    create_args_t a = {
        .access         = access | SYNCHRONIZE,
        .share          = 0,
        .disposition    = disposition,
        .create_options = create_options | FILE_NON_DIRECTORY_FILE
                                         | FILE_SYNCHRONOUS_IO_NONALERT,
        .options        = 0,
        .root           = NULL,
        .path           = SCRATCH,
    };
    return io_create(h, iosb, &a);
}

static void unlink_scratch(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = open_scratch(&h, &iosb, DELETE, FILE_OPEN,
                              FILE_DELETE_ON_CLOSE);
    if (NT_SUCCESS(s)) NtClose(h);
}

static bool t_create_reports_created(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;

    unlink_scratch();
    memset(&iosb, 0xCD, sizeof(iosb));
    NTSTATUS s = open_scratch(&h, &iosb, GENERIC_WRITE, FILE_CREATE, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    ASSERT_EQ_U32(iosb.Information, FILE_CREATED);
    ASSERT_EQ_U32(iosb.Status, STATUS_SUCCESS);
    unlink_scratch();
    return true;
}

static bool t_open_reports_opened(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;

    unlink_scratch();
    ASSERT_NTSTATUS(open_scratch(&h, &iosb, GENERIC_WRITE, FILE_CREATE, 0),
                    STATUS_SUCCESS);
    NtClose(h);

    memset(&iosb, 0xCD, sizeof(iosb));
    NTSTATUS s = open_scratch(&h, &iosb, GENERIC_READ, FILE_OPEN, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    ASSERT_EQ_U32(iosb.Information, FILE_OPENED);
    unlink_scratch();
    return true;
}

static bool t_overwrite_if_reports_overwritten(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;

    unlink_scratch();
    ASSERT_NTSTATUS(open_scratch(&h, &iosb, GENERIC_WRITE, FILE_CREATE, 0),
                    STATUS_SUCCESS);
    NtClose(h);

    memset(&iosb, 0xCD, sizeof(iosb));
    NTSTATUS s = open_scratch(&h, &iosb, GENERIC_WRITE, FILE_OVERWRITE_IF, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    ASSERT_EQ_U32(iosb.Information, FILE_OVERWRITTEN);

    /* On a path with nothing there the same disposition creates. */
    unlink_scratch();
    memset(&iosb, 0xCD, sizeof(iosb));
    s = open_scratch(&h, &iosb, GENERIC_WRITE, FILE_OVERWRITE_IF, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    ASSERT_EQ_U32(iosb.Information, FILE_CREATED);
    unlink_scratch();
    return true;
}

/* The disposition reaches the file system unchanged (below), but the
 * console answers it as an overwrite: a supersede of an existing file
 * reports FILE_OVERWRITTEN, never FILE_SUPERSEDED. */
static bool t_supersede_reports_overwritten(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;

    unlink_scratch();
    ASSERT_NTSTATUS(open_scratch(&h, &iosb, GENERIC_WRITE, FILE_CREATE, 0),
                    STATUS_SUCCESS);
    NtClose(h);

    memset(&iosb, 0xCD, sizeof(iosb));
    NTSTATUS s = open_scratch(&h, &iosb, GENERIC_WRITE | DELETE,
                              FILE_SUPERSEDE, 0);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    ASSERT_EQ_U32(iosb.Information, FILE_OVERWRITTEN);
    unlink_scratch();
    return true;
}

static bool t_create_on_existing_collides(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;

    unlink_scratch();
    ASSERT_NTSTATUS(open_scratch(&h, &iosb, GENERIC_WRITE, FILE_CREATE, 0),
                    STATUS_SUCCESS);
    NtClose(h);

    memset(&iosb, 0xCD, sizeof(iosb));
    h = (HANDLE)(ULONG_PTR)0xCDCDCDCD;
    NTSTATUS s = open_scratch(&h, &iosb, GENERIC_WRITE, FILE_CREATE, 0);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        unlink_scratch();
        FAIL_AND_RETURN("second FILE_CREATE succeeded");
    }
    ASSERT_NTSTATUS(s, STATUS_OBJECT_NAME_COLLISION);
    /* An error status is reported only through the return value: the
     * status block belongs to the completed request there never was. */
    ASSERT_EQ_U32(iosb.Status, 0xCDCDCDCD);
    ASSERT_EQ_U32(iosb.Information, 0xCDCDCDCD);
    ASSERT_EQ_PTR(h, (HANDLE)(ULONG_PTR)0xCDCDCDCD);
    unlink_scratch();
    return true;
}

static bool t_checked_bad_disposition_is_refused(void)
{
    HANDLE h = (HANDLE)(ULONG_PTR)0xCDCDCDCD;
    IO_STATUS_BLOCK iosb;
    create_args_t a = {
        .access         = GENERIC_WRITE | SYNCHRONIZE,
        .disposition    = FILE_MAXIMUM_DISPOSITION + 1,
        .create_options = FILE_NON_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT,
        .options        = IO_CHECK_CREATE_PARAMETERS,
        .path           = SCRATCH,
    };

    unlink_scratch();
    NTSTATUS s = io_create(&h, &iosb, &a);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        unlink_scratch();
        FAIL_AND_RETURN("out-of-range disposition was accepted");
    }
    ASSERT_NTSTATUS(s, STATUS_INVALID_PARAMETER);
    ASSERT_EQ_PTR(h, (HANDLE)(ULONG_PTR)0xCDCDCDCD);
    return true;
}

static bool t_checked_bad_share_access_is_refused(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    create_args_t a = {
        .access         = GENERIC_WRITE | SYNCHRONIZE,
        /* One bit past FILE_SHARE_READ|WRITE|DELETE. */
        .share          = 0x00000008,
        .disposition    = FILE_OPEN_IF,
        .create_options = FILE_NON_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT,
        .options        = IO_CHECK_CREATE_PARAMETERS,
        .path           = SCRATCH,
    };

    unlink_scratch();
    NTSTATUS s = io_create(&h, &iosb, &a);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        unlink_scratch();
        FAIL_AND_RETURN("invalid share-access bit was accepted");
    }
    ASSERT_NTSTATUS(s, STATUS_INVALID_PARAMETER);
    return true;
}

static bool t_checked_delete_on_close_needs_delete(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    create_args_t a = {
        .access         = GENERIC_WRITE | SYNCHRONIZE,
        .disposition    = FILE_OPEN_IF,
        .create_options = FILE_NON_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT |
                          FILE_DELETE_ON_CLOSE,
        .options        = IO_CHECK_CREATE_PARAMETERS,
        .path           = SCRATCH,
    };

    unlink_scratch();
    NTSTATUS s = io_create(&h, &iosb, &a);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        unlink_scratch();
        FAIL_AND_RETURN("delete-on-close without DELETE was accepted");
    }
    ASSERT_NTSTATUS(s, STATUS_INVALID_PARAMETER);
    return true;
}

static bool t_checked_synchronous_needs_synchronize(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    create_args_t a = {
        /* No SYNCHRONIZE, but a synchronous open. */
        .access         = GENERIC_WRITE,
        .disposition    = FILE_OPEN_IF,
        .create_options = FILE_NON_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT,
        .options        = IO_CHECK_CREATE_PARAMETERS,
        .path           = SCRATCH,
    };

    unlink_scratch();
    NTSTATUS s = io_create(&h, &iosb, &a);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        unlink_scratch();
        FAIL_AND_RETURN("synchronous open without SYNCHRONIZE was accepted");
    }
    ASSERT_NTSTATUS(s, STATUS_INVALID_PARAMETER);
    return true;
}

/* The other side of the gate: the same call the checked cases refuse
 * goes through when Options is zero, which is what NtCreateFile passes. */
static bool t_unchecked_options_skip_the_validation(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    create_args_t a = {
        .access         = GENERIC_WRITE | SYNCHRONIZE,
        .disposition    = FILE_OPEN_IF,
        .create_options = FILE_NON_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT |
                          FILE_DELETE_ON_CLOSE,
        .options        = 0,
        .path           = SCRATCH,
    };

    unlink_scratch();
    NTSTATUS s = io_create(&h, &iosb, &a);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);

    /* It really was delete-on-close: the file is gone. */
    s = open_scratch(&h, &iosb, GENERIC_READ, FILE_OPEN, 0);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        unlink_scratch();
        FAIL_AND_RETURN("delete-on-close left the file behind");
    }
    if (s != STATUS_OBJECT_NAME_NOT_FOUND && s != STATUS_NO_SUCH_FILE)
        FAIL_AND_RETURN("reopen after delete returned 0x%08x", (unsigned)s);
    return true;
}

static bool t_directory_file_opens_the_volume_root(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    create_args_t a = {
        .access         = GENERIC_READ | SYNCHRONIZE,
        .disposition    = FILE_OPEN,
        .create_options = FILE_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT,
        .options        = 0,
        .path           = ROOT,
    };

    NTSTATUS s = io_create(&h, &iosb, &a);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    ASSERT_EQ_U32(iosb.Information, FILE_OPENED);
    return true;
}

static bool t_relative_name_under_root_directory(void)
{
    HANDLE dir, h;
    IO_STATUS_BLOCK iosb;
    create_args_t dir_args = {
        .access         = GENERIC_READ | SYNCHRONIZE,
        .disposition    = FILE_OPEN,
        .create_options = FILE_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT,
        .path           = ROOT,
    };

    unlink_scratch();
    ASSERT_NTSTATUS(open_scratch(&h, &iosb, GENERIC_WRITE, FILE_CREATE, 0),
                    STATUS_SUCCESS);
    NtClose(h);

    ASSERT_NTSTATUS(io_create(&dir, &iosb, &dir_args), STATUS_SUCCESS);

    create_args_t rel = {
        .access         = GENERIC_READ | SYNCHRONIZE,
        .disposition    = FILE_OPEN,
        .create_options = FILE_NON_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT,
        .root           = dir,
        .path           = LEAF,
    };
    memset(&iosb, 0xCD, sizeof(iosb));
    NTSTATUS s = io_create(&h, &iosb, &rel);
    NtClose(dir);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    NtClose(h);
    ASSERT_EQ_U32(iosb.Information, FILE_OPENED);
    unlink_scratch();
    return true;
}

/* A refusal on the validation path never reaches a file system, so it
 * has no status block to report -- the caller's stays as it was. */
static bool t_refusal_leaves_the_status_block_alone(void)
{
    HANDLE h = (HANDLE)(ULONG_PTR)0xCDCDCDCD;
    IO_STATUS_BLOCK iosb;
    create_args_t a = {
        .access         = GENERIC_WRITE | SYNCHRONIZE,
        .disposition    = FILE_MAXIMUM_DISPOSITION + 1,
        .create_options = FILE_NON_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT,
        .options        = IO_CHECK_CREATE_PARAMETERS,
        .path           = SCRATCH,
    };

    memset(&iosb, 0xCD, sizeof(iosb));
    NTSTATUS s = io_create(&h, &iosb, &a);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        unlink_scratch();
        FAIL_AND_RETURN("refused call succeeded");
    }
    ASSERT_EQ_U32(iosb.Status, 0xCDCDCDCD);
    ASSERT_EQ_U32(iosb.Information, 0xCDCDCDCD);
    return true;
}


/* Where the disposition ends up.  A device the title registers itself
 * sees the create IRP the kernel built, so the disposition the file
 * system is asked for can be read directly rather than inferred from
 * what comes back.  The Xbox stack location keeps the create parameters
 * where NT does, so the disposition is the top byte of the second
 * parameter word. */

#define MJ_SLOTS 0x0E
#define FILE_DEVICE_UNKNOWN 0x00000022u
#define SL_RAW_DWORDS 6

static const char DEVICE_NAME[] = "\\Device\\nxkrnlcreatefile";

static DRIVER_OBJECT g_driver;
static PDEVICE_OBJECT g_device;
static ULONG g_create_raw[SL_RAW_DWORDS];
static ULONG g_creates;

static NTSTATUS NTAPI probe_dispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    const ULONG *sp = (const ULONG *)Irp->Tail.Overlay.CurrentStackLocation;

    (void)DeviceObject;
    if (sp != NULL && (UCHAR)(sp[0] & 0xFF) == 0 /* IRP_MJ_CREATE */) {
        memcpy(g_create_raw, sp, sizeof(g_create_raw));
        g_creates++;
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = FILE_OPENED;
    IofCompleteRequest(Irp, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS probe_setup(void)
{
    OBJECT_STRING name = {
        .Length        = (USHORT)(sizeof(DEVICE_NAME) - 1),
        .MaximumLength = sizeof(DEVICE_NAME),
        .Buffer        = (PCHAR)DEVICE_NAME,
    };
    NTSTATUS s;
    int i;

    if (g_device != NULL)
        return STATUS_SUCCESS;
    for (i = 0; i < MJ_SLOTS; i++)
        g_driver.MajorFunction[i] = probe_dispatch;
    s = IoCreateDevice(&g_driver, 0, &name, FILE_DEVICE_UNKNOWN, FALSE,
                       &g_device);
    if (!NT_SUCCESS(s))
        return s;
    /* Publish it: a fresh device is stamped initializing. */
    g_device->Flags |= 0x00000004u;
    g_device->Flags &= ~0x00000010u;
    return STATUS_SUCCESS;
}

static bool t_the_disposition_reaches_the_driver(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = probe_setup();
    create_args_t a = {
        .access         = GENERIC_READ | SYNCHRONIZE,
        .disposition    = FILE_SUPERSEDE,
        .create_options = FILE_NON_DIRECTORY_FILE |
                          FILE_SYNCHRONOUS_IO_NONALERT,
        .path           = DEVICE_NAME,
    };

    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("IoCreateDevice: 0x%08x", (unsigned)s);

    g_creates = 0;
    memset(g_create_raw, 0, sizeof(g_create_raw));
    s = io_create(&h, &iosb, &a);
    if (!NT_SUCCESS(s))
        FAIL_AND_RETURN("open of the probe device: 0x%08x", (unsigned)s);
    NtClose(h);

    ASSERT_EQ_U32(g_creates, 1);
    /* Parameters+0x04 carries (Disposition << 24) | CreateOptions. */
    if ((g_create_raw[2] >> 24) != FILE_SUPERSEDE)
        FAIL_AND_RETURN("create parameters: +0x00=0x%08x +0x04=0x%08x "
                        "+0x08=0x%08x +0x0c=0x%08x +0x10=0x%08x",
                        (unsigned)g_create_raw[1], (unsigned)g_create_raw[2],
                        (unsigned)g_create_raw[3], (unsigned)g_create_raw[4],
                        (unsigned)g_create_raw[5]);
    /* The create options ride in the same word, disposition aside. */
    ASSERT_EQ_U32(g_create_raw[2] & 0x00FFFFFF,
                  a.create_options & 0x00FFFFFF);
    return true;
}

static const test_entry_t io_createfile_entries[] = {
    { "create_reports_created", t_create_reports_created, NULL },
    { "open_reports_opened", t_open_reports_opened, NULL },
    { "overwrite_if_reports_overwritten",
      t_overwrite_if_reports_overwritten, NULL },
    { "supersede_reports_overwritten", t_supersede_reports_overwritten,
      NULL },
    { "create_on_existing_collides", t_create_on_existing_collides, NULL },
    { "checked_bad_disposition_is_refused",
      t_checked_bad_disposition_is_refused, NULL },
    { "checked_bad_share_access_is_refused",
      t_checked_bad_share_access_is_refused, NULL },
    { "checked_delete_on_close_needs_delete",
      t_checked_delete_on_close_needs_delete, NULL },
    { "checked_synchronous_needs_synchronize",
      t_checked_synchronous_needs_synchronize, NULL },
    { "unchecked_options_skip_the_validation",
      t_unchecked_options_skip_the_validation, NULL },
    { "directory_file_opens_the_volume_root",
      t_directory_file_opens_the_volume_root, NULL },
    { "relative_name_under_root_directory",
      t_relative_name_under_root_directory, NULL },
    { "refusal_leaves_the_status_block_alone",
      t_refusal_leaves_the_status_block_alone, NULL },
    { "the_disposition_reaches_the_driver",
      t_the_disposition_reaches_the_driver, NULL },
};

DEFINE_GROUP(io_createfile, "io/createfile");
