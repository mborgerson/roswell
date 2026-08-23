/*
 * IRP completion write-backs: UserIosb and UserEvent on a
 * non-synchronous file handle.
 *
 * Regression coverage for the IopCompleteRequest usermode/SEH-arm trim:
 * the title-observable contract is that a completed IO writes the
 * caller's IO_STATUS_BLOCK and signals the caller's event.  Retail
 * probes show the failure side is NOT contract: a garbage
 * non-NULL IO_STATUS_BLOCK pointer to NtReadFile and a garbage output
 * buffer to NtQueryDirectoryFile both bugcheck retail with 0x1E
 * (KMODE_EXCEPTION_NOT_HANDLED, 0xC0000005) -- there is no SEH safety
 * net for ring-0 titles, so the kernel only owes correctness for valid
 * pointers, pinned here.
 */

#include "../harness.h"
#include <string.h>

static const char SCRATCH[] =
    "\\Device\\Harddisk0\\Partition1\\nxkrnl-api-iocomplete.tmp";

#ifndef FILE_DELETE_ON_CLOSE
#define FILE_DELETE_ON_CLOSE 0x00001000
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING ((NTSTATUS)0x00000103L)
#endif

/* Open WITHOUT FILE_SYNCHRONOUS_IO_* so completion is delivered through
 * the async path (UserIosb write + UserEvent signal in the completion
 * APC), like a title's overlapped IO with an event. */
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
                        FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE);
}

static bool t_event_async_write_read(void)
{
    HANDLE h = NULL, ev = NULL;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off = { .QuadPart = 0 };
    LARGE_INTEGER timeout = { .QuadPart = -((LONGLONG)5 * 1000 * 10000) };
    char buf_w[64], buf_r[64];
    NTSTATUS s;

    s = open_scratch(&h);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    /* Auto-reset so the second IO's wait can't be satisfied by the
     * first IO's leftover signal. */
    s = NtCreateEvent(&ev, NULL, SynchronizationEvent, FALSE);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);

    for (size_t i = 0; i < sizeof(buf_w); i++)
        buf_w[i] = (char)(0x41 + (i % 23));

    /* Async write: IOSB poisoned first so the completion write-back is
     * provably the writer. */
    memset(&iosb, 0xEE, sizeof(iosb));
    s = NtWriteFile(h, ev, NULL, NULL, &iosb, buf_w, sizeof(buf_w), &off);
    if (s != STATUS_SUCCESS && s != STATUS_PENDING)
        FAIL_AND_RETURN("NtWriteFile -> 0x%08x", (unsigned)s);
    s = NtWaitForSingleObject(ev, FALSE, &timeout);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NTSTATUS(iosb.Status, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, sizeof(buf_w));

    /* Async read back through the same completion path. */
    memset(&iosb, 0xEE, sizeof(iosb));
    memset(buf_r, 0, sizeof(buf_r));
    s = NtReadFile(h, ev, NULL, NULL, &iosb, buf_r, sizeof(buf_r), &off);
    if (s != STATUS_SUCCESS && s != STATUS_PENDING)
        FAIL_AND_RETURN("NtReadFile -> 0x%08x", (unsigned)s);
    s = NtWaitForSingleObject(ev, FALSE, &timeout);
    ASSERT_NTSTATUS(s, STATUS_SUCCESS);
    ASSERT_NTSTATUS(iosb.Status, STATUS_SUCCESS);
    ASSERT_EQ_U32(iosb.Information, sizeof(buf_r));
    if (memcmp(buf_w, buf_r, sizeof(buf_w)) != 0)
        FAIL_AND_RETURN("read-back mismatch");

    NtClose(ev);
    NtClose(h);
    return true;
}

static const test_entry_t io_complete_entries[] = {
    {"event_async_write_read", t_event_async_write_read},
};

DEFINE_GROUP(io_complete, "io/complete");
