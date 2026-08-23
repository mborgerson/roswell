/*
 * Garbage (non-NULL, unmapped) pointers into IO and wait ordinals.
 *
 * Pins the retail bad-pointer contract: the kernel NEVER converts a
 * bad-pointer fault into a returned STATUS_ACCESS_VIOLATION -- it either
 * succeeds without touching the pointer, lets the AV propagate into the
 * title's own SEH chain (ring 0, shared handler chain), or bugchecks when
 * the fault lands at DISPATCH_LEVEL.  Kernel-side SEH is load-bearing for
 * raise-based IO-error flow (CcMapData/CcPinRead raising into FSD catches),
 * not for a bad-pointer contract.
 *
 * The enabled tests below are the machine-survivable rows; the fatal
 * rows are kept compile-gated (a bugcheck aborts the whole suite) so
 * the measurements stay reproducible.
 */

#include "../harness.h"
#include <string.h>
#include <excpt.h>

static const char SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-badptr.tmp";

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef STATUS_ACCESS_VIOLATION
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xC0000005L)
#endif
#ifndef STATUS_INVALID_HANDLE
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008L)
#endif
#ifndef STATUS_DISK_FULL
#define STATUS_DISK_FULL ((NTSTATUS)0xC000007FL)
#endif

/* Unmapped on both kernels: far above any title allocation, far below
 * the physical-memory window. */
#define GARBAGE_PTR ((PVOID)0x60000000)

/* Marker: the guarded call never returned (exception reached the
 * title's handler instead). */
#define ST_TITLE_CAUGHT ((NTSTATUS)0xE0DEAD01L)

/* Fatal probes: each row bugchecks or wedges at least one of the two
 * kernels (measured results in the header).  Compile-gated so the
 * default suite stays runnable; build with -DBADPTR_FATAL_PROBES=1 and
 * pick one entry per boot to reproduce. */
#ifndef BADPTR_FATAL_PROBES
#define BADPTR_FATAL_PROBES 0
#endif

static ULONG g_caught;

static NTSTATUS open_scratch(HANDLE *h)
{
    ANSI_STRING name = {
        .Length        = (USHORT)(sizeof(SCRATCH) - 1),
        .MaximumLength = sizeof(SCRATCH),
        .Buffer        = (PCHAR)SCRATCH,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(h, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa,
                        &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0,
                        FILE_OVERWRITE_IF,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                        FILE_DELETE_ON_CLOSE);
}

static NTSTATUS seed_scratch(HANDLE h, ULONG bytes)
{
    static char buf[512];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off = { .QuadPart = 0 };
    memset(buf, 0x42, sizeof(buf));
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    return NtWriteFile(h, NULL, NULL, NULL, &iosb, buf, bytes, &off);
}

/* --- enabled: the machine-survivable contracts ------------------------ */

static bool t_wait_garbage_handle(void)
{
    /* Garbage handle VALUE (not a deref): Ob validates the handle
     * before anything touches title memory. */
    NTSTATUS s;
    g_caught = 0;
    __try {
        s = NtWaitForSingleObject((HANDLE)0x6FEED5A0, FALSE, NULL);
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
        s = ST_TITLE_CAUGHT;
    }
    ASSERT_NTSTATUS(s, STATUS_INVALID_HANDLE);
    ASSERT_EQ_U32(g_caught, 0);
    return true;
}

static bool t_read_zero_length_garbage_buffer(void)
{
    /* Zero-length IO succeeds without ever dereferencing the buffer. */
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;

    s = open_scratch(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = seed_scratch(h, 512);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("seed NtWriteFile -> 0x%08x", (unsigned)s);
    }

    g_caught = 0;
    memset(&iosb, 0xee, sizeof(iosb));
    __try {
        LARGE_INTEGER off = { .QuadPart = 0 };
        s = NtReadFile(h, NULL, NULL, NULL, &iosb, GARBAGE_PTR, 0, &off);
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
        s = ST_TITLE_CAUGHT;
    }
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, 0);
    ASSERT_EQ_U32(g_caught, 0);
    return true;
}

static bool t_write_zero_length_garbage_buffer(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;

    s = open_scratch(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    g_caught = 0;
    memset(&iosb, 0xee, sizeof(iosb));
    __try {
        LARGE_INTEGER off = { .QuadPart = 0 };
        s = NtWriteFile(h, NULL, NULL, NULL, &iosb, GARBAGE_PTR, 0, &off);
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
        s = ST_TITLE_CAUGHT;
    }
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, 0);
    ASSERT_EQ_U32(g_caught, 0);
    return true;
}

static bool t_set_eof_beyond_free_space(void)
{
    /* Extending past free space exercises the cluster-allocation error
     * flow; retail surfaces STATUS_DISK_FULL as a plain return.  On
     * this disk free space exceeds the FAT 4 GB file cap, and vfat
     * trips its size-cap check first. */
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    FILE_FS_SIZE_INFORMATION fs;
    NTSTATUS s;

    s = open_scratch(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    s = NtQueryVolumeInformationFile(h, &iosb, &fs, sizeof(fs),
                                     FileFsSizeInformation);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        FAIL_AND_RETURN("query fs size -> 0x%08x", (unsigned)s);
    }

    {
        ULONGLONG cluster = (ULONGLONG)fs.SectorsPerAllocationUnit *
                            fs.BytesPerSector;
        FILE_END_OF_FILE_INFORMATION eof;
        eof.EndOfFile.QuadPart =
            ((ULONGLONG)fs.AvailableAllocationUnits.QuadPart + 16) * cluster;
        s = NtSetInformationFile(h, &iosb, &eof, sizeof(eof),
                                 FileEndOfFileInformation);
    }
    NtClose(h);

    ASSERT_NTSTATUS(s, STATUS_DISK_FULL);
    return true;
}

/* --- compile-gated: fatal probes (measured results in the header) ----- */

#if BADPTR_FATAL_PROBES

static bool t_fatal_read_garbage_buffer(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;

    s = open_scratch(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    s = seed_scratch(h, 512);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    g_caught = 0;
    __try {
        LARGE_INTEGER off = { .QuadPart = 0 };
        s = NtReadFile(h, NULL, NULL, NULL, &iosb, GARBAGE_PTR, 512, &off);
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
        s = ST_TITLE_CAUGHT;
    }
    tap_comment("badptr: FATAL read(garbage buf) -> 0x%08x caught=0x%08x",
                (unsigned)s, (unsigned)g_caught);
    NtClose(h);
    return true;
}

static bool t_fatal_write_garbage_buffer(void)
{
    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;

    s = open_scratch(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    g_caught = 0;
    __try {
        LARGE_INTEGER off = { .QuadPart = 0 };
        s = NtWriteFile(h, NULL, NULL, NULL, &iosb, GARBAGE_PTR, 512, &off);
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
        s = ST_TITLE_CAUGHT;
    }
    tap_comment("badptr: FATAL write(garbage buf) -> 0x%08x caught=0x%08x",
                (unsigned)s, (unsigned)g_caught);
    NtClose(h);
    return true;
}

static bool t_fatal_querydir_garbage_buffer(void)
{
    /* Both the full-size and undersized-Length shapes belong here:
     * retail touches the output buffer before validating Length, the
     * AV propagates into the title's handler, and the FCB stays
     * wedged (the NtClose below never returns). */
    static const char ROOT[] = "\\Device\\Harddisk0\\Partition1\\";
    ANSI_STRING name = {
        .Length        = (USHORT)(sizeof(ROOT) - 1),
        .MaximumLength = sizeof(ROOT),
        .Buffer        = (PCHAR)ROOT,
    };
    OBJECT_ATTRIBUTES oa = {
        .RootDirectory = NULL,
        .ObjectName    = &name,
        .Attributes    = OBJ_CASE_INSENSITIVE,
    };
    IO_STATUS_BLOCK iosb;
    HANDLE dir;
    NTSTATUS s;

    s = NtCreateFile(&dir, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                     FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                     FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    g_caught = 0;
    __try {
        s = NtQueryDirectoryFile(dir, NULL, NULL, NULL, &iosb, GARBAGE_PTR,
                                 sizeof(FILE_DIRECTORY_INFORMATION) + 256,
                                 FileDirectoryInformation, NULL, TRUE);
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
        s = ST_TITLE_CAUGHT;
    }
    tap_comment("badptr: FATAL querydir(garbage buf) -> 0x%08x caught=0x%08x",
                (unsigned)s, (unsigned)g_caught);
    NtClose(dir);
    return true;
}

static bool t_fatal_wait_garbage_timeout_signaled(void)
{
    /* Retail returns SUCCESS here (the timeout of an immediately
     * satisfiable wait is never read); our wait path reads it eagerly
     * under the dispatcher lock and bugchecks. */
    HANDLE ev;
    NTSTATUS s;

    s = NtCreateEvent(&ev, NULL, NotificationEvent, TRUE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    g_caught = 0;
    __try {
        s = NtWaitForSingleObject(ev, FALSE, (PLARGE_INTEGER)GARBAGE_PTR);
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
        s = ST_TITLE_CAUGHT;
    }
    tap_comment("badptr: FATAL wait(signaled, garbage timeout) -> 0x%08x caught=0x%08x",
                (unsigned)s, (unsigned)g_caught);
    NtClose(ev);
    return true;
}

static bool t_fatal_wait_garbage_timeout_nonsignaled(void)
{
    HANDLE ev;
    NTSTATUS s;

    s = NtCreateEvent(&ev, NULL, NotificationEvent, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    g_caught = 0;
    __try {
        s = NtWaitForSingleObject(ev, FALSE, (PLARGE_INTEGER)GARBAGE_PTR);
    } __except (g_caught = (ULONG)_exception_code(),
                EXCEPTION_EXECUTE_HANDLER) {
        s = ST_TITLE_CAUGHT;
    }
    tap_comment("badptr: FATAL wait(nonsignaled, garbage timeout) -> 0x%08x caught=0x%08x",
                (unsigned)s, (unsigned)g_caught);
    NtClose(ev);
    return true;
}

#endif /* BADPTR_FATAL_PROBES */

static const test_entry_t io_bad_ptr_entries[] = {
    {"wait_garbage_handle",              t_wait_garbage_handle},
    {"read_zero_length_garbage_buffer",  t_read_zero_length_garbage_buffer},
    {"write_zero_length_garbage_buffer", t_write_zero_length_garbage_buffer},
    {"set_eof_beyond_free_space",        t_set_eof_beyond_free_space,
     "vfat trips its FAT 4 GB size cap (C000000D) before the free-space "
     "check; retail FATX returns STATUS_DISK_FULL"},
#if BADPTR_FATAL_PROBES
    {"FATAL_read_garbage_buffer",        t_fatal_read_garbage_buffer},
    {"FATAL_write_garbage_buffer",       t_fatal_write_garbage_buffer},
    {"FATAL_querydir_garbage_buffer",    t_fatal_querydir_garbage_buffer},
    {"FATAL_wait_garbage_timeout_signaled",
     t_fatal_wait_garbage_timeout_signaled},
    {"FATAL_wait_garbage_timeout_nonsignaled",
     t_fatal_wait_garbage_timeout_nonsignaled},
#endif
};

DEFINE_GROUP(io_bad_ptr, "io/bad-ptr");
